#!/bin/sh
# Runs the proof-of-concept Linux test programs and compares them with
# expected.txt. Works on Haiku (with Renguin) and on Linux (as a reference).
# Usage: run-tests.sh [directory-with-binaries]
dir=${1:-$(dirname "$0")}
pass=0; fail=0
while IFS="|" read -r prog args status out; do
	case "$prog" in ''|'#'*) continue ;; esac
	want=$(printf '%b' "$out")
	got=$("$dir/$prog" $args 2>/dev/null); rc=$?
	if [ "$rc" = "$status" ] && [ "$got" = "$want" ]; then
		echo "PASS $prog"; pass=$((pass + 1))
	else
		echo "FAIL $prog: exit $rc (want $status), output [$got] (want [$want])"
		fail=$((fail + 1))
	fi
done < "$dir/expected.txt"
echo "$pass passed, $fail failed"
[ "$fail" = 0 ]
