#!/bin/bash
# Checks that built executables carry the hardening of the default build (see the HARDEN block of the
# project makefiles): position-independent, full RELRO (relocations read-only and bound at start), no
# executable stack, stack protector, fortified libc calls.
#
#   scripts/check_hardening.sh EXECUTABLE...
#
# Exits 1 when a property is missing from any of them.
set -u
fail=0
for b in "$@"; do
    missing=""
    readelf -h "$b" | grep -q "Type:.*DYN" || missing="$missing pie"
    readelf -lW "$b" | grep -q GNU_RELRO || missing="$missing relro"
    readelf -d "$b" | grep -qE "BIND_NOW|FLAGS.*NOW" || missing="$missing bind-now"
    readelf -lW "$b" | grep GNU_STACK | grep -q RWE && missing="$missing nx-stack"
    nm -D "$b" | grep -q __stack_chk_fail || missing="$missing stack-protector"
    nm -D "$b" | grep -q "_chk@" || missing="$missing fortify"
    if [ -n "$missing" ]; then
        echo "hardening: $b lacks:$missing"
        fail=1
    else
        echo "hardening: $b ok"
    fi
done
exit $fail
