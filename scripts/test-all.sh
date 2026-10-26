#!/bin/bash
# Every test, once, in every configuration, then the mutants: a check
# before committing.  Every step runs, even after one fails; a summary at
# the end says which failed, and the script exits 1 if one did.
#
#   scripts/test-all.sh
#
# Settings, from the environment:
#   HURD       the tree under test (make's default: the one next to jtest)
#   SEEDS      seeds per configuration (default 4)
#   PARALLEL   tests at once (default: the number of CPUs)
#   MUTANTS=0  leave the mutants out (the slowest step)
#
# Each step's output is in soak/all/STEP.log, and a failing run's files
# under soak/all/STEP/.

cd "$(dirname "$0")/.." || exit 1
export JT_WATCHDOG=${JT_WATCHDOG:-120}
export UBSAN_OPTIONS=${UBSAN_OPTIONS:-halt_on_error=1:print_stacktrace=1}
seeds=${SEEDS:-4}
parallel=${PARALLEL:-$(nproc)}
out=soak/all
rm -rf "$out"
mkdir -p "$out"
summary=""
failed=0
start=$(date +%s)

# step NAME COMMAND...: run it, its output to soak/all/NAME.log.
step () {
  local name=$1 t=$(date +%s) result
  shift
  printf '%-14s ' "$name"
  if "$@" > "$out/$name.log" 2>&1; then result=ok; else result=FAILED; failed=1; fi
  local line
  line=$(printf '%-14s %-7s %4ds  %s' "$name" "$result" $(( $(date +%s) - t )) \
	 "$(tail -1 "$out/$name.log")")
  echo "$result"
  summary+="$line"$'\n'
}

# soak NAME BUILD [VAR=VALUE...] [-- SOAK.PY OPTIONS...]
soak () {
  local name=$1 build=$2 vars=() opts=()
  shift 2
  while [ $# -gt 0 ] && [ "$1" != -- ]; do vars+=("$1"); shift; done
  [ "$1" = -- ] && shift
  opts=("$@")
  step "$name" env "${vars[@]}" python3 scripts/soak.py --build "$build" \
    --seeds "$seeds" --parallel "$parallel" --out "$out/$name" "${opts[@]}"
}

echo "HURD=${HURD:-(the tree next to jtest)}, $seeds seeds, $parallel at once"
step build       make -s B=build all
step build-tsan  make -s B=build/tsan SAN=thread all
step build-asan  make -s B=build/asan SAN=address,undefined all

soak plain   build
soak sync    build JT_SYNCHRONOUS=1
soak reorder build JT_REORDER=2
soak delays  build JT_PAGEOUT_DELAY=5000 JT_WRITE_DELAY=2000
soak tsan    build/tsan -- --points 30
soak asan    build/asan

if [ "${MUTANTS:-1}" != 0 ]; then
  step mutants python3 scripts/mutants.py --parallel "$parallel" --out "$out/mutants"
fi

echo
printf '%s' "$summary"
echo "$(( ($(date +%s) - start) / 60 )) minutes; $([ $failed = 0 ] && echo 'all passed' || echo 'SOMETHING FAILED')"
exit $failed
