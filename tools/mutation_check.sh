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
# The SocketCAN mutations (kernel timestamps, own frames) need a CAN
# interface for test_socketcan: vcan0, or the one OTK_TEST_CAN_IF names.
# Without one they are skipped with a note, unless OTK_MUTATION_NEED_CAN=1
# (CI), which makes a missing interface a problem. Without root, a private
# vcan0 in a user namespace works too:
#
#     unshare -rn sh -c 'ip link add dev vcan0 type vcan && ip link set vcan0 mtu 72 &&
#                        ip link set up vcan0 && exec tools/mutation_check.sh'
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
LINK=core/src/link.c
FRAMELOG=core/src/framelog.c
FLYWHEEL=examples/flywheel/flywheel.c
SOCKETCAN=host/src/socketcan.c

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
mutate "link: drop loses nothing" $LINK \
    "if (fl->drop && matches(link, OTK_LINK_DROP, frame)) {" "if (false) {"
mutate "link: drop_one_in loses nothing" $LINK \
    "if (link->matched_one_in >= fl->drop_one_in) {" "if (false) {"
mutate "link: delay not applied" $LINK \
    "due = add_sat(due, fl->delay);" ";"
mutate "link: jitter not applied" $LINK \
    "due = add_sat(due, (fl->jitter == UINT64_MAX) ? r : r % (fl->jitter + 1U));" "(void)r;"
mutate "link: corrupt flips the last byte, not byte N" $LINK \
    "s->frame.data[fl->corrupt - 1U] ^= 0xFFU;" "s->frame.data[frame->len - 1U] ^= 0xFFU;"
mutate "link: bus_off lets pushed frames through" $LINK \
    "if (fl->bus_off) {
        link->bus_off_lost++;
        return lost_at_push(info, OTK_LINK_LOST_BUS_OFF, t);" "if (false) {
        link->bus_off_lost++;
        return lost_at_push(info, OTK_LINK_LOST_BUS_OFF, t);"
mutate "link: a frame may overtake an earlier one with the same id" $LINK \
    "if (same_id(&link->line[i].frame, frame) && link->line[i].due > due) {" "if (false) {"
mutate "link: an id filter matches every frame" $LINK \
    "return extended == flt->extended && ((f->id ^ flt->id) & flt->mask) == 0U;" "return true;"

# What a link reports for a frame log (push_ex / pop_ex).
mutate "link: a drop is reported as drop_one_in" $LINK \
    "return lost_at_push(info, OTK_LINK_LOST_DROP, t);" "return lost_at_push(info, OTK_LINK_LOST_DROP_ONE_IN, t);"
mutate "link: a corrupted frame is not reported" $LINK \
    "s->corrupted = true;" ";"
mutate "link: the push time is not kept" $LINK \
    "s->pushed = t;" "s->pushed = 0U;"
mutate "link: pop_ex hides a bus_off loss at delivery" $LINK \
    "info->lost = lost ? OTK_LINK_LOST_BUS_OFF : OTK_LINK_LOST_NONE;" "info->lost = OTK_LINK_LOST_NONE;"

# Frame log writer (docs/frame-log-format.md).
mutate "framelog: times with six decimals, not nine" $FRAMELOG \
    'return fprintf(fp, "%llu.%09llu",' 'return fprintf(fp, "%llu.%06llu",'
mutate "framelog: an 11-bit id printed with 8 digits" $FRAMELOG \
    "ext ? 8 : 3" "8"
mutate "framelog: a t off the step grid is accepted" $FRAMELOG \
    "if (row->t % log->step != 0U) {" "if (false) {"
mutate "framelog: a lost to_dut frame may carry a bus time" $FRAMELOG \
    "if (row->dir == OTK_FRAME_TO_DUT && is_lost(row->fault) && row->t_bus != OTK_FRAMELOG_NO_TIME) {" "if (false) {"
mutate "framelog: errors are not sticky" $FRAMELOG \
    "    if (log->error) {
        return false;
    }
    const OtkCanFrame *f = row->frame;" "    const OtkCanFrame *f = row->frame;"
mutate "framelog: a corrupted frame is logged as delivered unchanged" $FRAMELOG \
    "return info->corrupted ? OTK_FRAME_CORRUPT : OTK_FRAME_OK;" "return OTK_FRAME_OK;"
