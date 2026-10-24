#!/usr/bin/env python3
"""Run every test over many seeds, and say which fail.

  scripts/soak.py [--seeds N] [--from S] [--parallel P] [--points K]
                  [--timeout SECONDS] [--build DIR] [TEST...]

Builds the tests first (make, in --build, default build/).  Each run is
in a directory of its own under soak/, and kept there if it fails, with
the test's output (out.log) and what it saved: crash images, their
replays and write logs.  Passing runs are removed.

Tests run P at a time (default 4), each with 1/P of the CPUs for its
crash check, and a test's seeds one after another.  K is JT_CRASH_POINTS
(default 100): the crash images per run, fewer than make check's 300 so
that more seeds fit.  A deterministic test (no seed) runs once.  Other
JT_* variables are passed on, for instance JT_WRITE_DELAY.  Exits 1 if
anything failed.
"""

import argparse
import concurrent.futures
import os
import shutil
import subprocess
import sys
import time

sys.path.insert (0, os.path.dirname (os.path.abspath (__file__)))
import common


def main ():
    ap = argparse.ArgumentParser (description="Soak the jtest tests.")
    ap.add_argument ("--seeds", type=int, default=20)
    ap.add_argument ("--from", dest="first", type=int, default=1)
    ap.add_argument ("--parallel", type=int, default=4)
    ap.add_argument ("--points", type=int, default=100)
    ap.add_argument ("--timeout", type=int, default=1800)
    ap.add_argument ("--build", default="build")
    ap.add_argument ("--out", default="soak")
    ap.add_argument ("tests", nargs="*")
    a = ap.parse_args ()

    build = os.path.abspath (os.path.join (common.JTEST, a.build))
    out = os.path.abspath (os.path.join (common.JTEST, a.out))
    tests = a.tests or common.tests ()
    unknown = set (tests) - set (common.tests ())
    if unknown:
        sys.exit ("no such test: " + ", ".join (sorted (unknown)))

    make = ["make", "-s", "-C", common.JTEST, "B=" + build, "all"]
    if subprocess.run (make).returncode:
        sys.exit ("the build failed")
    os.makedirs (out, exist_ok=True)

    cpus = os.cpu_count () or 1
    jobs = max (1, cpus // a.parallel)
    env = {"JT_CRASH_POINTS": str (a.points)}
    seeds = range (a.first, a.first + a.seeds)
    results = {}		# (test, seed) -> (status, seconds, line)

    def soak (test):
        for seed in ([a.first] if test in common.DETERMINISTIC else seeds):
            cwd = os.path.join (out, "%s-%d" % (test, seed))
            common.workdir (cwd, build)
            status, secs = common.run (os.path.join (build, test), test,
                                       seed, cwd, env=env,
                                       timeout=a.timeout, jobs=jobs)
            line = "" if status == 0 else common.failure_line (cwd)
            results[(test, seed)] = (status, secs, line)
            word = ("ok" if status == 0 else
                    "TIMEOUT" if status is None else "FAIL (%d)" % status)
            print ("%-14s seed %-4d %-10s %6.1fs %s"
                   % (test, seed, word, secs, line), flush=True)
            if status == 0:
                shutil.rmtree (cwd, ignore_errors=True)

    start = time.time ()
    with concurrent.futures.ThreadPoolExecutor (a.parallel) as pool:
        for f in [pool.submit (soak, t) for t in tests]:
            f.result ()

    failed = sorted (k for k, v in results.items () if v[0] != 0)
    print ("\n%d runs of %d tests in %.0fs: %d failed"
           % (len (results), len (tests), time.time () - start,
              len (failed)))
    for test, seed in failed:
        print ("  %s seed %d: %s (kept in %s)"
               % (test, seed, results[(test, seed)][2] or "see out.log",
                  os.path.join (a.out, "%s-%d" % (test, seed))))
    sys.exit (1 if failed else 0)


if __name__ == "__main__":
    main ()
