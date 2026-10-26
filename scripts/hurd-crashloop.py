#!/usr/bin/env python3
"""Crash ext2fs over and over on a real Hurd, and fsck after each crash.

Run as root inside the Hurd, on a filesystem made for it:

  mke2fs -t ext3 -o hurd -b 4096 DEVICE      (or a file: -F FILE SIZE)
  python3 hurd-crashloop.py --dev DEVICE --mnt /mnt/crash \\
      --blob /root/blob.tar --hours 8

Each round:
  1. Starts the translator: settrans -ac MNT /hurd/ext2fs [ARGS] DEVICE.
  2. Runs the workload in MNT (by default: untar BLOB into a new
     directory, remove the one before, repeat), in its own process group.
  3. After a random delay (--min-kill to --max-kill seconds), kill -9's
     the translator, then the workload.
  4. Replays the journal as e2fsck does at boot (e2fsck -E journal_only
     -y), then checks it read-only (e2fsck -fn), which must find nothing.
Every --clean-every rounds, the round ends with a clean unmount instead
(settrans -g), and the same check.

It stops at the first unclean check, and keeps the fsck output, the
workload's output and the translator's command line under --out.  With
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

DEFAULT_CMDS = [
    "mkdir -p d{n} && tar -xf {blob} -C d{n}",
    "rm -rf d$(({n} - 1))",
]


def log (out, msg):
    line = time.strftime ("%Y-%m-%d %H:%M:%S ") + msg
    print (line, flush=True)
    with open (os.path.join (out, "crashloop.log"), "a") as f:
        f.write (line + "\n")


def run (cmd, **kw):
    return subprocess.run (cmd, stdout=subprocess.PIPE,
                           stderr=subprocess.STDOUT, text=True, **kw)


def translator_pids (dev):
    """Pids of the ext2fs processes whose command line names DEV, from
    /proc (the Hurd's procfs)."""
    pids = []
    for name in os.listdir ("/proc"):
        if not name.isdigit ():
            continue
        try:
            with open ("/proc/%s/cmdline" % name, "rb") as f:
                args = f.read ().split (b"\0")
        except OSError:
            continue
        args = [a.decode (errors="replace") for a in args if a]
        if (args and os.path.basename (args[0]).startswith ("ext2fs")
                and dev in args[1:]):
            pids.append (int (name))
    return pids


def start_translator (a):
    os.makedirs (a.mnt, exist_ok=True)
    cmd = ["settrans", "-ac", a.mnt, "/hurd/ext2fs"] + a.args + [a.dev]
    r = run (cmd)
    if r.returncode:
        raise RuntimeError ("%s: %s" % (" ".join (cmd), r.stdout.strip ()))
    pids = translator_pids (a.dev)
    if len (pids) != 1:
        raise RuntimeError ("expected one ext2fs on %s, found %s"
                            % (a.dev, pids))
    return pids[0]


def clear_mountpoint (a):
    """Take whatever is left of the active translator off MNT: a dead one
    after a kill, or a live one left over from an earlier run."""
    run (["settrans", "-fga", a.mnt])
    run (["settrans", "-a", a.mnt])
    if not translator_pids (a.dev):
        return
    for pid in translator_pids (a.dev):
        os.kill (pid, signal.SIGKILL)
    time.sleep (1)
    if translator_pids (a.dev):
        raise RuntimeError ("an ext2fs on %s will not go away" % a.dev)


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
    ap.add_argument ("--blob", help="a tar file for the default workload")
    ap.add_argument ("--cmd", action="append",
                     help="a workload command (repeatable); default: untar "
                     "the blob into d{n}, remove d{n-1}")
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
    a = ap.parse_args ()

    a.args = a.args.split ()
    a.cmd = a.cmd or DEFAULT_CMDS
    if any ("{blob}" in c for c in a.cmd) and not a.blob:
        ap.error ("the workload uses {blob}: give --blob")
    if a.blob:
        a.blob = os.path.abspath (a.blob)
    if os.geteuid () != 0:
        ap.error ("run it as root")
    os.makedirs (a.out, exist_ok=True)
    rng = random.Random (a.seed)
    deadline = time.monotonic () + a.hours * 3600 if a.hours else None

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
            w = start_workload (a, wlog)
            time.sleep (delay)
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
        log (a.out, "round %d: %s after %.1f s, workload %s: clean"
             % (rnd, kind, delay,
                "still running" if wstatus in (None, -9) else
                "had stopped (exit %s)" % wstatus))

    log (a.out, "done: %d rounds (%d kill -9, %d clean unmounts), all clean"
         % (rnd, crashes, cleans))
    return 0


if __name__ == "__main__":
    sys.exit (main ())