mutate "flywheel: a delivered frame's bus time is its send time" $FLYWHEEL \
    "log_frame(info.pushed, lost ? OTK_FRAMELOG_NO_TIME : now," "log_frame(info.pushed, lost ? OTK_FRAMELOG_NO_TIME : info.pushed,"
mutate "flywheel: --seed is ignored" $FLYWHEEL \
    "otk_link_init(&speed_link, OTK_CAN_QUEUE_MAX, link_seed);" "otk_link_init(&speed_link, OTK_CAN_QUEUE_MAX, LINK_SEED_DEFAULT);"

# SocketCAN: kernel timestamps and the socket's own frames (frame logs with
# bus_clock=kernel). Only test_socketcan can catch these, and it needs a CAN
# interface: it exits 77 (skipped) without one.
can_rc=0
"$WORK/build/tests/test_socketcan" >/dev/null 2>&1 || can_rc=$?
if [[ $can_rc -eq 0 ]]; then
    mutate "socketcan: SO_TIMESTAMPNS never enabled" $SOCKETCAN \
        "if (setsockopt(s->fd, SOL_SOCKET, SO_TIMESTAMPNS, &on, sizeof(on)) < 0) {
        return false;
    }" "(void)on;"
    mutate "socketcan: CAN_RAW_RECV_OWN_MSGS never set" $SOCKETCAN \
        "if (own_msgs && setsockopt(" "if (false && setsockopt("
    mutate "socketcan: bus_clock says host with kernel timestamps" $SOCKETCAN \
        "return s->timestamps ? OTK_BUS_CLOCK_KERNEL : OTK_BUS_CLOCK_HOST;" "return OTK_BUS_CLOCK_HOST;"
    mutate "socketcan: an own frame is not marked as own" $SOCKETCAN \
        "info->own = (msg.msg_flags & MSG_CONFIRM) != 0;" "info->own = false;"
    mutate "socketcan: a timestamp is received but not reported" $SOCKETCAN \
        "info->has_time = true;" ";"
    mutate "socketcan: the timestamp loses its nanoseconds" $SOCKETCAN \
        "info->time_ns = (uint64_t)ts.tv_sec * OTK_NS_PER_S + (uint64_t)ts.tv_nsec;" \
        "info->time_ns = (uint64_t)ts.tv_sec * OTK_NS_PER_S;"
    mutate "socketcan: a time before the anchor wraps around" $SOCKETCAN \
        "return (time_ns < anchor_ns) ? OTK_FRAMELOG_NO_TIME : (OtkNs)(time_ns - anchor_ns);" \
        "return (OtkNs)(time_ns - anchor_ns);"
    mutate "socketcan: the plain receive hands back the socket's own frames" $SOCKETCAN \
        "if (r != 1 || !info.own) {" "if (true) {"
elif [[ ${OTK_MUTATION_NEED_CAN:-0} == 1 ]]; then
    echo "NO CAN    socketcan mutations not run: test_socketcan exited $can_rc (no CAN interface?)"
    problems=$((problems + 1))
else
    echo "note      socketcan mutations skipped: no CAN interface (test_socketcan exited $can_rc)"
fi

# The host <-> bridge protocol codec (v0.4 step 1).
BRG=core/src/bridge.c
mutate "bridge: CRC starts at 0 instead of 0xFFFF" $BRG \
    "uint32_t crc = 0xFFFFu;" "uint32_t crc = 0x0000u;"
mutate "bridge: 16-bit fields big-endian" $BRG \
    "p[0] = (uint8_t)(v & 0xFFu);
    p[1] = (uint8_t)(v >> 8);" "p[1] = (uint8_t)(v & 0xFFu);
    p[0] = (uint8_t)(v >> 8);"
mutate "bridge: a failed candidate frame dropped whole" $BRG \
    "drop(p, 1); /* a valid frame may start inside this one */" "drop(p, need);"
mutate "bridge: reply's state byte not written" $BRG \
    "out[20] = m->state;" "(void)0;"
mutate "bridge: capture tick extended past the reference" $BRG \
    "if (t > ref) {" "if (t > ref + 1u) {"
mutate "bridge: phase increment truncated, not rounded" $BRG \
    "/ (double)tick_hz + 0.5;" "/ (double)tick_hz;"

echo
echo "$killed mutations killed, $problems problems"
[[ $problems -eq 0 ]]
