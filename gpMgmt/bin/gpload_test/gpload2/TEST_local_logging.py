#!/usr/bin/env pytest
"""
Tests for how gpload is invoked and where it writes its log.

Both are settled before gpload connects to the database, so these do not
need a running cluster.
"""

import os
import shutil
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
    tmp = tempfile.mkdtemp()
    try:
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
    finally:
        shutil.rmtree(tmp, ignore_errors=True)
