#!/usr/bin/env bash
# SPDX-License-Identifier: Apache-2.0
#
# Dependency check (ARCHITECTURE.md, section 2). Dependencies point one way:
#
#   applications -> open-tick host -> open-tick core -> ISO C standard library
#
# 1. core/ includes only ISO C standard headers and core's own headers, and
#    asks for no OS extensions (_POSIX_C_SOURCE, _GNU_SOURCE, ...). That is
#    what keeps it buildable on any host with a C11 compiler.
# 2. Nothing in open-tick includes an application. An include with a path is
#    either open-tick's own (opentick/...) or a known system directory;
#    anything else, such as <opencrank/j1939.h>, fails.
# 3. core/ never includes host/.
#
# Usage: tools/check_deps.sh
set -euo pipefail

ROOT=$(cd "$(dirname "$0")/.." && pwd)
status=0

ISO_C_HEADERS="assert.h complex.h ctype.h errno.h fenv.h float.h inttypes.h iso646.h limits.h locale.h math.h
setjmp.h signal.h stdalign.h stdarg.h stdatomic.h stdbool.h stddef.h stdint.h stdio.h stdlib.h stdnoreturn.h
string.h tgmath.h threads.h time.h uchar.h wchar.h wctype.h"
SYSTEM_DIRS="sys/ linux/ net/ netinet/ arpa/"

fail() {
    echo "FAIL: $*"
    status=1
}

includes() { # file -> "line|header"
    grep -nE '^[[:space:]]*#[[:space:]]*include' "$1" |
        sed -E 's/^([0-9]+):[[:space:]]*#[[:space:]]*include[[:space:]]*[<"]([^>"]+)[>"].*/\1|\2/' || true
}

echo "== core/: ISO C and core only"
while IFS= read -r -d '' file; do
    rel=${file#"$ROOT"/}
    if grep -qE '_(POSIX_C|GNU|DEFAULT|XOPEN|BSD)_SOURCE' "$file"; then
        fail "$rel asks for OS extensions; core/ is ISO C only"
    fi
    while IFS='|' read -r line header; do
        [[ -z "$header" ]] && continue
        if [[ " $(echo $ISO_C_HEADERS) " == *" $header "* ]]; then
            continue
        fi
        if [[ "$header" == opentick/* && "$header" != opentick/host/* && -f "$ROOT/core/include/$header" ]]; then
            continue
        fi
        fail "$rel:$line: '$header' is not allowed in core/"
    done < <(includes "$file")
done < <(find "$ROOT/core" -name '*.[ch]' -print0)

echo "== everywhere: no application headers"
while IFS= read -r -d '' file; do
    rel=${file#"$ROOT"/}
    while IFS='|' read -r line header; do
        [[ -z "$header" || "$header" != */* ]] && continue
        if [[ "$header" == opentick/* ]]; then
            if [[ -f "$ROOT/core/include/$header" || -f "$ROOT/host/include/$header" ]]; then
                continue
            fi
            fail "$rel:$line: '$header' does not exist in open-tick"
            continue
        fi
        known=0
        for dir in $SYSTEM_DIRS; do
            [[ "$header" == "$dir"* ]] && known=1
        done
        [[ $known -eq 1 ]] && continue
        fail "$rel:$line: '$header' is neither open-tick's nor a known system header (an application?)"
    done < <(includes "$file")
done < <(find "$ROOT/core" "$ROOT/host" "$ROOT/tests" "$ROOT/examples" -name '*.[ch]' -print0)

if [[ $status -eq 0 ]]; then
    echo "dependencies OK"
fi
exit $status
