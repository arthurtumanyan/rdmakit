#!/bin/bash
# Static analysis of the C sources of rdmaduct, arenakv and kvpace:
#   clang --analyze   the clang static analyzer (the checks scan-build runs), every source file
#   cppcheck          warning and portability checks
#
#   scripts/analyze.sh
#
# Prints every finding and exits 1 when there is any, 0 otherwise. Needs clang and cppcheck.
set -u
cd "$(dirname "$0")/.."
# Every optional part is enabled, so that the RDMA transport and the TLS code (rdmaduct encrypted endpoints, the
# TLS listeners of arenakv, the TLS and native paths of kvpace) are analyzed too.
defs="-DDUCT_HAVE_RDMA -DDUCT_HAVE_TLS -DAKV_HAVE_TLS -DPACE_HAVE_TLS -DPACE_HAVE_NATIVE"
inc="-Irdmaduct/include -Irdmaduct/src -Iarenakv/include -Iarenakv/src -Ikvpace/include -Ikvpace/src"
files=$(git ls-files 'rdmaduct/src/*.c' 'rdmaduct/tools/*.c' 'arenakv/src/*.c' 'arenakv/tools/*.c' 'kvpace/src/*.c' 'kvpace/tools/*.c')
found=0
for f in $files; do
    out=$(clang --analyze -Xanalyzer -analyzer-output=text -std=gnu2x -D_GNU_SOURCE $defs $inc -o /dev/null "$f" 2>&1)
    if echo "$out" | grep -q "warning:"; then
        echo "$out"
        found=1
    fi
done
cp=$(cppcheck --quiet --enable=warning,portability --inline-suppr --std=c11 -D_GNU_SOURCE $defs $inc \
    rdmaduct/src rdmaduct/tools arenakv/src arenakv/tools kvpace/src 2>&1 | grep -v missingInclude)
if [ -n "$cp" ]; then
    echo "$cp"
    found=1
fi
echo "analyze: $(echo "$files" | wc -w) files, $([ $found = 0 ] && echo "no findings" || echo "findings above")"
exit $found
