#!/usr/bin/env python3
"""Crash ext2fs over and over on a real Hurd, and fsck after each crash.

Run as root inside the Hurd, on a filesystem made for it:

  mke2fs -t ext3 -o hurd -b 4096 DEVICE      (or a file: -F FILE SIZE)
  python3 hurd-crashloop.py --dev DEVICE --mnt /mnt/crash \\
      --blob /root/blob.tar --hours 8

--blob and --out are paths on the running system, and must not be under
--mnt: that is the filesystem each round crashes.

Each round:
  1. Starts the translator: settrans -ac MNT TRANSLATOR [ARGS] DEVICE,
     TRANSLATOR being --translator (default /hurd/ext2fs), for instance
     a build's ext2fs.static.
  2. Runs the workload in MNT (by default: remove MNT/work, untar BLOB
     into a new one, repeat), in its own process group.
  3. After a random delay (--min-kill to --max-kill seconds), kill -9's
     the translator, then the workload.
  4. Replays the journal as e2fsck does at boot (e2fsck -E journal_only
     -y), then checks it read-only (e2fsck -fn), which must find nothing.
Every --clean-every rounds, the round ends with a clean unmount instead
(settrans -g), and the same check.

It stops at the first unclean check, and keeps the fsck output and the
workload's output under --out.  The translator runs through
--out/translator.sh, which records its pid in --out/translator.pid.  With
--save-image and a file as DEVICE, it also copies the image as the crash
left it, before e2fsck touches it.  It stops too after --hours, or
--rounds.  The log of every round goes to --out/crashloop.log.

Workload commands (--cmd, repeatable) run with sh -c in MNT, one after
another in a loop, with {blob} replaced by --blob and {n} by a counter
that goes up each pass, so that they can name new directories.
"""

import argparse
import os
import random
import shutil
import signal
import subprocess
import sys
import time

# Remove the last tree, unpack a new one: a kill lands in a big rm -rf
# or a big untar, and what an earlier round left half done is removed
# first, so nothing it left can make tar fail.
DEFAULT_CMDS = [
    "rm -rf work",
    "mkdir work",
    "tar -xf {blob} -C work",
]


def log (out, msg):
    line = time.strftime ("%Y-%m-%d %H:%M:%S ") + msg
    print (line, flush=True)
    with open (os.path.join (out, "crashloop.log"), "a") as f:
        f.write (line + "\n")


def run (cmd, **kw):
    return subprocess.run (cmd, stdout=subprocess.PIPE,
                           stderr=subprocess.STDOUT, text=True, **kw)


VERBOSE = False


def step (msg):
    """With --verbose, say what comes next, so that a hang shows where."""
    if VERBOSE:
        print (time.strftime ("%H:%M:%S ") + "  " + msg, flush=True)


def write_wrapper (a):
    """The program settrans starts: it writes its pid to a file and execs
    the translator, which keeps that pid.  The pid is then known without
    looking through /proc, whose reads can block on the Hurd."""
    a.pidfile = os.path.join (a.out, "translator.pid")
    a.wrapper = os.path.join (a.out, "translator.sh")
    a.tlog = os.path.join (a.out, "translator.log")
    # Its output to the log first, so that it never holds settrans' (or
    # the script's) open, and a record of which binary ran.
    with open (a.wrapper, "w") as f:
        f.write ("""#!/bin/sh
exec >> %(log)s 2>&1
echo "=== $(date '+%%Y-%%m-%%d %%H:%%M:%%S') pid $$: %(bin)s $*"
echo "    md5 $(md5sum %(bin)s | cut -d' ' -f1)"
echo $$ > %(pid)s
exec %(bin)s "$@"
""" % {"log": a.tlog, "bin": a.translator, "pid": a.pidfile})
    os.chmod (a.wrapper, 0o755)


def recorded_pid (a):
    """The pid of the translator last started, if it still runs."""
    try:
        with open (a.pidfile) as f:
            pid = int (f.read ().strip ())
    except (OSError, ValueError):
        return None
    try:
        os.kill (pid, 0)
    except ProcessLookupError:
        return None
    except PermissionError:
        pass
    return pid


