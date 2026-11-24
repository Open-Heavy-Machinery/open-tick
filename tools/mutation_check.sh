#!/usr/bin/env bash
# SPDX-License-Identifier: Apache-2.0
#
# Mutation sanity check, the same pattern as open-crank's: a test that cannot
# fail proves nothing. Each mutation below breaks one behaviour that
# applications rely on, in a scratch copy of the tree; the tests must then
# fail.
#
# A mutation "survives" if the suite still passes: that behaviour is not
# really tested. A mutation is "stale" if its code pattern no longer exists:
# update the pattern when you change the code.
#
# The paced example (5 s of wall clock) is left out of every run here; the
# pacer's own tests cover it.
#
# Usage: tools/mutation_check.sh   (needs cmake, a C compiler, python3)
set -euo pipefail

ROOT=$(cd "$(dirname "$0")/.." && pwd)
WORK=$(mktemp -d)
trap 'rm -rf "$WORK"' EXIT
CTEST_ARGS=(--test-dir "$WORK/build" -E example_flywheel_paced)

tar -C "$ROOT" --exclude=./build --exclude='./build-*' --exclude=./.git --exclude=./.cache -cf - . | tar -C "$WORK" -xf -
cmake -S "$WORK" -B "$WORK/build" -DCMAKE_BUILD_TYPE=Debug >/dev/null
cmake --build "$WORK/build" -j >/dev/null
if ! ctest "${CTEST_ARGS[@]}" >/dev/null 2>&1; then
    echo "The unmutated tree already fails its tests; fix that first."
    exit 1
fi

killed=0
problems=0

# mutate <description> <file> <exact text> <replacement>
mutate() {
    local what=$1 file=$2 from=$3 to=$4
    local path="$WORK/$file"
    cp "$path" "$path.orig"
    if ! python3 - "$path" "$from" "$to" <<'EOF'
import sys
path, old, new = sys.argv[1:4]
text = open(path).read()
if old not in text:
    sys.exit(1)
open(path, "w").write(text.replace(old, new, 1))
EOF
    then
        echo "STALE     $what  (pattern not found in $file)"
        problems=$((problems + 1))
        mv "$path.orig" "$path"
        touch "$path" # newer than the mutated object, so the next build restores it
        return
    fi
    if ! cmake --build "$WORK/build" -j >/dev/null 2>&1; then
        echo "NO BUILD  $what  (the mutation does not compile)"
        problems=$((problems + 1))
    elif ctest "${CTEST_ARGS[@]}" >/dev/null 2>&1; then
        echo "SURVIVED  $what"
        problems=$((problems + 1))
    else
        failing=$( (ctest "${CTEST_ARGS[@]}" 2>/dev/null || true) | sed -n 's/^.*[0-9] - \([a-z0-9_]*\) (Failed).*$/\1/p' | tr '\n' ' ')
        echo "killed    $what  [${failing% }]"
        killed=$((killed + 1))
    fi
    mv "$path.orig" "$path"
    touch "$path" # newer than the mutated object, so the next build restores it
}

SIG=core/src/signals.c
SCN=core/src/scenario.c
PLAYER=core/src/player.c
CAN=core/src/can.c
TRACE=core/src/trace.c
PACER=host/src/pacer.c
ORACLE=core/src/oracle.c

mutate "signals: duplicate names accepted" $SIG \
    "if (strcmp(name, t->names[j]) == 0) {" "if (false) {"
mutate "signals: 'end' accepted as an application signal" $SIG \
    "if (strcmp(name, OTK_SIGNAL_END_NAME) == 0) {
            return fail(err, err_len, \"signal %zu: '%s' is reserved\", i, name);" \
    "if (false) {
            return fail(err, err_len, \"signal %zu: '%s' is reserved\", i, name);"
mutate "scenario: unknown signal names accepted" $SCN \
    "return fail(err, err_len, line_no, \"unknown signal '%s'\", f_signal);" "continue;"
mutate "scenario: decreasing times accepted" $SCN \
    "if (s->n > 0U && t < s->events[s->n - 1U].t) {" "if (false) {"
mutate "scenario: events after 'end' accepted" $SCN \
    "if (s->has_end) {" "if (false) {"
mutate "scenario: times truncated instead of rounded" $SCN \
    "(OtkNs)(t_s * 1e9 + 0.5)" "(OtkNs)(t_s * 1e9)"
mutate "scenario: a broken signal table is not checked" $SCN \
    "if (!otk_signal_table_check(signals, table_err, sizeof(table_err))) {" "if (false) {"
mutate "player: events apply one step late" $PLAYER \
    "s->events[p->next].t <= now" "s->events[p->next].t < now"
mutate "player: 'end' handed to the application" $PLAYER \
    "if (e->signal != OTK_SIGNAL_END && p->apply != NULL) {" "if (p->apply != NULL) {"
mutate "player: done before 'end'" $PLAYER \
    "now >= p->scenario->end" "now + 1U >= p->scenario->end"
mutate "can: 11-bit identifiers may exceed 11 bits" $CAN \
    "? OTK_CAN_EFF_MASK : OTK_CAN_SFF_MASK" "? OTK_CAN_EFF_MASK : OTK_CAN_EFF_MASK"
mutate "can: CAN FD lengths accepted on classic frames" $CAN \
    "if (!fd) {
        return false;" "if (false) {
        return false;"
mutate "can: queue overflow not counted" $CAN \
    "q->overflows++;" ";"
mutate "trace: a missing column goes unnoticed" $TRACE \
    "if (t->col < t->n_cols) {
        set_error" "if (false) {
        set_error"
mutate "trace: text that breaks the CSV accepted" $TRACE \
    "if (s == NULL || !csv_safe(s)) {" "if (s == NULL) {"
mutate "trace: values of the wrong kind accepted" $TRACE \
    "if (c->kind != kind) {" "if (false) {"
mutate "pacer: overruns not counted" $PACER \
    "p->overruns++;" ";"
mutate "pacer: deadline not advanced (runs unpaced)" $PACER \
    "p->next.tv_nsec += (long)(p->period % OTK_NS_PER_S);" ";"
mutate "verdicts: reaction written from the window start, not the event" $ORACLE \
    "ok = ok && print_ms(reaction_ns(c), fp) >= 0;" "ok = ok && print_ms(c->true_since_ns - c->from_ns, fp) >= 0;"

echo
echo "$killed mutations killed, $problems problems"
[[ $problems -eq 0 ]]
