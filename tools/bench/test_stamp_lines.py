# SPDX-License-Identifier: CECILL-C
# Copyright (c) 2026 Philippe Leduc
#
# stamp_lines.py over logs written here, and stamper.sh's start, stop and truncate around it: each
# line stamped once and in order, a log rewritten under it read as a new stream, a last line with
# no newline kept, its exit once the watched process is gone, a zombie or another user's, a start
# refused when the stamper is not running, a stop refused when it failed, and bench-capture.sh
# truncating its log under no running stamper.

import os
import re
import shutil
import signal
import subprocess
import sys
import tempfile
import time
import unittest

HERE = os.path.dirname(os.path.abspath(__file__))
STAMPER = os.path.join(HERE, "stamp_lines.py")


def stamps(log):
    with open(log + ".times") as f:
        return [line.rstrip("\n").split("\t", 1) for line in f]


def lines(log):
    return [text for _, text in stamps(log)]


def until(what, ready, bound=10.0):
    deadline = time.monotonic() + bound
    while not ready():
        if time.monotonic() > deadline:
            raise AssertionError("waited %.0fs for %s" % (bound, what))
        time.sleep(0.01)


def zombie_state(pid):
    with open("/proc/%d/stat" % pid) as f:
        stat = f.read()
    return stat[stat.rfind(")") + 2]


class Stamped(unittest.TestCase):
    def setUp(self):
        self.scratch = tempfile.mkdtemp(prefix="kickos-stamp-")
        self.log = os.path.join(self.scratch, "console.log")
        open(self.log, "w").close()

    def tearDown(self):
        shutil.rmtree(self.scratch)

    def start(self, watched):
        proc = subprocess.Popen([sys.executable, STAMPER, self.log, str(watched)])
        until("the stamper to be ready", lambda: os.path.exists(self.log + ".times"))
        return proc

    def append(self, text, mode="a"):
        with open(self.log, mode) as f:
            f.write(text)

    def stamped(self, text):
        until("a stamp of %r" % text, lambda: text in lines(self.log))

    def test_grown_and_rewritten(self):
        proc = self.start(os.getpid())
        self.append("one\r\n")
        self.stamped("one")
        self.append("two\n")
        self.stamped("two")
        self.append("thr")
        proc.send_signal(signal.SIGSTOP)
        self.append("new-one\nnew-two\nnew-three, longer than the stream it replaces\n", "w")
        proc.send_signal(signal.SIGCONT)
        self.stamped("new-three, longer than the stream it replaces")
        proc.send_signal(signal.SIGSTOP)
        self.append("same-1\nsame-2\nsame-3, the length of the stream it replaces...\n", "w")
        proc.send_signal(signal.SIGCONT)
        self.stamped("same-3, the length of the stream it replaces...")
        self.append("tail with no newline")
        proc.terminate()
        self.assertEqual(proc.wait(10), 0)
        self.assertEqual(lines(self.log), [
            "one", "two", "new-one", "new-two", "new-three, longer than the stream it replaces",
            "same-1", "same-2", "same-3, the length of the stream it replaces...",
            "tail with no newline"])
        at = [float(t) for t, _ in stamps(self.log)]
        self.assertEqual(at, sorted(at), "the stamps go back in time")
        self.assertLess(at[0], at[1], "two lines that arrived in separate polls share a stamp")

    def test_watched_process_ends(self):
        watched = subprocess.Popen(["sleep", "60"])
        proc = self.start(watched.pid)
        self.append("last words")
        watched.kill()
        watched.wait()
        self.assertEqual(proc.wait(10), 0)
        self.assertEqual(lines(self.log), ["last words"])

    def test_watched_zombie(self):
        watched = subprocess.Popen(["sleep", "60"])
        try:
            watched.send_signal(signal.SIGKILL)
            until("the planted child to become a zombie", lambda: zombie_state(watched.pid) == "Z")
            proc = self.start(watched.pid)
            self.assertEqual(proc.wait(10), 0)
        finally:
            watched.wait()

    def test_watched_by_another_user(self):
        try:
            os.kill(1, 0)
        except PermissionError:
            pass
        else:
            self.skipTest("this user may signal pid 1")
        proc = self.start(1)
        self.assertEqual(proc.wait(10), 0)