def start_translator (a):
    os.makedirs (a.mnt, exist_ok=True)
    try:
        os.unlink (a.pidfile)
    except FileNotFoundError:
        pass
    cmd = ["settrans", "-ac", a.mnt, a.wrapper] + a.args + [a.dev]
    step (" ".join (cmd))
    # Not through a pipe, in case something settrans starts keeps it
    # open; the translator itself writes to --out/translator.log.
    with open (a.tlog, "a") as tlog:
        status = subprocess.run (cmd, stdin=subprocess.DEVNULL, stdout=tlog,
                                 stderr=subprocess.STDOUT).returncode
    if status:
        raise RuntimeError ("%s failed (exit %d); see %s"
                            % (" ".join (cmd), status, a.tlog))
    pid = recorded_pid (a)
    step ("translator pid %s" % pid)
    if pid is None:
        raise RuntimeError ("the translator on %s did not start, or died"
                            % a.dev)
    return pid


def clear_mountpoint (a):
    """Take whatever is left of the active translator off MNT: a dead one
    after a kill, or a live one left over from an earlier run."""
    step ("clearing %s" % a.mnt)
    run (["settrans", "-fga", a.mnt])
    run (["settrans", "-a", a.mnt])
    pid = recorded_pid (a)
    if pid is None:
        return
    step ("killing translator %d" % pid)
    os.kill (pid, signal.SIGKILL)
    for _ in range (50):
        if recorded_pid (a) is None:
            return
        time.sleep (0.1)
    raise RuntimeError ("translator %d on %s will not go away" % (pid, a.dev))


def start_workload (a, wlog):
    """The workload, as one shell loop in a process group of its own."""
    body = []
    for c in a.cmd:
        body.append (c.replace ("{blob}", a.blob or "").replace ("{n}", "$n"))
    script = ("n=1; while :; do " + " && ".join (body)
              + " || exit 1; n=$((n + 1)); done")
    return subprocess.Popen (["sh", "-c", script], cwd=a.mnt, stdout=wlog,
                             stderr=subprocess.STDOUT, start_new_session=True)


def stop_workload (p):
    if p.poll () is None:
        try:
            os.killpg (p.pid, signal.SIGKILL)
        except ProcessLookupError:
            pass
    try:
        p.wait (timeout=60)
    except subprocess.TimeoutExpired:
        pass


def check (a, rnd, rdir):
    """Replay, then check read-only.  Returns None if clean, or what is
    wrong."""
    step ("e2fsck of %s" % a.dev)
    r1 = run (["e2fsck", "-E", "journal_only", "-y", a.dev])
    r2 = run (["e2fsck", "-fn", a.dev])
    with open (os.path.join (rdir, "fsck.txt"), "w") as f:
        f.write ("# e2fsck -E journal_only -y: exit %d\n%s\n"
                 "# e2fsck -fn: exit %d\n%s\n"
                 % (r1.returncode, r1.stdout, r2.returncode, r2.stdout))
    if r1.returncode not in (0, 1):
        return "e2fsck -E journal_only exit %d" % r1.returncode
    if r2.returncode:
        return "e2fsck -fn exit %d after the replay" % r2.returncode
    return None


