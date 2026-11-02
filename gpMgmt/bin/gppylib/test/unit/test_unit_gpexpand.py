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

    #
    # tests for validate_icproxy_addr()
    #
    ICPROXY_MESH = "1:-1:cdw:35432,2:0:sdw1:36000,3:1:sdw2:36001,4:0:sdw2:37000,5:1:sdw1:37001"
    # (dbid, content, role, address, port) as returned by the gp_segment_configuration query
    ICPROXY_SEG_ROWS = [(1, -1, 'p', 'cdw', 5432),
                        (2, 0, 'p', 'sdw1', 40000), (3, 1, 'p', 'sdw2', 40001),
                        (4, 0, 'm', 'sdw2', 50000), (5, 1, 'm', 'sdw1', 50001)]

    def _icproxy_subject(self, mesh, ic_type='proxy', seg_rows=None, configured=None):
        """
        A gpexpand instance whose catalog queries answer with the given mesh
        (the running value), interconnect type and segment rows.  `configured`
        is what pg_file_settings holds; None means the files do not set it.
        """
        if seg_rows is None:
            seg_rows = self.ICPROXY_SEG_ROWS
        cursor = Mock()
        cursor.fetchall.return_value = seg_rows
        patches = [patch('gpexpand.dbconn.queryRow', side_effect=[[mesh], [ic_type]]),
                   patch('gpexpand.dbconn.query', return_value=cursor),
                   patch('gpexpand.subprocess.check_call'),
                   patch('gpexpand.WorkerPool'),
                   patch('gpexpand.dbconn.queryFileSetting', return_value=configured)]
        mocks = [p.start() for p in patches]
        for p in patches:
            self.addCleanup(p.stop)
        self.check_call_mock = mocks[2]
        return self.subject.gpexpand(self.subject.logger, self.gparray, Mock(), self.options)

    def _new_seg(self, dbid, content, host, port, role):
        return self.subject.NewSegmentInput(host, host, port, '/data/seg%s' % dbid,
                                            str(dbid), str(content), role)

    def test_validate_icproxy_addr_derives_ports_per_role(self):
        expand = self._icproxy_subject(self.ICPROXY_MESH)
        new_segs = [self._new_seg(6, 2, 'sdw3', 40002, 'p'),
                    self._new_seg(7, 2, 'sdw1', 50002, 'm')]

        expand.validate_icproxy_addr(new_segs)

        # primary: 36000 + (40002 - 40000); mirror: 37000 + (50002 - 50000)
        self.check_call_mock.assert_called_once_with(
            ['gpconfig', '-c', 'gp_interconnect_proxy_addresses', '-v',
             self.ICPROXY_MESH + ",6:2:sdw3:36002,7:2:sdw1:37002"])

    def test_validate_icproxy_addr_extends_the_configured_value_not_the_running_one(self):
        # gpinitstandby wrote the standby's entry (dbid 8) but nothing reloaded
        # the configuration yet: the running value does not have it
        configured = self.ICPROXY_MESH + ",8:-1:scdw:35432"
        expand = self._icproxy_subject(self.ICPROXY_MESH, configured=configured)

        expand.validate_icproxy_addr([self._new_seg(6, 2, 'sdw3', 40002, 'p')])

        self.check_call_mock.assert_called_once_with(
            ['gpconfig', '-c', 'gp_interconnect_proxy_addresses', '-v',
             self.ICPROXY_MESH + ",6:2:sdw3:36002,8:-1:scdw:35432"])

    def test_validate_icproxy_addr_is_a_noop_when_the_files_unset_the_mesh(self):
        # the running value is stale, the files say the mesh is gone
        expand = self._icproxy_subject(self.ICPROXY_MESH, configured="")

        expand.validate_icproxy_addr([self._new_seg(6, 2, 'sdw3', 40002, 'p')])

        self.check_call_mock.assert_not_called()

    def test_validate_icproxy_addr_fails_on_a_broken_configured_value(self):
        expand = self._icproxy_subject(self.ICPROXY_MESH)
        self.subject.dbconn.queryFileSetting.side_effect = Exception(
            'gp_interconnect_proxy_addresses is set in the configuration files but cannot be applied: x')

        with self.assertRaises(Exception) as ex:
            expand.validate_icproxy_addr([self._new_seg(6, 2, 'sdw3', 40002, 'p')])

        self.assertIn("cannot be applied", str(ex.exception))
        self.check_call_mock.assert_not_called()

    def test_validate_icproxy_addr_accepts_blanks_and_a_trailing_comma(self):
        mesh = " 1:-1:cdw:35432, 2:0:sdw1:36000 ,3:1:sdw2:36001,4:0:sdw2:37000,5:1:sdw1:37001,"
        expand = self._icproxy_subject(mesh)

        expand.validate_icproxy_addr([self._new_seg(6, 2, 'sdw3', 40002, 'p')])

        self.check_call_mock.assert_called_once_with(
            ['gpconfig', '-c', 'gp_interconnect_proxy_addresses', '-v',
             self.ICPROXY_MESH + ",6:2:sdw3:36002"])

    def test_validate_icproxy_addr_updates_mesh_even_when_not_in_proxy_mode(self):
        expand = self._icproxy_subject(self.ICPROXY_MESH, ic_type='udpifc')

        expand.validate_icproxy_addr([self._new_seg(6, 2, 'sdw3', 40002, 'p')])

        self.check_call_mock.assert_called_once()

    def test_validate_icproxy_addr_is_a_noop_without_a_mesh(self):
        expand = self._icproxy_subject("")

        expand.validate_icproxy_addr([self._new_seg(6, 2, 'sdw3', 40002, 'p')])

        self.check_call_mock.assert_not_called()

    def test_validate_icproxy_addr_keeps_existing_entries(self):
        mesh = self.ICPROXY_MESH + ",6:2:sdw3:16502"
        expand = self._icproxy_subject(mesh)

        expand.validate_icproxy_addr([self._new_seg(6, 2, 'sdw3', 40002, 'p')])

        self.check_call_mock.assert_not_called()

    def test_validate_icproxy_addr_rejects_a_colliding_derived_port(self):
        expand = self._icproxy_subject(self.ICPROXY_MESH)
        # 36000 + (40001 - 40000) = 36001 on sdw2 is already dbid 3's proxy port
        new_segs = [self._new_seg(6, 2, 'sdw2', 40001, 'p')]

        with self.assertRaises(Exception) as ex:
            expand.validate_icproxy_addr(new_segs)

        self.assertEqual("Checking ICProxy addresses failed", str(ex.exception))
        self.check_call_mock.assert_not_called()

    def test_validate_icproxy_addr_rejects_a_derived_port_used_by_a_postmaster(self):
        # primary base 36000 / 40000: a new primary on sdw2 with port 44001 derives
        # 40001, which is dbid 3's postmaster port on that host
        expand = self._icproxy_subject(self.ICPROXY_MESH)

        with self.assertRaises(Exception) as ex:
            expand.validate_icproxy_addr([self._new_seg(6, 2, 'sdw2', 44001, 'p')])

        self.assertEqual("Checking ICProxy addresses failed", str(ex.exception))
        self.check_call_mock.assert_not_called()

    def test_validate_icproxy_addr_rejects_a_derived_port_used_by_a_new_postmaster(self):
        # two new primaries on sdw3: dbid 6 (port 40002) derives proxy port 36002,
        # which is the postmaster port the input file gives dbid 7 on that host
        expand = self._icproxy_subject(self.ICPROXY_MESH)
        new_segs = [self._new_seg(6, 2, 'sdw3', 40002, 'p'),
                    self._new_seg(7, 3, 'sdw3', 36002, 'p')]

        with self.assertRaises(Exception) as ex:
            expand.validate_icproxy_addr(new_segs)

        self.assertEqual("Checking ICProxy addresses failed", str(ex.exception))
        self.check_call_mock.assert_not_called()

    def test_validate_icproxy_addr_rejects_a_role_absent_from_the_mesh(self):
        # a mesh without mirrors cannot tell us where the new mirror's port goes
        mesh = "1:-1:cdw:35432,2:0:sdw1:36000,3:1:sdw2:36001"
        expand = self._icproxy_subject(mesh, seg_rows=[(1, -1, 'p', 'cdw', 5432),
                                                       (2, 0, 'p', 'sdw1', 40000),
                                                       (3, 1, 'p', 'sdw2', 40001)])

        with self.assertRaises(Exception) as ex:
            expand.validate_icproxy_addr([self._new_seg(6, 2, 'sdw1', 50002, 'm')])

        self.assertEqual("Checking ICProxy addresses failed", str(ex.exception))
        self.check_call_mock.assert_not_called()

    def test_validate_icproxy_addr_gpconfig_failure_is_fatal_in_proxy_mode(self):
        expand = self._icproxy_subject(self.ICPROXY_MESH, ic_type='proxy')
        self.check_call_mock.side_effect = self.subject.subprocess.CalledProcessError(1, 'gpconfig')

        with self.assertRaises(Exception) as ex:
            expand.validate_icproxy_addr([self._new_seg(6, 2, 'sdw3', 40002, 'p')])

        self.assertEqual("Updating ICProxy addresses failed", str(ex.exception))

    def test_validate_icproxy_addr_gpconfig_failure_is_a_warning_otherwise(self):
        expand = self._icproxy_subject(self.ICPROXY_MESH, ic_type='udpifc')
        self.check_call_mock.side_effect = self.subject.subprocess.CalledProcessError(1, 'gpconfig')

        expand.validate_icproxy_addr([self._new_seg(6, 2, 'sdw3', 40002, 'p')])

        self.subject.logger.warning.assert_called()

    #
    # end tests for validate_icproxy_addr()
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
