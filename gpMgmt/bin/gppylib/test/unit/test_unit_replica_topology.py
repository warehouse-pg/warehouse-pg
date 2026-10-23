#!/usr/bin/env python3

import os
import re
import shutil
import tempfile

from gppylib import pgconf, replica_topology
from gppylib.replica_topology import ReplicaTopologyError
from gppylib.test.unit.gp_unittest import GpTestCase, run_tests

REPLICA_CONF = """# whpg_replica.conf — managed by `whpg-dr replica`. DO NOT EDIT.
# cluster=prod generation=3 written=2026-10-23T00:00:00Z
port = 6000
hot_standby = on
restore_command = 'barman-wal-restore -U gpadmin rec prod-seg-1 %f %p'
gp_pause_on_restore_point_replay = '20261023-000000R'
whpg_hot_standby_anchor_name = '20261023-000000R'
whpg_dispatch_topology_file = 'whpg_dr_topology'
"""

TOPOLOGY = """# whpg-dr dispatch topology
# generation 3
-1 1 cdw-rec cdw-rec 6000 /data/coordinator/gpseg-1

0 2 sdw1-rec sdw1-rec 6002 /data/primary/gpseg0
1\t3\tsdw2-rec\tsdw2-rec.example\t6003\t/data/primary/gpseg1
"""


class ReplicaTopologyTestCase(GpTestCase):
    def setUp(self):
        self.datadir = tempfile.mkdtemp()

    def tearDown(self):
        shutil.rmtree(self.datadir)
        super(ReplicaTopologyTestCase, self).tearDown()

    def _write(self, name, content):
        path = os.path.join(self.datadir, name)
        with open(path, 'w') as fp:
            fp.write(content)
        return path

    def test_not_a_replica_without_the_file(self):
        self.assertIsNone(replica_topology.read_replica_conf(self.datadir))

    def test_not_a_replica_without_the_topology_setting(self):
        self._write('whpg_replica.conf', REPLICA_CONF.replace("whpg_dispatch_topology_file = 'whpg_dr_topology'\n", ''))
        self.assertIsNone(replica_topology.read_replica_conf(self.datadir))
        self._write('whpg_replica.conf', REPLICA_CONF.replace("'whpg_dr_topology'", "''"))
        self.assertIsNone(replica_topology.read_replica_conf(self.datadir))

    def test_replica_conf(self):
        self._write('whpg_replica.conf', REPLICA_CONF)
        rc = replica_topology.read_replica_conf(self.datadir)
        self.assertEqual(rc.cluster, 'prod')
        self.assertEqual(rc.generation, 3)
        self.assertEqual(rc.topology_path(), os.path.join(self.datadir, 'whpg_dr_topology'))
        self.assertEqual(rc.managed_settings(), {
            'port', 'hot_standby', 'restore_command', 'gp_pause_on_restore_point_replay',
            'whpg_hot_standby_anchor_name', 'whpg_dispatch_topology_file'})

    def test_absolute_topology_path(self):
        self._write('whpg_replica.conf', REPLICA_CONF.replace("'whpg_dr_topology'", "'/elsewhere/topo'"))
        self.assertEqual(replica_topology.read_replica_conf(self.datadir).topology_path(), '/elsewhere/topo')

    def test_enable_completed_needs_the_mark_of_this_generation(self):
        self._write('whpg_replica.conf', REPLICA_CONF)
        self.assertFalse(replica_topology.read_replica_conf(self.datadir).enable_completed())

        self._write('whpg_replica.conf', REPLICA_CONF + '# whpg-dr: enable completed generation=3\n')
        self.assertTrue(replica_topology.read_replica_conf(self.datadir).enable_completed())

        # a mark left from an earlier generation does not count
        self._write('whpg_replica.conf', REPLICA_CONF + '# whpg-dr: enable completed generation=2\n')
        self.assertFalse(replica_topology.read_replica_conf(self.datadir).enable_completed())

        # nor does a mark without the managed header
        self._write('whpg_replica.conf', REPLICA_CONF.replace('# cluster=prod generation=3 written=2026-10-23T00:00:00Z\n', '')
                    + '# whpg-dr: enable completed generation=3\n')
        self.assertFalse(replica_topology.read_replica_conf(self.datadir).enable_completed())

    def test_gparray_from_replica(self):
        self._write('whpg_replica.conf', REPLICA_CONF)
        self._write('whpg_dr_topology', TOPOLOGY)
        array = replica_topology.gparray_from_replica(replica_topology.read_replica_conf(self.datadir))

        self.assertEqual(array.coordinator.getSegmentHostName(), 'cdw-rec')
        self.assertEqual(array.coordinator.getSegmentPort(), 6000)
        self.assertEqual(array.coordinator.getSegmentDataDirectory(), '/data/coordinator/gpseg-1')
        self.assertIsNone(array.standbyCoordinator)
        self.assertFalse(array.hasMirrors)
        segs = array.getSegDbList()
        self.assertEqual([(s.getSegmentContentId(), s.getSegmentDbId(), s.getSegmentHostName(),
                           s.getSegmentAddress(), s.getSegmentPort(), s.getSegmentDataDirectory())
                          for s in segs],
                         [(0, 2, 'sdw1-rec', 'sdw1-rec', 6002, '/data/primary/gpseg0'),
                          (1, 3, 'sdw2-rec', 'sdw2-rec.example', 6003, '/data/primary/gpseg1')])
        self.assertTrue(all(s.isSegmentPrimary(current_role=True) and s.isSegmentUp() for s in segs))

    def test_missing_topology_file(self):
        self._write('whpg_replica.conf', REPLICA_CONF)
        with self.assertRaisesRegex(ReplicaTopologyError, 'cannot read the replica topology file'):
            replica_topology.gparray_from_replica(replica_topology.read_replica_conf(self.datadir))

    def test_parse_topology_refusals(self):
        coordinator = '-1 1 h h 6000 /d/-1\n'
        cases = [
            (coordinator + '0 2 h h 6002\n', 'line 2: expected 6 fields'),
            (coordinator + '0 2 h h 6002 /d/0 extra\n', 'line 2: expected 6 fields'),
            (coordinator + 'x 2 h h 6002 /d/0\n', 'line 2: content, dbid and port must be integers'),
            (coordinator + '-2 2 h h 6002 /d/0\n', 'line 2: invalid content -2'),
            (coordinator + '0 0 h h 6002 /d/0\n', 'line 2: invalid dbid 0'),
            (coordinator + '0 2 h h 70000 /d/0\n', 'line 2: invalid port 70000'),
            (coordinator + '-1 2 h h 6002 /d/0\n', 'line 2: duplicate entry for content -1'),
            (coordinator + '0 1 h h 6002 /d/0\n', 'line 2: duplicate dbid 1'),
            ('0 2 h h 6002 /d/0\n', 'missing coordinator row'),
            (coordinator + '40000 2 h h 6002 /d/0\n', 'line 2: invalid content 40000'),
            (coordinator + '0 40000 h h 6002 /d/0\n', 'line 2: invalid dbid 40000'),
            (coordinator + '0 2 %s h 6002 /d/0\n' % ('h' * 65), 'line 2: hostname or address exceeds 64 characters'),
            (coordinator + '0 2 h %s 6002 /d/0\n' % ('a' * 65), 'line 2: hostname or address exceeds 64 characters'),
            (coordinator + '0 2 h h 6002 relative/gpseg0\n', 'line 2: datadir must be an absolute path'),
            (coordinator, 'no segment rows'),
        ]
        for text, message in cases:
            with self.assertRaisesRegex(ReplicaTopologyError, message):
                replica_topology.parse_topology(text)

    def test_parse_topology_sorts_by_content(self):
        rows = replica_topology.parse_topology('1 3 h h 6003 /d/1\n  # comment\n-1 1 h h 6000 /d/-1\n0 2 h h 6002 /d/0\n')
        self.assertEqual([r[0] for r in rows], [-1, 0, 1])


