import csv
import gzip
import io
import os
import shutil
import subprocess
import sys
import tempfile
from datetime import datetime

from gppylib import logfilter
from gppylib.logfilter import (CsvFlatten, FilterLogEntries, GroupByTimestamp,
                               MatchColumns, MatchInFirstLine, TimestampInBounds,
                               filterize)
from gppylib.test.unit.gp_unittest import GpTestCase, run_tests

# One CSV log entry per element: (timestamp, severity, message).  The third
# message spans two lines, like a logged multi-line statement, and the fourth
# contains the '|' character used as the delimiter of the flattened output.
ENTRIES = [
    ('2026-09-24 10:00:01.000001 CST', 'LOG', 'database system is ready'),
    ('2026-09-24 10:05:02.000002 CST', 'ERROR', 'relation "t" does not exist'),
    ('2026-09-24 10:10:03.000003 CST', 'LOG', 'statement: select 1,\n2'),
    ('2026-09-24 10:15:04.000004 CST', 'FATAL', 'terminating connection|due to conflict'),
    ('2026-09-24 10:20:05.000005 CST', 'LOG', 'duration: 1.234 ms'),
]


def csv_log_text(entries=ENTRIES):
    """Render entries in the 30-column CSV server log format."""
    buf = io.StringIO()
    writer = csv.writer(buf, delimiter=',', quotechar='"', lineterminator='\n')
    for timestamp, severity, message in entries:
        row = [''] * 30
        row[0] = timestamp
        row[1] = 'gpadmin'
        row[3] = 'p123'
        row[11] = 'seg-1'
        row[16] = severity
        row[17] = '00000' if severity == 'LOG' else '42P01'
        row[18] = message
        row[27] = 'postgres.c'
        row[28] = '1735'
        writer.writerow(row)
    return buf.getvalue()


def flatten(text):
    """Run CSV log text through the same reader/flattener gplogfilter uses."""
    reader = csv.reader(io.StringIO(text, newline=''), delimiter=',', quotechar='"')
    return list(CsvFlatten(reader))


class CsvFlattenTestCase(GpTestCase):
    def test_every_entry_starts_with_its_own_timestamp(self):
        lines = flatten(csv_log_text())
        self.assertEqual(len(lines), len(ENTRIES))
        for line, (timestamp, _, message) in zip(lines, ENTRIES):
            self.assertTrue(line.startswith(timestamp + '|'), repr(line))
            self.assertNotIn('\x00', line)
            self.assertTrue(line.endswith('\n'), repr(line))
            self.assertNotIn('\r', line)
            self.assertEqual(line.count('\n'), 1 + message.count('\n'))

    def test_severity_gets_colon_suffix(self):
        self.assertIn('|ERROR: |42P01|', flatten(csv_log_text())[1])

    def test_field_containing_delimiter_is_quoted(self):
        line = flatten(csv_log_text())[3]
        self.assertIn('|"terminating connection|due to conflict"|', line)
        row = next(csv.reader([line], delimiter='|', quotechar='"'))
        self.assertEqual(row[18], 'terminating connection|due to conflict')

    def test_short_record_passes_through(self):
        lines = flatten('2026-09-24 10:00:01.000001 CST,gpadmin,partial\n')
        self.assertEqual(lines, ['2026-09-24 10:00:01.000001 CST|gpadmin|partial\n'])


