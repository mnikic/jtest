#!/usr/bin/env python3
"""What code of the tree the tests never run.

  scripts/coverage.py [--seeds N] [--points K] [--parallel P] [--report]
                      [FILE...]

Builds the tests with coverage (make COV=1, in build/cov), runs every
test over N seeds (default 3) in each configuration below with
scripts/soak.py, then reads the counts with gcov.  For each FILE of the
tree (default: the ext2fs files the tests link), it prints the lines
and branches run, the functions never called, and the functions with
lines never run, most first.  Every line never run, with its source, is
in soak/coverage/FILE.txt, grouped by function.  With --report, only
the gcov part runs, on the counts the last run left.

The configurations: plain; JT_REORDER=2 (crashes that lose writes
since the last flush); JT_PAGEOUT_DELAY and JT_WRITE_DELAY (page-outs
and writes in flight a while).  K is JT_CRASH_POINTS (default 50).
"""

import argparse
import glob
import os
import re
import shutil
import subprocess
import sys
import tempfile

sys.path.insert (0, os.path.dirname (os.path.abspath (__file__)))
import common

FILES = ["journal.c", "balloc.c", "getblk.c", "truncate.c", "inode.c",
         "ialloc.c", "orphan.c", "hyper.c"]

CONFIGS = [
    ("plain", {}),
    ("reorder", {"JT_REORDER": "2"}),
    ("delays", {"JT_PAGEOUT_DELAY": "5000", "JT_WRITE_DELAY": "2000"}),
]

FUNC = re.compile (r"^function (\S+) called (\d+)")
LINE = re.compile (r"^\s*([^:]+):\s*(\d+):(.*)$")
BRANCH = re.compile (r"^branch\s+\d+ (taken (\d+)|never executed)")


def run_tests (a, build, out):
    """The soaks, into the counts in BUILD.  Returns the failed runs."""
    for f in glob.glob (os.path.join (build, "*.gcda")):
        os.unlink (f)
    failed = []
    for name, env in CONFIGS:
        e = dict (os.environ, COV="1", **env)
        cmd = [sys.executable, os.path.join (common.JTEST, "scripts",
                                             "soak.py"),
               "--build", build, "--seeds", str (a.seeds),
               "--points", str (a.points), "--parallel", str (a.parallel),
               "--out", os.path.join (out, name)]
        print ("== %s: %s" % (name, " ".join ("%s=%s" % kv
                                              for kv in env.items ())
                              or "no options"), flush=True)
        p = subprocess.run (cmd, env=e, stdout=subprocess.PIPE, text=True)
        lines = p.stdout.splitlines ()
        print ("\n".join (l for l in lines if "runs of" in l))
        failed += [l.strip () for l in lines
                   if " seed " in l and " ok " not in l and "kept in" not in l]
    return failed


def gcov_files (build, tmp):
    """gcov every object in BUILD into TMP: {source path: gcov file}."""
    gcnos = glob.glob (os.path.join (build, "*.gcno"))
    # The objects name their sources relative to jtest, so gcov runs
    # there, and what it writes is moved to TMP.
    before = set (glob.glob (os.path.join (common.JTEST, "*.gcov")))
    subprocess.run (["gcov", "-b", "-c", "-p", "-o", build] + gcnos,
                    cwd=common.JTEST, stdout=subprocess.DEVNULL,
                    stderr=subprocess.DEVNULL)
    files = {}
    for g in set (glob.glob (os.path.join (common.JTEST, "*.gcov"))) - before:
        dest = os.path.join (tmp, os.path.basename (g))
        shutil.move (g, dest)
        src = os.path.basename (g)[:-len (".gcov")]
        src = src.replace ("#", "/").replace ("^", "..")
        files[os.path.normpath (src)] = dest
    return files