class GppylibInstallListTestCase(GpTestCase):
    def test_every_gppylib_module_is_installed(self):
        """gppylib/Makefile installs an explicit list of modules: a module
        left out of it is missing from $GPHOME, and every utility importing
        it fails there while the source tree works."""
        gppylib_dir = os.path.abspath(os.path.join(os.path.dirname(__file__), '..', '..'))
        with open(os.path.join(gppylib_dir, 'Makefile')) as fp:
            makefile = fp.read().replace('\\\n', ' ')
        listed = set()
        for var in ('PROGRAMS', 'DATA'):
            for m in re.finditer(r'^%s\s*=\s*(.*)$' % var, makefile, re.MULTILINE):
                listed.update(m.group(1).split())
        modules = set(f for f in os.listdir(gppylib_dir) if f.endswith('.py'))
        self.assertEqual(modules - listed, set())


class PgconfIncludeIfExistsTestCase(GpTestCase):
    def setUp(self):
        self.datadir = tempfile.mkdtemp()

    def tearDown(self):
        shutil.rmtree(self.datadir)
        super(PgconfIncludeIfExistsTestCase, self).tearDown()

    def _write(self, name, content):
        path = os.path.join(self.datadir, name)
        with open(path, 'w') as fp:
            fp.write(content)
        return path

    def test_include_if_exists_reads_the_file(self):
        conf = self._write('postgresql.conf', "port = 5432\nmax_connections = 100\ninclude_if_exists = 'extra.conf'\n")
        self._write('extra.conf', "port = 6000\n")
        settings = pgconf.readfile(conf)
        self.assertEqual(settings.int('port'), 6000)
        self.assertEqual(settings.int('max_connections'), 100)
        self.assertNotIn('include_if_exists', settings)

    def test_include_if_exists_skips_a_missing_file(self):
        conf = self._write('postgresql.conf', "port = 5432\ninclude_if_exists = 'missing.conf'\n")
        settings = pgconf.readfile(conf)
        self.assertEqual(settings.int('port'), 5432)
        self.assertNotIn('include_if_exists', settings)

    def test_include_of_a_missing_file_still_fails(self):
        conf = self._write('postgresql.conf', "port = 5432\ninclude = 'missing.conf'\n")
        with self.assertRaisesRegex(Exception, 'missing.conf included from .* does not exist'):
            pgconf.readfile(conf)


if __name__ == '__main__':
    run_tests()
