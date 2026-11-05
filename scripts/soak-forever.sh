#!/bin/sh
# Soak until stopped: every configuration in turn, each on seeds no batch
# has run before, then again.  Meant for a machine left to it for hours
# or days.  It ignores the hangup of the terminal it was started from,
# so a dropped ssh connection does not stop it; its output then goes on
# in soak/forever/summary.log only.  One runs at a time: a second start
# while one runs says so and exits.
#
#   scripts/soak-forever.sh [-h HOURS] [-c CYCLES] [-f FROM] [-n SEEDS]
#                           [-p PARALLEL]
#
# A cycle is a batch of SEEDS seeds (default 50) per configuration:
# plain, JT_REORDER=2, the delays, JT_SYNCHRONOUS=1, JT_SYNCHRONOUS=1 with
# JT_REORDER=2, then a fifth as many under TSan, and under ASan with
# UBSan.  Seeds start at FROM (default 10000) and go up by batch.  It
# stops after HOURS, or CYCLES cycles; by default never.  PARALLEL is
# soak.py's (default: the number of CPUs).
#
# soak/forever/summary.log gets a line per batch; a failing batch's runs
# are kept, with their images, in its directory under soak/forever/.
# Rerun one with the batch's variables and its seed:
#   JT_REORDER=2 build/t_wrap build/base.img 10123
# HURD is make's (the tree next to jtest by default).

hours=0 cycles=0 from=10000 seeds=50 parallel=$(nproc)
while getopts h:c:f:n:p: o; do
  case $o in
    h) hours=$OPTARG ;;
    c) cycles=$OPTARG ;;
    f) from=$OPTARG ;;
    n) seeds=$OPTARG ;;
    p) parallel=$OPTARG ;;
    *) sed -n '2,23s/^# \{0,1\}//p' "$0"; exit 2 ;;
  esac
done

cd "$(dirname "$0")/.." || exit 1
out=soak/forever
mkdir -p $out
summary=$out/summary.log

# One at a time: two would share the batch directories, last.log, the
# summary and the builds.  The lock is held, through descriptor 9, by
# this shell and everything it starts, so it is free again only once the
# last test of a stopped run has gone too.
exec 9>"$out/lock"
if ! flock -n 9; then
  echo "soak-forever is already running (pid $(cat "$out/pid" 2>/dev/null))," \
    "on $out" >&2
  exit 1
fi
echo $$ > "$out/pid"

# A job started with & from an ssh shell gets SIGHUP when the connection
# goes, and so do the jobs it starts.  Ignored here, it stays ignored in
# make, soak.py and the tests.  INT, TERM and QUIT are noted in the
# summary; a SIGKILL (the OOM killer) cannot be.
trap '' HUP
for sig in INT TERM QUIT; do
  trap "echo \"\$(date '+%F %T') stopped by SIG$sig; next seed \$from\" \
    >> \"\$summary\"; exit 1" $sig
done
san_seeds=$(( seeds / 5 > 0 ? seeds / 5 : 1 ))
end=0
[ "$hours" != 0 ] && end=$(( $(date +%s) + $(awk "BEGIN { printf \"%d\", $hours * 3600 }") ))

echo "Building plain, TSan and ASan+UBSan..."
make -s B=build all || exit 1
make -s B=build/tsan SAN=thread all || exit 1
make -s B=build/asan SAN=address,undefined all || exit 1

# One batch: soak.py on BUILD, seeds FROM.. , with the variables in CFG.
batch () {
  build=$1 n=$2 points=$3 name=$4; shift 4
  dir=$out/$name-$from
  # On a hang or a failed store check, every thread's backtrace in the
  # kept run's out.log (gdb, if it is installed).
  env "$@" JT_WATCHDOG=300 JT_WATCHDOG_GDB=1 \
    python3 scripts/soak.py --build "$build" \
    --seeds "$n" --from "$from" --parallel "$parallel" --points "$points" \
    --out "$dir" > "$out/last.log" 2>&1
  status=$?
  # soak.py's count line, then a line per failed run.
  count=$(grep -E "^[0-9]+ runs of " "$out/last.log" | tail -1)
  [ -n "$count" ] || count="no summary (soak.py ended with status $status)"
  echo "$(date '+%F %T') $name seeds $from-$((from + n - 1)): $count" \
    | tee -a "$summary"
  grep -E "^  t_[a-z_]+ seed [0-9]+:" "$out/last.log" | sed 's/^/    /' \
    | tee -a "$summary"
  [ $status -eq 0 ] && rm -rf "$dir"
  from=$((from + n))
}

done_ () {
  [ "$end" != 0 ] && [ "$(date +%s)" -ge "$end" ] && return 0
  [ "$cycles" != 0 ] && [ "$cycle" -ge "$cycles" ] && return 0
  return 1
}

cycle=0
while ! done_; do
  batch build "$seeds" 100 plain
  batch build "$seeds" 100 reorder JT_REORDER=2
  batch build "$seeds" 100 delays JT_PAGEOUT_DELAY=5000 JT_WRITE_DELAY=2000
  batch build "$seeds" 100 sync JT_SYNCHRONOUS=1
  batch build "$seeds" 100 sync-reorder JT_SYNCHRONOUS=1 JT_REORDER=2
  batch build/tsan "$san_seeds" 30 tsan
  batch build/asan "$san_seeds" 30 asan
  cycle=$((cycle + 1))
done
echo "$(date '+%F %T') stopped after $cycle cycles; next seed $from" | tee -a "$summary"