def parse (path):
    """Per function, in order: (name, calls, lines run, lines, unrun lines
    as (number, text), branches taken, branches)."""
    funcs = []
    cur = None
    pending = []
    for raw in open (path, errors="replace"):
        m = FUNC.match (raw)
        if m:
            pending.append ((m.group (1), int (m.group (2))))
            continue
        m = BRANCH.match (raw)
        if m:
            if cur:
                cur[6] += 1
                if m.group (2) and int (m.group (2)) > 0:
                    cur[5] += 1
            continue
        m = LINE.match (raw)
        if not m:
            continue
        count, num, text = m.group (1).strip (), int (m.group (2)), m.group (3)
        if pending:
            # gcov puts a function's record just before its first line;
            # functions the compiler merged are counted together.
            name = pending[0][0]
            calls = sum (c for _, c in pending)
            cur = [name, calls, 0, 0, [], 0, 0]
            funcs.append (cur)
            pending = []
        if count == "-" or not cur:
            continue
        cur[3] += 1
        if count.startswith ("#####") or count.startswith ("====="):
            cur[4].append ((num, text))
        else:
            cur[2] += 1
    return funcs


def report (files, wanted, out):
    os.makedirs (out, exist_ok=True)
    for name in wanted:
        srcs = [s for s in files if s.endswith ("/ext2fs/" + name)]
        if not srcs:
            print ("\n%s: no counts (not linked into any test?)" % name)
            continue
        funcs = parse (files[srcs[0]])
        run = sum (f[2] for f in funcs)
        total = sum (f[3] for f in funcs)
        btaken = sum (f[5] for f in funcs)
        btotal = sum (f[6] for f in funcs)
        print ("\n%s: %d of %d lines run (%.0f%%), %d of %d branches taken "
               "(%.0f%%)" % (name, run, total, 100.0 * run / max (total, 1),
                             btaken, btotal, 100.0 * btaken / max (btotal, 1)))
        never = [f for f in funcs if f[1] == 0]
        if never:
            print ("  never called: " + ", ".join (f[0] for f in never))
        partly = sorted ((f for f in funcs if f[1] and f[4]),
                         key=lambda f: -len (f[4]))
        for f in partly[:15]:
            print ("  %-44s %3d of %3d lines not run"
                   % (f[0], len (f[4]), f[3]))
        if len (partly) > 15:
            print ("  ... %d more functions with lines not run"
                   % (len (partly) - 15))
        with open (os.path.join (out, name + ".txt"), "w") as t:
            for f in funcs:
                if not f[4]:
                    continue
                t.write ("%s (called %d): %d of %d lines not run\n"
                         % (f[0], f[1], len (f[4]), f[3]))
                last = None
                for num, text in f[4]:
                    if last is not None and num != last + 1:
                        t.write ("    ...\n")
                    t.write ("  %5d %s\n" % (num, text))
                    last = num
                t.write ("\n")


def main ():
    ap = argparse.ArgumentParser (description="Coverage of the tree.")
    ap.add_argument ("--seeds", type=int, default=3)
    ap.add_argument ("--points", type=int, default=50)
    ap.add_argument ("--parallel", type=int, default=4)
    ap.add_argument ("--report", action="store_true")
    ap.add_argument ("files", nargs="*")
    a = ap.parse_args ()

    build = os.path.join (common.JTEST, "build", "cov")
    out = os.path.join (common.JTEST, "soak", "coverage")
    failed = []
    if not a.report:
        make = ["make", "-s", "-C", common.JTEST, "COV=1", "B=" + build, "all"]
        if subprocess.run (make).returncode:
            sys.exit ("the build failed")
        shutil.rmtree (out, ignore_errors=True)
        failed = run_tests (a, build, out)

    with tempfile.TemporaryDirectory () as tmp:
        report (gcov_files (build, tmp), a.files or FILES, out)
    print ("\nEvery line not run, by function: %s/FILE.txt"
           % os.path.relpath (out, common.JTEST))
    if failed:
        print ("\n%d runs failed, so some counts stop early:" % len (failed))
        for l in failed:
            print ("  " + l[:160])


if __name__ == "__main__":
    main ()
