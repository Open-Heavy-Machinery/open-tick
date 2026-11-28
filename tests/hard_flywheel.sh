#!/usr/bin/env bash
# SPDX-License-Identifier: Apache-2.0
#
# otk-flywheel --driver hard against otk-fake-bridge, without a board:
#   ok        the run ends ok (exit 0) and writes its trace and budget;
#   aborted   the bridge falls silent at step 60: exit 4, result=aborted;
#   used      a bridge that is not fresh is refused (exit 1);
#   fidelity  the ok run passes examples/flywheel/fidelity.csv against the
#             Tang's gateware/tang_nano_20k/fidelity.csv; a machine that asks
#             more is refused with exit 5, naming the signal and the column,
#             before the bridge is opened.
# With a CAN interface (vcan0, or OTK_TEST_CAN_IF), the ok run also sends its
# SPEED frames there and checks the frame log on the bridge's clock.
#
#   tests/hard_flywheel.sh <build dir> <scenario.csv> <out dir>
set -euo pipefail
build=${1:?build dir}; scenario=${2:?scenario}; out=${3:?out dir}
mkdir -p "$out"
fake="$build/tests/otk-fake-bridge"
fly="$build/otk-flywheel"
fail() { echo "hard_flywheel: $*" >&2; exit 1; }
root="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)"
fid=(--fidelity "$root/examples/flywheel/fidelity.csv" --bridge-fidelity "$root/gateware/tang_nano_20k/fidelity.csv")

serve() { # link, seconds, [silent_from]
    rm -f "$1"
    "$fake" "$@" > "$1.log" 2>&1 &
    for _ in $(seq 50); do [[ -L $1 ]] && return 0; sleep 0.1; done
    fail "the fake bridge did not start"
}

# ok, with CAN if there is an interface
ifname=${OTK_TEST_CAN_IF:-vcan0}
can=()
if ip link show "$ifname" >/dev/null 2>&1; then can=(--can "$ifname" --frames "$out/hard.frames.csv"); fi
serve "$out/bridge.pty" 30
rc=0
"$fly" "$scenario" --quiet --driver hard --dev "$out/bridge.pty" --trace "$out/hard.csv" \
    --bitstream-md5 fake --commit test "${fid[@]}" "${can[@]}" > "$out/hard.txt" 2>&1 || rc=$?
rm -f "$out/bridge.pty"; wait
cat "$out/hard.txt"
[[ $rc -eq 0 ]] || fail "the ok run exited $rc"
grep -q '^otk-flywheel: fidelity: every signal is within what the bridge guarantees (lead 160000000 ns, DUT-to-plant delay 184200000 ns' \
    "$out/hard.txt" || fail "fidelity: the ok run's check"
head -1 "$out/hard.budget.csv" | grep -q '^# otk-hard-budget 1; result=ok; H=16; step_ns=10000000;' || fail "budget metadata"
grep -qx 'underruns,0,0,steps,yes' "$out/hard.budget.csv" || fail "budget: underruns"
grep -qx 'plant_lead_ns,160000000,160000000,ns,yes' "$out/hard.budget.csv" || fail "budget: plant lead"
grep -Eqx 'realtime_to_monotonic_rejected,,[0-9]+,samples,' "$out/hard.budget.csv" || fail "budget: REALTIME samples dropped"
[[ $(wc -l < "$out/hard.csv") -gt 150 ]] || fail "the trace is short"
if [[ ${#can[@]} -gt 0 ]]; then
    head -1 "$out/hard.frames.csv" | grep -q '; bus_clock=bridge; bus_clock_err_ns=[0-9][0-9]*;' || fail "frame log metadata"
    [[ $(grep -c ',to_dut,,100,' "$out/hard.frames.csv") -gt 150 ]] || fail "frame log: SPEED frames"
    grep -q '^can_tx_err_p99_ns,1100000,[0-9]' "$out/hard.budget.csv" || fail "budget: CAN"
    echo "hard_flywheel: CAN on $ifname checked"
else
    echo "hard_flywheel: no CAN interface $ifname; the CAN part was not checked"
fi

# aborted
serve "$out/bridge.pty" 30 60
rc=0
"$fly" "$scenario" --quiet --driver hard --dev "$out/bridge.pty" --trace "$out/aborted.csv" > "$out/aborted.txt" 2>&1 || rc=$?
rm -f "$out/bridge.pty"; wait
[[ $rc -eq 4 ]] || { cat "$out/aborted.txt"; fail "the aborted run exited $rc, not 4"; }
head -1 "$out/aborted.budget.csv" | grep -q 'result=aborted' || fail "aborted budget"

# a used bridge: the first run left it stopped
serve "$out/bridge.pty" 30
"$fly" "$scenario" --quiet --driver hard --dev "$out/bridge.pty" --trace "$out/first.csv" --allow-reused-bridge >/dev/null 2>&1 \
    || fail "the first run on the reused fake failed"
rc=0
"$fly" "$scenario" --quiet --driver hard --dev "$out/bridge.pty" --trace "$out/used.csv" > "$out/used.txt" 2>&1 || rc=$?
rm -f "$out/bridge.pty"; wait
[[ $rc -eq 1 ]] && grep -q "not freshly programmed" "$out/used.txt" || { cat "$out/used.txt"; fail "a used bridge was not refused ($rc)"; }
echo "hard_flywheel: ok, aborted and used-bridge runs as expected"

# fidelity: a machine that needs its injector pulse within 100 ms of the DUT
# is refused (this run's delay is 161.3 ms), before the bridge is opened: the
# device does not exist, and the exit code is still 5
cat > "$out/too_fast.csv" <<'CSV'
# otk-fidelity 1; side=machine; source=hard_flywheel.sh
signal,direction,transport,max_rate_hz,resolution_ns,latency_ns,jitter_ns,logic_v
wheel,to_dut,rotary,2900,100,,,3.3
inj1,from_dut,capture,10,,100000000,,3.3
CSV
rc=0
"$fly" "$scenario" --quiet --driver hard --dev "$out/no-such-bridge" --trace "$out/refused.csv" \
    --fidelity "$out/too_fast.csv" --bridge-fidelity "$root/gateware/tang_nano_20k/fidelity.csv" > "$out/refused.txt" 2>&1 || rc=$?
cat "$out/refused.txt"
[[ $rc -eq 5 ]] || fail "fidelity: the refused run exited $rc, not 5"
grep -q 'signal inj1 (line 4), column latency_ns: requires at most 100000000 ns; the bridge.s capture0 gives 184200000 ns' \
    "$out/refused.txt" || fail "fidelity: the refusal does not name the signal and the column"
[[ ! -e $out/refused.csv ]] || fail "fidelity: a refused run wrote a trace"
rc=0
"$fly" "$scenario" --quiet --driver hard --dev "$out/no-such-bridge" --trace "$out/refused.csv" \
    --fidelity "$out/too_fast.csv" > /dev/null 2>&1 || rc=$?
[[ $rc -eq 2 ]] || fail "fidelity: --fidelity without --bridge-fidelity exited $rc, not 2"
echo "hard_flywheel: fidelity checked"
