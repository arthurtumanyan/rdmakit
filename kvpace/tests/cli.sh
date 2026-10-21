#!/bin/bash
# Command-line checks of kvpace that need no server: invalid option values are rejected with exit status 2
# before any connection is opened, and --help and --version succeed.
#
#   tests/cli.sh BUILD_DIR
#
# Exits 0 when every check passes and 1 otherwise.
set -u
k=${1:?usage: cli.sh BUILD_DIR}/kvpace
fail=0
check() { # name, expected, actual
    if [ "$2" = "$3" ]; then
        echo "cli: ok   $1"
    else
        echo "cli: FAIL $1: expected [$2], got [$3]"
        fail=1
    fi
}
"$k" --help >/dev/null 2>&1
check "--help" 0 $?
check "--version" 1 "$("$k" --version | grep -c '^kvpace ')"
# Port 1 is never contacted: each case must fail during option parsing.
for bad in "--threads abc" "--threads 0" "--conns-per-thread 0" "--pipeline 0" "--rate -5" "--duration x" \
    "--duration 0" "--requests 0" "--keys 0" "--theta 1" "--read-ratio 2" "--value-size 0" "--block-size 0" \
    "--new-conv-prob 1.5" "--multi-get 0" "--driver foo" "--dist bogus" "--workload ycsb-e" \
    "--percentiles 50,abc" "stray"; do
    # shellcheck disable=SC2086
    "$k" --port 1 $bad >/dev/null 2>&1
    check "rejects '$bad'" 2 $?
done
exit $fail
