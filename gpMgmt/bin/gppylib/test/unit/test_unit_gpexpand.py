import os
import imp

from .gp_unittest import *
from mock import *
from gparray import Segment, GpArray
from gppylib.db.dbconn import DbURL
from gppylib.db import catalog
from gppylib.gplog import *
from gppylib.system.configurationInterface import GpConfigurationProvider
from gppylib.system.environment import GpCoordinatorEnvironment
from gppylib.db import dbconn
import io
import sys

class GpExpand(GpTestCase):
    def setUp(self):
        # because gpexpand does not have a .py extension,
        # we have to use imp to import it
        # if we had a gpexpand.py, this is equivalent to:
        #   import gpexpand
        #   self.subject = gpexpand
        gpexpand_file = os.path.abspath(os.path.dirname(__file__) + "/../../../gpexpand")
        self.subject = imp.load_source('gpexpand', gpexpand_file)
        self.old_sys_argv = sys.argv
        sys.argv = []  # We need to do this otherwise, the parser will read the command line as the default arguments.
        self.options, self.args, self.parser = self.subject.parseargs()

        self.subject.logger = Mock(spec=['log', 'warn', 'info', 'debug', 'error', 'warning', 'fatal', 'exception'])

        # All we're doing here is checking files using open, which doesn't seem worth the effort to fully mock out
        self.subject.is_gpexpand_running = Mock(return_value=False)

        self.gparray = self.createGpArrayWith2Primary2Mirrors()
        self.apply_patches([
            patch('gpexpand.GpArray.initFromCatalog', return_value=self.gparray),
            patch('builtins.open', mock_open(), create=True),
            # with open() mocked, gplog would open its log file on a mock and
            # leave that handler on the logger for every test that follows
            patch('gpexpand.setup_tool_logging'),
            patch('builtins.input'),
            patch('gpexpand.copy.deepcopy', return_value=Mock()),
            patch('gpexpand.dbconn.query', return_value=FakeCursor()),
            patch('gpexpand.GpExpandStatus', return_value=Mock()),
            patch('gpexpand.GpExpandStatus.return_value.get_current_status', return_value=(None, None)),
            patch('gpexpand.GpStart.local', return_value=Mock()),
            patch('gpexpand.GpStop.local', return_value=Mock()),
            patch('gpexpand.chk_local_db_running', return_value=(True, True, True, True, True)),
            patch('gpexpand.get_local_db_mode', return_value='NORMAL'),
            patch('gpexpand.dbconn.DbURL', return_value=Mock(), spec=DbURL),
            patch('gpexpand.dbconn.connect', return_value=Mock()),
            patch('gpexpand.GpCoordinatorEnvironment', return_value=Mock(), spec=GpCoordinatorEnvironment),
            patch('gpexpand.configurationInterface.getConfigurationProvider'),
            patch('os.path.exists', return_value=Mock()),
            patch('gpexpand.get_default_logger', return_value=self.subject.logger),
            patch('gpexpand.HeapChecksum'),
        ])
        self.input_mock = self.get_mock_from_apply_patch("input")
        self.getConfigProviderFunctionMock = self.get_mock_from_apply_patch("getConfigurationProvider")
        self.gpCoordinatorEnvironmentMock = self.get_mock_from_apply_patch("GpCoordinatorEnvironment")
        self.previous_coordinator_data_directory = os.getenv('COORDINATOR_DATA_DIRECTORY', '')
        os.environ["COORDINATOR_DATA_DIRECTORY"] = '/tmp/dirdoesnotexist'
        configProviderMock = Mock(spec=GpConfigurationProvider)
        self.getConfigProviderFunctionMock.return_value = configProviderMock
        configProviderMock.initializeProvider.return_value = configProviderMock
        self.gpCoordinatorEnvironmentMock.return_value.getCoordinatorPort.return_value = 123456
        self.mock_heap_checksum = self.get_mock_from_apply_patch('HeapChecksum')
        self.mock_heap_checksum.return_value.get_coordinator_value.return_value = 1
        self.mock_heap_checksum.return_value.get_segments_checksum_settings.return_value = ([1], [1])
        self.mock_heap_checksum.return_value.are_segments_consistent.return_value = True
        self.mock_heap_checksum.return_value.check_segment_consistency.return_value = ([2], [], 1)

    def tearDown(self):
        os.environ['COORDINATOR_DATA_DIRECTORY'] = self.previous_coordinator_data_directory
        sys.argv = self.old_sys_argv
        super(GpExpand, self).tearDown()

    def _make_hostname_cmd(self, remote_host, reported_name):
        cmd = Mock()
        cmd.remoteHost = remote_host
        cmd.was_successful.return_value = True
        cmd.get_hostname.return_value = reported_name
        return cmd

    def test_consolidate_hosts_returns_configured_names_not_reported_fqdn(self):
        pool = Mock()
        pool.getCompletedItems.return_value = [
            self._make_hostname_cmd('sdw1', 'sdw1.internal.example.com'),
            self._make_hostname_cmd('sdw2', 'sdw2.internal.example.com'),
        ]

        hosts = self.subject.SegmentTemplate.consolidate_hosts(pool, ['sdw1', 'sdw2', 'sdw1'])

        self.assertEqual(sorted(hosts), ['sdw1', 'sdw2'])
        # each configured name is only probed once
        self.assertEqual(pool.addCommand.call_count, 2)

    def test_consolidate_hosts_dedups_names_of_the_same_machine(self):
        pool = Mock()
        pool.getCompletedItems.return_value = [
            self._make_hostname_cmd('sdw1-1', 'sdw1.internal.example.com'),
            self._make_hostname_cmd('sdw1-2', 'sdw1.internal.example.com'),
            self._make_hostname_cmd('sdw2', 'sdw2.internal.example.com'),
        ]

        hosts = self.subject.SegmentTemplate.consolidate_hosts(pool, ['sdw1-1', 'sdw1-2', 'sdw2'])

        self.assertEqual(len(hosts), 2)
        self.assertIn('sdw2', hosts)
        self.assertEqual(len(set(hosts) & {'sdw1-1', 'sdw1-2'}), 1)

    def test_consolidate_hosts_keeps_hosts_with_empty_hostname(self):
        pool = Mock()
        pool.getCompletedItems.return_value = [
            self._make_hostname_cmd('sdw1', ''),
            self._make_hostname_cmd('sdw2', ''),
        ]

        hosts = self.subject.SegmentTemplate.consolidate_hosts(pool, ['sdw1', 'sdw2'])

        self.assertEqual(sorted(hosts), ['sdw1', 'sdw2'])

    def test_consolidate_hosts_keeps_host_with_empty_hostname_named_like_reported_one(self):
        # sdw1 reports no hostname, and sdw2 reports "sdw1": two machines
        cmds = [
            self._make_hostname_cmd('sdw1', ''),
            self._make_hostname_cmd('sdw2', 'sdw1'),
        ]

        for completed in (cmds, cmds[::-1]):
            pool = Mock()
            pool.getCompletedItems.return_value = completed

            hosts = self.subject.SegmentTemplate.consolidate_hosts(pool, ['sdw1', 'sdw2'])

            self.assertEqual(sorted(hosts), ['sdw1', 'sdw2'])

    def test_consolidate_hosts_raises_when_hostname_fails(self):
        pool = Mock()
        failed = self._make_hostname_cmd('sdw1', '')
        failed.was_successful.return_value = False
        pool.getCompletedItems.return_value = [failed]

        with self.assertRaises(self.subject.SegmentTemplateError):
            self.subject.SegmentTemplate.consolidate_hosts(pool, ['sdw1'])

    @patch('gpexpand.is_cluster_up_and_balanced', return_value=True)
    def test_validate_heap_checksums_aborts_when_cluster_inconsistent(self, mock1):
        self.options.filename = '/tmp/doesnotexist' # Replacement of the sys.argv

        self.mock_heap_checksum.return_value.get_segments_checksum_settings.return_value = ([1], [0])
        self.mock_heap_checksum.return_value.are_segments_consistent.return_value = False
        self.primary0.heap_checksum = 1
        self.primary1.heap_checksum = 0
        self.coordinator.heap_checksum = '1'
        self.mock_heap_checksum.return_value.check_segment_consistency.return_value = (
            [self.primary0], [self.primary1], self.coordinator.heap_checksum)

        with self.assertRaises(SystemExit):
            self.subject.main(self.options, self.args, self.parser)

        self.subject.logger.fatal.assert_any_call("Cluster heap checksum setting differences reported")
        self.subject.logger.fatal.assert_any_call("Heap checksum settings on 1 of 2 segment instances "
                                                          "do not match coordinator <<<<<<<<")
        self.subject.logger.error.assert_called_with("gpexpand failed: Segments have heap_checksum set "
                                                             "inconsistently to coordinator \n\nExiting...")

    @patch('gpexpand.FileDirExists.return_value.filedir_exists', return_value=True)
    @patch('gpexpand.FileDirExists', return_value=Mock())
    @patch('gpexpand.is_cluster_up_and_balanced', return_value=True)
    def test_validate_heap_checksums_completes_when_cluster_consistent(self, mock1, mock2, mock3):
        """
        If all the segment checksums match the checksum at the coordinator, then the cluster is consistent.
        This is essentially making sure that the validate_heap_checksums() internal method has not detected any
        inconisistency.  However, since we are mocking all the dependencies, the actual message from gpexpand would be a
        failure from the method after validate_heap_checksums().
        """
        self.options.filename = '/tmp/doesnotexist' # Replacement of the sys.argv
        with self.assertRaises(SystemExit):
            self.subject.main(self.options, self.args, self.parser)
        self.subject.logger.info.assert_any_call("Heap checksum setting consistent across cluster")
        self.subject.logger.error.assert_called_with("gpexpand failed: Invalid input file: No expansion "
                                                                  "segments defined \n\nExiting...")

    #
    # unit tests for interview_setup()
    #
    def test_user_aborts(self):
        self.input_mock.return_value = "N"
        with self.assertRaises(SystemExit):
            self.subject.interview_setup(self.gparray, self.options)
        self.subject.logger.info.assert_any_call("User Aborted. Exiting...")

    def test_nonstandard_gpArray_user_aborts(self):
        self.input_mock.side_effect = ["Y", "N"]
        self.gparray.isStandardArray = Mock(return_value=(False, ""))
        with patch('sys.stdout', new=io.StringIO()) as mock_stdout:
            with self.assertRaises(SystemExit):
                self.subject.interview_setup(self.gparray, self.options)
            self.assertIn('The current system appears to be non-standard.', mock_stdout.getvalue())

        self.subject.logger.info.assert_any_call("User Aborted. Exiting...")

    @patch('gppylib.db.dbconn.querySingleton', return_value=0)
    def test_unit_cluster_up_and_balanced_true(self,mock1):
        expected = True
        actual = self.subject.is_cluster_up_and_balanced(dbconn.DbURL())
        self.assertEqual(actual, expected)

    @patch('gppylib.db.dbconn.querySingleton', return_value=2)
    def test_unit_cluster_up_and_balanced_false(self, mock):
        expected = False
        actual = self.subject.is_cluster_up_and_balanced(dbconn.DbURL())
        self.assertEqual(actual, expected)

    @patch('gppylib.db.dbconn.querySingleton', side_effect=Exception())
    def test_unit_cluster_up_and_balanced_exception(self, mock1):
        with self.assertRaises(Exception) as ex:
            self.subject.is_cluster_up_and_balanced(dbconn.DbURL())

        self.assertTrue('failed to query cluster role check' in str(ex.exception))

    @patch('gppylib.db.dbconn.connect', side_effect=Exception())
    def test_unit_cluster_up_and_balanced_conn_exception(self, mock1):
        with self.assertRaises(Exception) as ex:
            self.subject.is_cluster_up_and_balanced(dbconn.DbURL())

        self.assertTrue('failed to query cluster role check' in str(ex.exception))

    #
    # end tests for interview_setup()
    #

    def createGpArrayWith2Primary2Mirrors(self):
        self.coordinator = Segment.initFromString(
            "1|-1|p|p|s|u|cdw|cdw|5432|/data/coordinator")
        self.primary0 = Segment.initFromString(
            "2|0|p|p|s|u|aspen|sdw1|40000|/Users/pivotal/workspace/gpdb/gpAux/gpdemo/datadirs/qddir/demoDataDir-1")
        self.primary1 = Segment.initFromString(
            "3|1|p|p|s|u|sdw2|sdw2|40001|/data/primary1")
        self.mirror0 = Segment.initFromString(
            "4|0|m|m|s|u|sdw2|sdw2|50000|/data/mirror0")
        self.mirror1 = Segment.initFromString(
            "5|1|m|m|s|u|sdw1|sdw1|50001|/data/mirror1")
        return GpArray([self.coordinator, self.primary0, self.primary1, self.mirror0, self.mirror1])

if __name__ == '__main__':
    run_tests()
