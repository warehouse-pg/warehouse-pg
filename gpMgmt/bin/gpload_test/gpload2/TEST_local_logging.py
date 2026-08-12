#!/usr/bin/env pytest
"""
Tests for how gpload is invoked and where it writes its log.

Both cases are settled before gpload connects to the database, so these
do not need a running cluster.
"""

import os
import subprocess
import tempfile

# Parses as YAML but is rejected by gpload's own validation. Reaching that
# complaint means gpload got through option parsing, opened its log file and
# read the control file, which is what these tests are looking for.
INCOMPLETE_CONFIG = "VERSION: 1.0.0.1\n"


def run_gpload(args):
    p = subprocess.Popen(['gpload'] + args,
                         stdout=subprocess.PIPE,
                         stderr=subprocess.PIPE,
                         universal_newlines=True)
    out, err = p.communicate()
    return p.returncode, out, err


def test_config_file_path_containing_spaces():
    "gpload accepts a control file whose path contains spaces"
    with tempfile.TemporaryDirectory() as tmp:
        confdir = os.path.join(tmp, 'my config dir')
        os.makedirs(confdir)
        conf = os.path.join(confdir, 'load me.yml')
        with open(conf, 'w') as f:
            f.write(INCOMPLETE_CONFIG)

        logfile = os.path.join(tmp, 'gpload.log')
        rc, out, err = run_gpload(['-f', conf, '-l', logfile])

        # The wrapper used to expand $*, which joined the arguments and let
        # the shell split the path again, so gpload rejected the leftovers
        # and printed its usage text without ever opening a log file.
        assert 'COMMAND NAME: gpload' not in out
        assert os.path.exists(logfile)
        with open(logfile) as f:
            assert 'gpload session started' in f.read()


def test_log_falls_back_to_gpadminlogs():
    "gpload logs to $HOME/gpAdminLogs when the requested log cannot be opened"
    with tempfile.TemporaryDirectory() as tmp:
        conf = os.path.join(tmp, 'load.yml')
        with open(conf, 'w') as f:
            f.write(INCOMPLETE_CONFIG)

        # Put a regular file where the log's parent directory would go, so
        # that the path cannot be opened however gpload goes about it.
        blocker = os.path.join(tmp, 'not-a-directory')
        open(blocker, 'w').close()

        name = 'gpload_fallback_%d.log' % os.getpid()
        requested = os.path.join(blocker, name)
        fallback = os.path.join(os.environ['HOME'], 'gpAdminLogs', name)

        try:
            rc, out, err = run_gpload(['-f', conf, '-l', requested])

            assert 'could not open logfile' in err
            assert os.path.exists(fallback)
            with open(fallback) as f:
                log = f.read()
            assert 'could not use requested log file' in log
            assert 'gpload session started' in log
        finally:
            if os.path.exists(fallback):
                os.remove(fallback)


def test_missing_log_directory_is_not_created():
    "gpload does not create a directory named by -l, it falls back instead"
    name = 'gpload_nodir_%d.log' % os.getpid()
    fallback = os.path.join(os.environ['HOME'], 'gpAdminLogs', name)
    with tempfile.TemporaryDirectory() as tmp:
        conf = os.path.join(tmp, 'load.yml')
        with open(conf, 'w') as f:
            f.write(INCOMPLETE_CONFIG)

        missing = os.path.join(tmp, 'no such dir')
        try:
            rc, out, err = run_gpload(['-f', conf,
                                       '-l', os.path.join(missing, name)])

            # A mistyped -l should show up as a warning, not as a directory
            # that gpload invented on the user's behalf.
            assert not os.path.exists(missing)
            assert os.path.exists(fallback)
            with open(fallback) as f:
                assert 'could not use requested log file' in f.read()
        finally:
            if os.path.exists(fallback):
                os.remove(fallback)
