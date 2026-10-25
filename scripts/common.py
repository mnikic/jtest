"""What scripts/soak.py and scripts/mutants.py share: the tests, the image
each runs on, and running one in a directory of its own."""

import os
import shutil
import subprocess
import time

JTEST = os.path.dirname (os.path.dirname (os.path.abspath (__file__)))

# Tests that take no seed: one run is all there is.
DETERMINISTIC = {"t_forget_race", "t_free_only", "t_late_edit", "t_logfmt",
                 "t_nodecache", "t_notify_race", "t_release_order",
                 "t_sweep_race"}

IMAGES = ("base.img", "balloc.img", "big.img", "full.img")


def tests ():
    return sorted (f[:-2] for f in os.listdir (os.path.join (JTEST, "tests"))
                   if f.startswith ("t_") and f.endswith (".c"))


def image_for (test):
    """As make check picks it."""
    if test.startswith (("t_balloc_full", "t_balloc_lock")):
        return "full.img"
    for p in ("t_balloc", "t_trunc", "t_orphan", "t_rwcycle", "t_abortfiles",
              "t_nodecache", "t_fdir"):
        if test.startswith (p):
            return "balloc.img"
    return "base.img"


def workdir (path, build):
    """A fresh directory to run a test in: it writes its images and logs
    to build/ there.  The input images are links to BUILD's."""
    shutil.rmtree (path, ignore_errors=True)
    os.makedirs (os.path.join (path, "build"))
    for img in IMAGES:
        src = os.path.join (build, img)
        if os.path.exists (src):
            os.symlink (src, os.path.join (path, "build", img))


def run (binary, test, seed, cwd, extra_args=(), env=None, timeout=1800,
         jobs=None):
    """Run TEST (BINARY) with SEED in CWD.  Returns (status, seconds); a
    status of None is a timeout.  Its output goes to CWD/out.log."""
    e = dict (os.environ)
    if env:
        e.update (env)
    # Under TSan: the understood reports suppressed, and every report,
    # not just the first, in the log.  A fault on a file pager's mapping
    # waits for the page in the SIGSEGV handler (env/fpager.c), which is
    # not signal-safe, by design.
    e.setdefault ("TSAN_OPTIONS", "suppressions=%s halt_on_error=0 "
                  "history_size=4 report_signal_unsafe=0"
                  % os.path.join (JTEST, "tsan.supp"))
    if jobs:
        e["JT_JOBS"] = str (jobs)
    args = [binary, os.path.join ("build", image_for (test)), str (seed)]
    args += list (extra_args)
    start = time.time ()
    with open (os.path.join (cwd, "out.log"), "w") as out:
        try:
            p = subprocess.run (args, cwd=cwd, env=e, stdout=out,
                                stderr=subprocess.STDOUT, timeout=timeout)
            status = p.returncode
        except subprocess.TimeoutExpired:
            status = None
    return status, time.time () - start


def failure_line (cwd):
    """The line of CWD/out.log that says what failed, if one does."""
    try:
        lines = open (os.path.join (cwd, "out.log"), errors="replace")
    except OSError:
        return ""
    found = ""
    for line in lines:
        if ("CHECK failed" in line or "crash after" in line
                or "Assertion" in line or "STALE" in line
                or "ERROR: " in line or "panic" in line
                or "DEADLOCK" in line):
            found = line.strip ()
            break
    return found[:200]