def main ():
    ap = argparse.ArgumentParser (
        description=__doc__, formatter_class=argparse.RawTextHelpFormatter)
    ap.add_argument ("--dev", required=True,
                     help="the device or image file the filesystem is on")
    ap.add_argument ("--mnt", required=True, help="where to set it up")
    ap.add_argument ("--blob", help="a tar file for the default workload: "
                     "a path on this system, not under --mnt, which is the "
                     "filesystem the rounds crash")
    ap.add_argument ("--cmd", action="append",
                     help="a workload command (repeatable); default: "
                     "rm -rf work, mkdir work, untar the blob into it")
    ap.add_argument ("--translator", default="/hurd/ext2fs",
                     help="the ext2fs to run (default /hurd/ext2fs), e.g. "
                     "a build's ext2fs/ext2fs.static")
    ap.add_argument ("--args", default="",
                     help="extra ext2fs options, e.g. \"-s\" (one string)")
    ap.add_argument ("--min-kill", type=float, default=5,
                     help="seconds before the kill, at least (default 5)")
    ap.add_argument ("--max-kill", type=float, default=60,
                     help="seconds before the kill, at most (default 60)")
    ap.add_argument ("--clean-every", type=int, default=10,
                     help="every N rounds unmount cleanly instead "
                     "(default 10, 0 never)")
    ap.add_argument ("--hours", type=float, default=0,
                     help="stop after this long (default: no limit)")
    ap.add_argument ("--rounds", type=int, default=0,
                     help="stop after this many rounds (default: no limit)")
    ap.add_argument ("--out", default="/root/crashloop",
                     help="where to keep logs (not on the tested "
                     "filesystem; default /root/crashloop)")
    ap.add_argument ("--save-image", action="store_true",
                     help="with a file as --dev, keep a copy of a crash "
                     "image that fails")
    ap.add_argument ("--seed", type=int, help="for the kill delays")
    ap.add_argument ("--verbose", action="store_true",
                     help="say each step before it runs")
    a = ap.parse_args ()
    global VERBOSE
    VERBOSE = a.verbose

    a.args = a.args.split ()
    a.cmd = a.cmd or DEFAULT_CMDS
    if any ("{blob}" in c for c in a.cmd) and not a.blob:
        ap.error ("the workload uses {blob}: give --blob")
    if a.blob:
        a.blob = os.path.abspath (a.blob)
        if not os.path.isfile (a.blob):
            ap.error ("%s: no such file" % a.blob)
        mnt = os.path.abspath (a.mnt).rstrip ("/") + "/"
        if a.blob.startswith (mnt) or os.path.abspath (a.out).startswith (mnt):
            ap.error ("--blob and --out must not be on the filesystem under "
                      "test (%s)" % a.mnt)
    a.translator = os.path.abspath (a.translator)
    if not os.access (a.translator, os.X_OK):
        ap.error ("%s is not an executable" % a.translator)
    if os.geteuid () != 0:
        ap.error ("run it as root")
    os.makedirs (a.out, exist_ok=True)
    rng = random.Random (a.seed)
    deadline = time.monotonic () + a.hours * 3600 if a.hours else None

    write_wrapper (a)
    log (a.out, "translator %s %s on %s" % (a.translator, " ".join (a.args),
                                           a.dev))
    clear_mountpoint (a)
    if check (a, 0, a.out):
        log (a.out, "the filesystem is not clean to begin with: see "
             + os.path.join (a.out, "fsck.txt"))
        return 1

    rnd = 0
    crashes = cleans = 0
    while True:
        if a.rounds and rnd >= a.rounds:
            break
        if deadline and time.monotonic () >= deadline:
            break
        rnd += 1
        rdir = os.path.join (a.out, "round")
        shutil.rmtree (rdir, ignore_errors=True)
        os.makedirs (rdir)
        clean = a.clean_every and rnd % a.clean_every == 0
        delay = rng.uniform (a.min_kill, a.max_kill)

        pid = start_translator (a)
        with open (os.path.join (rdir, "workload.txt"), "w") as wlog:
            step ("workload started; the kill in %.1f s" % delay)
            w = start_workload (a, wlog)
            started = time.monotonic ()
            stopped_at = None
            while time.monotonic () - started < delay:
                if stopped_at is None and w.poll () is not None:
                    stopped_at = time.monotonic () - started
                time.sleep (0.2)
            if stopped_at is None and w.poll () is not None:
                stopped_at = time.monotonic () - started
            if clean:
                stop_workload (w)
                r = run (["settrans", "-g", a.mnt])
                if r.returncode:
                    log (a.out, "round %d: the clean unmount failed: %s"
                         % (rnd, r.stdout.strip ()))
                    clear_mountpoint (a)
            else:
                os.kill (pid, signal.SIGKILL)
                stop_workload (w)
                clear_mountpoint (a)
        wstatus = w.returncode

        if a.save_image and os.path.isfile (a.dev):
            shutil.copyfile (a.dev, os.path.join (rdir, "crash.img"))
        what = check (a, rnd, rdir)
        kind = "clean unmount" if clean else "kill -9"
        if what:
            fdir = os.path.join (a.out, "failed-round-%d" % rnd)
            shutil.move (rdir, fdir)
            log (a.out, "round %d (%s after %.1f s): %s; kept in %s"
                 % (rnd, kind, delay, what, fdir))
            return 1
        if clean:
            cleans += 1
        else:
            crashes += 1
        if stopped_at is None:
            wnote = "running at the kill"
        else:
            # It stopped by itself: say why, from its last line.
            last = ""
            try:
                with open (os.path.join (rdir, "workload.txt"),
                           errors="replace") as f:
                    lines = [l.strip () for l in f if l.strip ()]
                last = lines[-1] if lines else "no output"
            except OSError:
                pass
            wnote = "had stopped by itself after %.1f s (exit %s: %s)" % (
                stopped_at, wstatus, last[:120])
        log (a.out, "round %d: %s after %.1f s, workload %s: clean"
             % (rnd, kind, delay, wnote))

    log (a.out, "done: %d rounds (%d kill -9, %d clean unmounts), all clean"
         % (rnd, crashes, cleans))
    return 0


if __name__ == "__main__":
    sys.exit (main ())
