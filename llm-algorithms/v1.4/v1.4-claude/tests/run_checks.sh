#!/usr/bin/env bash
# Randomised cross-check of sop_solver against the brute-force DP on small instances.
# usage: tests/run_checks.sh [num_instances] [threads] [extra solver flags, e.g. --no-heuristic]
set -u
cd "$(dirname "$0")/.."
COUNT=${1:-60}
THREADS=${2:-1}
EXTRA=${3:-}
fail=0
tmp=$(mktemp -d)
for seed in $(seq 1 "$COUNT"); do
  n=$((4 + seed % 11))                      # N in 4..14
  case $((seed % 5)) in 0) d=0.0;; 1) d=0.05;; 2) d=0.15;; 3) d=0.3;; 4) d=0.6;; esac
  multi=""; [ $((seed % 3)) -eq 0 ] && multi="--multi"
  python3 tests/gen_instance.py "$n" "$d" "$seed" 50 $multi > "$tmp/i.sop"
  ref=$(python3 tests/brute_force.py "$tmp/i.sop")
  out=$(./sop_solver "$tmp/i.sop" --threads "$THREADS" $EXTRA 2>"$tmp/err" | tail -1)
  cost=$(echo "$out" | awk -F, '{print $5}')
  opt=$(echo "$out" | awk -F, '{print $7}')
  if [ "$cost" != "$ref" ] || { [ "$ref" != "-1" ] && [ "$opt" != "yes" ]; }; then
    echo "MISMATCH seed=$seed n=$n dens=$d $multi: solver=$cost/$opt reference=$ref"; fail=1
    cp "$tmp/i.sop" "/tmp/fail_$seed.sop"
  fi
done
rm -rf "$tmp"
[ $fail -eq 0 ] && echo "all $COUNT random instances match the reference" || exit 1