class FilterLogEntriesTestCase(GpTestCase):
    def setUp(self):
        self.lines = flatten(csv_log_text())
        self.begin = datetime(2026, 9, 24, 10, 0, 1)
        self.end = datetime(2026, 9, 24, 10, 10, 3)

    def filtered(self, **kwargs):
        return list(FilterLogEntries(self.lines, **kwargs))

    def test_one_group_per_record(self):
        groups = list(GroupByTimestamp(self.lines))
        self.assertEqual([len(group) for group in groups], [1] * len(ENTRIES))
        self.assertEqual([group[0] for group in groups], self.lines)

    def test_tail(self):
        self.assertEqual(self.filtered(ibegin=-2), self.lines[-2:])

    def test_slice(self):
        self.assertEqual(self.filtered(ibegin=1, jend=3), self.lines[1:3])
        self.assertEqual(self.filtered(ibegin=3), self.lines[3:])
        self.assertEqual(self.filtered(jend=2), self.lines[:2])

    def test_slice_beyond_end_of_input(self):
        self.assertEqual(self.filtered(ibegin=0, jend=100), self.lines)
        self.assertEqual(self.filtered(ibegin=100), [])
        self.assertEqual(self.filtered(ibegin=-100), self.lines)

    def test_trouble_filter(self):
        trouble = filterize(MatchInFirstLine, 'ERROR: |FATAL: |PANIC: ')
        self.assertEqual(self.filtered(filters=[trouble]), [self.lines[1], self.lines[3]])

    def test_timestamp_bounds_include_first_entry(self):
        # begin is the timestamp of the very first entry; end is exclusive
        expected = self.lines[:2]
        self.assertEqual(self.filtered(beginstamp=self.begin, endstamp=self.end), expected)
        # the same bounds applied to grouped entries
        self.assertEqual(self.filtered(beginstamp=self.begin, endstamp=self.end, jend=100),
                         expected)

    def test_timestamp_bounds_on_empty_input(self):
        self.assertEqual(list(TimestampInBounds([], self.begin, None)), [])
        self.assertEqual(list(FilterLogEntries([], beginstamp=self.begin)), [])
        self.assertEqual(list(FilterLogEntries([], beginstamp=self.begin, ibegin=-1)), [])

    def test_match_columns(self):
        out = list(MatchColumns(GroupByTimestamp(self.lines), '1,17,19'))
        self.assertEqual(out[0], ['2026-09-24 10:00:01.000001 CST|LOG: |database system is ready\n'])
        self.assertEqual(out[3], ['2026-09-24 10:15:04.000004 CST|FATAL: |'
                                  '"terminating connection|due to conflict"\n'])


