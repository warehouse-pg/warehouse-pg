import errno
import os
import shutil
import subprocess
import sys

from mock import patch

from gppylib.test.unit.gp_unittest import *
from gppylib import mainUtils
from gppylib.mainUtils import PIDLockFile, PIDLockHeld, SimpleMainLock
from gppylib.mainUtils import parseStatusLine


class MainUtilsTestCase(GpTestCase):

    def setUp(self):
        # parent PID
        self.ppid = os.getpid()
        self.lockfile = "/tmp/lock-%s" % (str(self.ppid))
        self.lock = PIDLockFile(self.lockfile)

    def test_release_removes_lock(self):
        self.lock.acquire()
        self.assertEqual(True,os.path.exists(self.lockfile))
        self.lock.release()
        self.assertEqual(False, os.path.exists(self.lockfile))

    def test_with_block_removes_lock(self):
        with self.lock:
            self.assertEqual(True,os.path.exists(self.lockfile))
        self.assertEqual(False, os.path.exists(self.lockfile))

    def test_lock_owned_by_parent(self):
        with self.lock as l:
            self.assertEqual(l.read_pid(), self.ppid)


    def test_exceptionPIDLockHeld_if_same_pid(self):
        with self.lock:
            with self.assertRaisesRegex(PIDLockHeld, "PIDLock already held at %s" % (self.lockfile)):
                self.lock.acquire()

    def test_child_can_read_lock_owner(self):
        with self.lock as l:
            pid = os.fork()
            if pid == 0:
                self.assertEqual(l.read_pid(), self.ppid)
                os._exit(0)
            else:
                os.wait()

    def test_exceptionPIDLockHeld_if_different_pid(self):
        self.lock.acquire()
        pid = os.fork()
        # if child, os.fork() == 0
        if pid == 0:
            with self.assertRaisesRegex(PIDLockHeld, "PIDLock already held at %s" % (self.lockfile)):
                self.lock.acquire()
            os._exit(0)
        else:
            os.wait()
            self.lock.release()

    def test_accept_double_dash_in_data_dir_name_for_start(self):
        line = 'STATUS--DIR:/Users/shrakesh/workspace/gpdb/gpAux/gpdemo/datadirs/dbfast2/demoData--Dir1--STARTED:True--REASONCODE:0--REASON:Start Succeeded'
        reasonStr, started = parseStatusLine(line, isStart=True)[1:3]
        self.assertTrue(started)
        self.assertEqual(reasonStr, 'Start Succeeded')


    def test_accept_double_dash_in_data_dir_name_for_stop(self):
        line = 'STATUS--DIR:/Users/shrakesh/workspace/gpdb/gpAux/gpdemo/datadirs/dbfast2/demoData--Dir1--STOPPED:True--REASON:Shutdown Succeeded'
        reasonStr, stopped = parseStatusLine(line, isStop=True)[1:3]
        self.assertTrue(stopped)
        self.assertEqual(reasonStr, 'Shutdown Succeeded')


    def test_accept_double_dash_in_data_dir_name_for_excep(self):
        line = 'STATUS--DIR:/Users/shrakesh/workspace/gpdb/gpAux/gpdemo/datadirs/dbfast2/demoData--Dir1--STOPPED:True--REASON:Shutdown Succeeded'

        with self.assertRaises(Exception):
            parseStatusLine(line)


    def test_childPID_can_not_remove_parent_lock(self):
        with self.lock:
            pid = os.fork()
            if pid == 0:
                newlock = PIDLockFile(self.lockfile)
                try:
                    with newlock:
                        # the __exit__ or __del__ of this with
                        # would normally call release - but this
                        # PID does not own the lock
                        pass
                # we expect the the acquire to fail
                except:
                    pass
                self.assertEqual(True, os.path.exists(self.lockfile))
                os._exit(0)
            else:
                os.wait()

    def test_release_is_idempotent(self):
        self.lock.acquire()
        self.addCleanup(self.lock.release)
        self.lock.release()
        self.assertTrue(self.lock.released)
        # second release must not touch the filesystem at all
        with patch('gppylib.mainUtils.PIDLockFile.read_pid') as read_pid, \
             patch('gppylib.mainUtils.shutil.rmtree') as rmtree:
            self.lock.release()
            read_pid.assert_not_called()
            rmtree.assert_not_called()

    def test_reacquire_after_release_clears_released_flag(self):
        self.lock.acquire()
        self.addCleanup(self.lock.release)
        self.lock.release()
        self.lock.acquire()
        self.assertFalse(self.lock.released)
        self.lock.release()
        self.assertEqual(False, os.path.exists(self.lockfile))

    def test_del_swallows_exceptions(self):
        # Simulates interpreter teardown, where builtins like open() are gone
        # and read_pid() raises NameError.
        self.lock.acquire()
        self.addCleanup(self.lock.release)
        with patch('gppylib.mainUtils.PIDLockFile.read_pid', side_effect=NameError("name 'open' is not defined")):
            try:
                self.lock.__del__()
            except Exception as e:
                self.fail("__del__ raised %r" % e)

    def test_del_after_explicit_release_is_noop_at_interpreter_shutdown(self):
        # End-to-end check: a PIDLockFile whose __main__ dict is
        # kept alive past module teardown must not emit "Exception ignored in
        # __del__ ... NameError: name 'open' is not defined" when the
        # interpreter exits.
        #
        # A bare global in __main__ is NOT enough to trigger the bug: the
        # __main__ dict is dropped as soon as sys.modules['__main__'] is
        # cleared, while builtins are still intact. gpmovemirrors reproduces
        # because it defines functions at module level; each function's
        # __globals__ points back at __main__.__dict__, forming a cycle that
        # is only broken by the final GC pass, after module dicts and
        # builtins are gone. The _hold() function below recreates that cycle.
        lockdir = self.lockfile + "-shutdown"
        self.addCleanup(lambda: os.path.isdir(lockdir) and shutil.rmtree(lockdir))
        script = (
            "import sys\n"
            "from gppylib.mainUtils import PIDLockFile\n"
            "def _hold():\n"
            "    return lock\n"
            "lock = PIDLockFile(sys.argv[1])\n"
            "lock.acquire()\n"
            "lock.release()\n"
        )
        proc = self._run_python(script, lockdir)
        self.assertEqual(0, proc.returncode, proc.stderr)
        self.assertNotIn("Exception ignored", proc.stderr)
        self.assertNotIn("NameError", proc.stderr)
        self.assertFalse(os.path.exists(lockdir))

    def test_child_exit_does_not_remove_parent_lock(self):
        # A forked child inherits the lock object. When the child exits
        # normally (here via sys.exit() inside the with block, which runs
        # __exit__), it must not remove the lock its parent still holds.
        # Run in a separate interpreter so the child can exit for real.
        lockdir = self.lockfile + "-fork"
        self.addCleanup(shutil.rmtree, lockdir, True)
        script = (
            "import os, sys\n"
            "from gppylib.mainUtils import PIDLockFile\n"
            "with PIDLockFile(sys.argv[1]) as lock:\n"
            "    pid = os.fork()\n"
            "    if pid == 0:\n"
            "        sys.exit(0)\n"
            "    os.waitpid(pid, 0)\n"
            "    print(lock.read_pid() == os.getpid())\n"
        )
        proc = self._run_python(script, lockdir)
        self.assertEqual(0, proc.returncode, proc.stderr)
        self.assertEqual("True", proc.stdout.strip())
        self.assertFalse(os.path.exists(lockdir))

    def test_child_acquire_records_child_pid(self):
        # A forked child that acquires through an inherited object must record
        # its own PID, and so be able to release the lock again.
        self.addCleanup(shutil.rmtree, self.lockfile, True)
        pid = os.fork()
        if pid == 0:
            code = 1
            try:
                self.lock.acquire()
                code = 2 if self.lock.read_pid() != os.getpid() else 0
                self.lock.release()
                if code == 0 and os.path.exists(self.lockfile):
                    code = 3
            finally:
                os._exit(code)
        _, status = os.waitpid(pid, 0)
        self.assertEqual(0, os.WEXITSTATUS(status))

    def test_acquire_replaces_empty_lock_dir(self):
        self.addCleanup(shutil.rmtree, self.lockfile, True)
        os.makedirs(self.lockfile)
        self.lock.acquire()
        self.assertEqual(self.ppid, self.lock.read_pid())
        self.lock.release()
        self.assertFalse(os.path.exists(self.lockfile))

    def test_acquire_failure_leaves_no_lock_dir(self):
        # If writing the PID fails (e.g. ENOSPC), neither the lock directory
        # nor the temporary one may be left behind.
        self.addCleanup(shutil.rmtree, self.lockfile, True)
        with patch('gppylib.mainUtils.open', create=True,
                   side_effect=OSError(errno.ENOSPC, "No space left on device")):
            with self.assertRaises(OSError) as cm:
                self.lock.acquire()
        self.assertNotIsInstance(cm.exception, PIDLockHeld)
        self.assertFalse(os.path.exists(self.lockfile))
        self.assertFalse(os.path.exists("%s.%d.tmp" % (self.lockfile, self.ppid)))

    def _simple_main_lock(self):
        sml = SimpleMainLock({})
        sml.ppath = self.lockfile
        sml.pidlockfile = self.lock
        return sml

    def _assert_simple_main_lock_acquired(self, sml):
        self.assertIsNone(sml.acquire())
        self.assertIs(self.lock, sml.locktorelease)
        self.assertEqual(self.ppid, self.lock.read_pid())
        sml.release()
        self.assertFalse(os.path.exists(self.lockfile))

    def test_simple_main_lock_acquires_empty_lock_dir(self):
        self.addCleanup(shutil.rmtree, self.lockfile, True)
        os.makedirs(self.lockfile)
        self._assert_simple_main_lock_acquired(self._simple_main_lock())

    def test_simple_main_lock_acquires_lock_dir_without_pid_file(self):
        self.addCleanup(shutil.rmtree, self.lockfile, True)
        os.makedirs(self.lockfile)
        open(os.path.join(self.lockfile, "stray"), 'w').close()
        self._assert_simple_main_lock_acquired(self._simple_main_lock())

    def test_simple_main_lock_acquires_lock_dir_with_empty_pid_file(self):
        self.addCleanup(shutil.rmtree, self.lockfile, True)
        os.makedirs(self.lockfile)
        open(os.path.join(self.lockfile, "PID"), 'w').close()
        self._assert_simple_main_lock_acquired(self._simple_main_lock())

    def test_simple_main_lock_reports_live_owner(self):
        self.addCleanup(shutil.rmtree, self.lockfile, True)
        owner = os.getppid()
        os.makedirs(self.lockfile)
        with open(os.path.join(self.lockfile, "PID"), 'w') as p:
            p.write(str(owner))
        sml = self._simple_main_lock()
        self.assertEqual(owner, sml.acquire())
        self.assertIsNone(sml.locktorelease)
        sml.release()
        self.assertTrue(os.path.exists(self.lockfile))

    def test_simple_main_lock_never_reports_ownerless_lock_as_acquired(self):
        # Even if the lock directory cannot be taken over, a directory without
        # a PID file must not be reported as acquired.
        self.addCleanup(shutil.rmtree, self.lockfile, True)
        os.makedirs(self.lockfile)
        sml = self._simple_main_lock()
        with patch('gppylib.mainUtils.PIDLockFile.acquire',
                   side_effect=PIDLockHeld("held", self.lockfile)):
            self.assertEqual("unknown", sml.acquire())
        self.assertIsNone(sml.locktorelease)

    def _run_python(self, script, *args):
        pylib = os.path.dirname(os.path.dirname(os.path.abspath(mainUtils.__file__)))
        env = dict(os.environ)
        env['PYTHONPATH'] = os.pathsep.join([pylib] + [p for p in sys.path if p])
        return subprocess.run([sys.executable, '-c', script] + list(args),
                              env=env, stdout=subprocess.PIPE, stderr=subprocess.PIPE,
                              universal_newlines=True, timeout=60)



if __name__ == '__main__':
    run_tests()
