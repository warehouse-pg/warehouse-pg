import imp
import os
import shutil
import tempfile
from optparse import Values

from .gp_unittest import GpTestCase, run_tests
from mock import MagicMock, patch


class GpInitStandbyPgHbaTestCase(GpTestCase):
    def setUp(self):
        gpinitstandby_file = os.path.abspath(
            os.path.dirname(__file__) + '/../../../gpinitstandby')
        self.subject = imp.load_source('gpinitstandby', gpinitstandby_file)

        self.tmpDir = tempfile.mkdtemp()
        self.pg_hba_path = os.path.join(self.tmpDir, 'pg_hba.conf')
        with open(self.pg_hba_path, 'w') as f:
            f.write('# comment\nhost all all 127.0.0.1/32 trust\n')

        self.mock_array = MagicMock()
        self.mock_array.coordinator.getSegmentDataDirectory.return_value = self.tmpDir
        self.mock_array.coordinator.getSegmentHostName.return_value = 'cdw'

        self.mock_options = Values()
        setattr(self.mock_options, 'standby_host', 'scdw')
        setattr(self.mock_options, 'hba_hostnames', False)

        self.apply_patches([
            patch('gpinitstandby.gp.IfAddrs.list_addrs', return_value=['192.168.1.2']),
            patch('gpinitstandby.unix.UserId.local', return_value='gpadmin'),
            patch('gpinitstandby.pg.ReloadDbConf.local'),
        ])

    def tearDown(self):
        shutil.rmtree(self.tmpDir)
        super(GpInitStandbyPgHbaTestCase, self).tearDown()

    def test_update_pg_hba_conf_creates_backup(self):
        self.subject.update_pg_hba_conf(self.mock_options, self.mock_array)

        backup_path = os.path.join(self.tmpDir, self.subject.PG_HBA_BACKUP)
        self.assertTrue(os.path.exists(backup_path),
                        'backup file should exist after update_pg_hba_conf')

    def test_undo_update_pg_hba_conf_restores_backup(self):
        self.subject.update_pg_hba_conf(self.mock_options, self.mock_array)

        original_content = open(os.path.join(self.tmpDir, self.subject.PG_HBA_BACKUP)).read()

        self.subject.undo_update_pg_hba_conf(self.mock_array)

        restored_content = open(self.pg_hba_path).read()
        self.assertEqual(original_content, restored_content,
                         'pg_hba.conf should be restored to original content')
        self.assertFalse(
            os.path.exists(os.path.join(self.tmpDir, self.subject.PG_HBA_BACKUP)),
            'backup file should be removed after undo_update_pg_hba_conf')

    def test_update_pg_hba_conf_safe_with_path_containing_spaces(self):
        # shutil.copy/move must handle paths with spaces without invoking a shell
        spaced_dir = os.path.join(self.tmpDir, 'dir with spaces')
        os.makedirs(spaced_dir)
        pg_hba = os.path.join(spaced_dir, 'pg_hba.conf')
        with open(pg_hba, 'w') as f:
            f.write('# comment\nhost all all 127.0.0.1/32 trust\n')
        self.mock_array.coordinator.getSegmentDataDirectory.return_value = spaced_dir

        self.subject.update_pg_hba_conf(self.mock_options, self.mock_array)

        backup_path = os.path.join(spaced_dir, self.subject.PG_HBA_BACKUP)
        self.assertTrue(os.path.exists(backup_path))

        self.subject.undo_update_pg_hba_conf(self.mock_array)
        self.assertTrue(os.path.exists(pg_hba))
        self.assertFalse(os.path.exists(backup_path))


if __name__ == '__main__':
    run_tests()