class Stamper(unittest.TestCase):
    """stamper.sh as bench-capture.sh sources it, each case one bash run."""

    def setUp(self):
        self.scratch = tempfile.mkdtemp(prefix="kickos-stamper-")
        self.log = os.path.join(self.scratch, "console.log")
        open(self.log, "w").close()

    def tearDown(self):
        shutil.rmtree(self.scratch)

    def planted(self, body):
        path = os.path.join(self.scratch, "planted")
        os.makedirs(path)
        with open(os.path.join(path, "stamp_lines.py"), "w") as f:
            f.write(body)
        return path

    def bash(self, script, *args):
        return subprocess.run(["bash", "-c", '. "$0"; ' + script, os.path.join(HERE, "stamper.sh")]
                              + list(args), capture_output=True, text=True, timeout=30)

    def test_truncate_drops_the_cut(self):
        run = self.bash('stamper_start "$1" "$2" $$ || exit 1; echo "before the cut" >> "$2"; '
                        'stamper_truncate "$1" "$2" $$ || exit 2; echo "after the cut" >> "$2"; '
                        'stamper_stop || exit 3', HERE, self.log)
        self.assertEqual(run.returncode, 0, run.stderr)
        self.assertEqual(lines(self.log), ["after the cut"])

    def test_dead_start_refused(self):
        dead = self.planted("import sys\nsys.exit(1)\n")
        with open(self.log + ".times", "w") as f:
            f.write("1.000000\tstale\n")
        run = self.bash('stamper_start "$1" "$2" $$ && exit 1; echo "$STAMPER_WHY"', dead, self.log)
        self.assertEqual(run.returncode, 0, "a start whose stamper exits at once is accepted")
        self.assertIn("is not running", run.stdout)
        self.assertFalse(os.path.exists(self.log + ".times"),
                         "an earlier capture's stamps survive a refused start")

    def test_late_failure_refused(self):
        late = self.planted("import os, signal, sys, time\n"
                            "open(sys.argv[1] + '.times', 'w').close()\n"
                            "signal.signal(signal.SIGTERM, lambda *_: sys.exit(3))\n"
                            "while os.path.getsize(sys.argv[1]) == 0:\n"
                            "    time.sleep(0.01)\n"
                            "sys.exit(1)\n")
        run = self.bash('stamper_start "$1" "$2" $$ || exit 1; stamper_stop && exit 2; '
                        'echo "$STAMPER_WHY"', late, self.log)
        self.assertEqual(run.returncode, 0, "a stamper that fails at its stop is accepted")
        self.assertIn("status 3", run.stdout)
        run = self.bash('stamper_start "$1" "$2" $$ || exit 1; echo "a line" >> "$2"; '
                        'while stamper_alive "$STAMPER"; do sleep 0.01; done; '
                        'stamper_stop && exit 2; echo "$STAMPER_WHY"', late, self.log)
        self.assertEqual(run.returncode, 0, "a stamper that failed during the capture is accepted")
        self.assertIn("status 1", run.stdout)


class Capture(unittest.TestCase):
    def test_no_cut_under_a_running_stamper(self):
        with open(os.path.join(HERE, "bench-capture.sh")) as f:
            source = f.read().splitlines()
        starts = [n for n, line in enumerate(source) if "stamper_start " in line]
        self.assertTrue(starts, "bench-capture.sh starts no stamper")
        cut = re.compile(r'(^|[^>])>[ \t]*"?\$LOG"?([^.A-Za-z_]|$)')
        cuts = ["%d: %s" % (n + 1, line) for n, line in enumerate(source)
                if n >= starts[0] and cut.search(line)]
        self.assertEqual(cuts, [], "bench-capture.sh truncates its log under a running stamper")


if __name__ == "__main__":
    unittest.main()