class GplogfilterScriptTestCase(GpTestCase):
    """
    Runs the gplogfilter script itself, the way a user would.
    """

    @classmethod
    def setUpClass(cls):
        super(GplogfilterScriptTestCase, cls).setUpClass()
        cls.pylib = os.path.dirname(os.path.dirname(os.path.abspath(logfilter.__file__)))
        cls.script = os.path.join(cls.pylib, 'gplogfilter')
        if not os.path.exists(cls.script):
            cls.script = shutil.which('gplogfilter')

    def setUp(self):
        if not self.script:
            self.skipTest('gplogfilter script not found')
        self.tmpdir = tempfile.mkdtemp()
        self.log = self.write_log('gpdb-2026-09-24_100000.csv', csv_log_text())
        self.expected = flatten(csv_log_text())

    def tearDown(self):
        shutil.rmtree(self.tmpdir)
        super(GplogfilterScriptTestCase, self).tearDown()

    def write_log(self, name, text):
        path = os.path.join(self.tmpdir, name)
        with open(path, 'w', newline='') as f:
            f.write(text)
        return path

    def gplogfilter(self, *args, **kwargs):
        env = dict(os.environ)
        env['PYTHONPATH'] = os.pathsep.join([self.pylib] + [p for p in sys.path if p])
        for name in kwargs.get('unset', ()):
            env.pop(name, None)
        env.update(kwargs.get('env', {}))
        cmd = [sys.executable, self.script]
        if kwargs.get('quiet', True):
            cmd.append('-q')
        proc = subprocess.run(cmd + list(args), input=kwargs.get('stdin'), env=env,
                              stdout=subprocess.PIPE, stderr=subprocess.PIPE)
        self.assertEqual(proc.returncode, kwargs.get('returncode', 0),
                         proc.stderr.decode(errors='replace'))
        return proc.stdout, proc.stderr

    def assertOutputIs(self, out, lines):
        self.assertEqual(out.decode(), ''.join(lines))

    def test_all_entries(self):
        out, err = self.gplogfilter(self.log)
        self.assertOutputIs(out, self.expected)
        self.assertEqual(err, b'')

    def test_tail(self):
        out, _ = self.gplogfilter('-n', '2', self.log)
        self.assertOutputIs(out, self.expected[-2:])

    def test_trouble(self):
        out, _ = self.gplogfilter('-t', self.log)
        self.assertOutputIs(out, [self.expected[1], self.expected[3]])
        out, _ = self.gplogfilter('-t', '-n', '1', self.log)
        self.assertOutputIs(out, [self.expected[3]])

    def test_begin_and_end(self):
        out, _ = self.gplogfilter('-b', '2026-09-24 10:00:01', '-e', '2026-09-24 10:10:03', self.log)
        self.assertOutputIs(out, self.expected[:2])
        out, _ = self.gplogfilter('-b', '2026-09-24 10:15', self.log)
        self.assertOutputIs(out, self.expected[3:])
        out, _ = self.gplogfilter('-e', '2026-09-24 10:05:02', '-t', self.log)
        self.assertOutputIs(out, [])

    def test_duration(self):
        out, _ = self.gplogfilter('-b', '2026-09-24 10:05', '-d', ':6', self.log)
        self.assertOutputIs(out, self.expected[1:3])
        out, _ = self.gplogfilter('-e', '2026-09-24 10:20:05', '-d', '0:10:2', self.log)
        self.assertOutputIs(out, self.expected[2:4])

    def test_columns(self):
        out, _ = self.gplogfilter('-C', '1,17,19', '-t', self.log)
        self.assertOutputIs(out, ['2026-09-24 10:05:02.000002 CST|ERROR: |"relation ""t"" does not exist"\n',
                                  '2026-09-24 10:15:04.000004 CST|FATAL: |'
                                  '"terminating connection|due to conflict"\n'])

    def test_compressed_input(self):
        gz = self.log + '.gz'
        with open(self.log, 'rb') as src, gzip.open(gz, 'wb') as dst:
            shutil.copyfileobj(src, dst)
        out, _ = self.gplogfilter('-n', '2', gz)
        self.assertOutputIs(out, self.expected[-2:])

        # the -u flag is needed when the name does not end in .gz, and a
        # name without a .csv suffix is read as plain text
        plain_gz = os.path.join(self.tmpdir, 'archived.log')
        os.rename(gz, plain_gz)
        out, _ = self.gplogfilter('-u', '-n', '1', plain_gz)
        self.assertEqual(out.decode(), csv_log_text().splitlines(True)[-1])

    def test_stdin(self):
        text = csv_log_text().encode()
        out, _ = self.gplogfilter('-n', '1', '-', stdin=text)
        self.assertEqual(out, text.splitlines(True)[-1])
        out, _ = self.gplogfilter('-u', '-n', '1', '-', stdin=gzip.compress(text))
        self.assertEqual(out, text.splitlines(True)[-1])

    def test_output_file(self):
        target = os.path.join(self.tmpdir, 'out')
        out, err = self.gplogfilter('-o', target, self.log, quiet=False)
        self.assertEqual(out, b'')
        self.assertIn(' output to ' + target, err.decode())
        with open(target, newline='') as f:
            self.assertEqual(f.read(), ''.join(self.expected))

        self.gplogfilter('-a', '-o', target, '-n', '1', self.log)
        with open(target, newline='') as f:
            self.assertEqual(f.read(), ''.join(self.expected + self.expected[-1:]))

    def test_output_directory(self):
        outdir = os.path.join(self.tmpdir, 'outdir')
        os.mkdir(outdir)
        self.gplogfilter('-o', outdir, '-t', self.log)
        with open(os.path.join(outdir, os.path.basename(self.log) + '.out'), newline='') as f:
            self.assertEqual(f.read(), self.expected[1] + self.expected[3])

    def test_compressed_output(self):
        target = os.path.join(self.tmpdir, 'out.gz')
        self.gplogfilter('-o', target, self.log)
        with gzip.open(target, 'rt', newline='') as f:
            self.assertEqual(f.read(), ''.join(self.expected))

        target = os.path.join(self.tmpdir, 'out2')
        self.gplogfilter('-z', '6', '-o', target, '-n', '1', self.log)
        with gzip.open(target + '.gz', 'rt', newline='') as f:
            self.assertEqual(f.read(), self.expected[-1])

        out, _ = self.gplogfilter('-z', '1', '-n', '1', self.log)
        self.assertEqual(gzip.decompress(out).decode(), self.expected[-1])

    def test_undecodable_bytes_pass_through_unchanged(self):
        raw = (b'2026-09-24 10:00:01.000001 CST|gpadmin|LOG: |caf\xe9 \xff\xfe|1\n'
               b'2026-09-24 10:00:02.000002 CST|gpadmin|ERROR: |plain|2\n')
        log = os.path.join(self.tmpdir, 'legacy.log')
        with open(log, 'wb') as f:
            f.write(raw)

        out, _ = self.gplogfilter(log)
        self.assertEqual(out, raw)
        out, _ = self.gplogfilter('-', stdin=raw)
        self.assertEqual(out, raw)
        out, _ = self.gplogfilter('-t', '-', stdin=raw)
        self.assertEqual(out, raw.splitlines(True)[1])

        target = os.path.join(self.tmpdir, 'out')
        self.gplogfilter('-o', target, log)
        with open(target, 'rb') as f:
            self.assertEqual(f.read(), raw)
        self.gplogfilter('-o', target + '.gz', log)
        with gzip.open(target + '.gz', 'rb') as f:
            self.assertEqual(f.read(), raw)

    def test_output_encoding_narrower_than_input(self):
        log = os.path.join(self.tmpdir, 'legacy.log')
        with open(log, 'wb') as f:
            f.write(b'2026-09-24 10:00:01.000001 CST|caf\xc3\xa9 \xff|x\n')
        out, _ = self.gplogfilter(log, env={'PYTHONIOENCODING': 'ascii', 'LC_ALL': 'C.UTF-8'})
        self.assertEqual(out, b'2026-09-24 10:00:01.000001 CST|caf\\xe9 \xff|x\n')

    def test_long_field_in_plain_log(self):
        line = '2026-09-24 10:00:01.000001 CST|%s|last\n' % ('x' * 200000)
        log = self.write_log('legacy.log', line)
        out, _ = self.gplogfilter('-C', '1,3', log)
        self.assertEqual(out.decode(), '2026-09-24 10:00:01.000001 CST|last\n')

    def test_unterminated_last_line_does_not_join_next_file(self):
        first = self.write_log('a.log', '2026-09-24 10:00:01.000001 CST|first|no line end')
        second = self.write_log('b.log', '2026-09-24 10:00:02.000002 CST|second|\n')
        out, _ = self.gplogfilter(first, second)
        self.assertEqual(out.decode(), '2026-09-24 10:00:01.000001 CST|first|no line end\n'
                                       '2026-09-24 10:00:02.000002 CST|second|\n')

    def test_no_input_and_no_data_directory(self):
        _, err = self.gplogfilter(returncode=2,
                                  unset=('COORDINATOR_DATA_DIRECTORY', 'MASTER_DATA_DIRECTORY'))
        self.assertIn('specify input file or "-" for standard input', err.decode())

    def test_files_outside_time_range_are_skipped(self):
        earlier = [(ts.replace('09-24', '09-23'), sev, msg) for ts, sev, msg in ENTRIES]
        old_log = self.write_log('gpdb-2026-09-23_000000.csv', csv_log_text(earlier))
        old_expected = flatten(csv_log_text(earlier))

        # the old file was superseded a minute before the range begins
        out, err = self.gplogfilter('--prunefiles', '-b', '2026-09-24 10:01',
                                    old_log, self.log, quiet=False)
        self.assertOutputIs(out, self.expected[1:])
        self.assertIn('SKIP file: ' + old_log, err.decode())
        self.assertNotIn('SKIP file: ' + self.log, err.decode())

        # nothing is skipped without --prunefiles
        out, err = self.gplogfilter('-b', '2026-09-24 10:01', old_log, self.log, quiet=False)
        self.assertOutputIs(out, self.expected[1:])
        self.assertNotIn('SKIP file', err.decode())

        # a file superseded right when the range begins may still hold
        # entries stamped after the rotation, as may one superseded earlier
        for begin, expected in (('2026-09-24 10:00', self.expected),
                                ('2026-09-24 10:00:59', self.expected[1:]),
                                ('2026-09-24', self.expected)):
            out, err = self.gplogfilter('--prunefiles', '-b', begin, old_log, self.log,
                                        quiet=False)
            self.assertOutputIs(out, expected)
            self.assertNotIn('SKIP file', err.decode(), begin)

        # the new file was created after the range ends
        out, err = self.gplogfilter('--prunefiles', '-e', '2026-09-24', old_log, self.log,
                                    quiet=False)
        self.assertOutputIs(out, old_expected)
        self.assertIn('SKIP file: ' + self.log, err.decode())
        self.assertNotIn('SKIP file: ' + old_log, err.decode())

        # a file created right when the range ends may hold entries stamped
        # just before the rotation
        out, err = self.gplogfilter('--prunefiles', '-e', '2026-09-24 10:00', old_log, self.log,
                                    quiet=False)
        self.assertOutputIs(out, old_expected)
        self.assertNotIn('SKIP file', err.decode())

        # a range inside the old file still reads the old file
        out, err = self.gplogfilter('--prunefiles', '-b', '2026-09-23 10:05',
                                    '-e', '2026-09-23 10:11', old_log, self.log, quiet=False)
        self.assertOutputIs(out, old_expected[1:3])
        self.assertNotIn('SKIP file: ' + old_log, err.decode())
        self.assertIn('SKIP file: ' + self.log, err.decode())


if __name__ == '__main__':
    run_tests()
