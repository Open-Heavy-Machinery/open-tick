#!/usr/bin/env bash
# SPDX-License-Identifier: Apache-2.0
# Breaks one behaviour of the gateware at a time, in a scratch copy, and
# checks that sim/run.sh fails: every mutation must be killed.
#
#   gateware/tang_nano_20k/sim/mutations.sh <open-tick build dir>
set -euo pipefail
here="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"
gw="$(cd -- "$here/.." && pwd)"
build="$(cd -- "${1:?open-tick build directory}" && pwd)"
work=$(mktemp -d)
trap 'rm -rf "$work"' EXIT
cp -r "$gw" "$work/tang_nano_20k"

killed=0
problems=0
# mutate <description> <file under rtl/> <exact text> <replacement>
mutate() {
    local what=$1 file=$work/tang_nano_20k/rtl/$2 from=$3 to=$4
    cp "$file" "$file.orig"
    if ! python3 - "$file" "$from" "$to" <<'PY'
import sys
path, old, new = sys.argv[1:4]
text = open(path).read()
if old not in text:
    sys.exit(1)
open(path, "w").write(text.replace(old, new, 1))
PY
    then
        echo "STALE     $what"
        problems=$((problems + 1))
    elif "$work/tang_nano_20k/sim/run.sh" "$build" >/dev/null 2>&1; then
        echo "SURVIVED  $what"
        problems=$((problems + 1))
    else
        echo "killed    $what"
        killed=$((killed + 1))
    fi
    mv "$file.orig" "$file"
}

mutate "crc: initial value 0 instead of 0xFFFF" crc16.vh "c = crc ^ {b, 8'h00};" "c = (crc ^ 16'hFFFF) ^ {b, 8'h00};"
mutate "frame_rx: CRC never fails" frame_rx.v "msg_crc_ok <= ({byte_data, crc_lo} == crc);" "msg_crc_ok <= 1'b1;"
mutate "core: an underrun applies zero instead of holding" bridge_core.v "end else if (underruns != 32'hFFFFFFFF) begin" "end else if (1'b1) begin applied <= 96'd0;"
mutate "core: a retransmission is applied again" bridge_core.v "end else if (seq_valid && m_seq == last_seq) begin" "end else if (1'b0) begin"
mutate "core: gaps accepted" bridge_core.v "end else if (sh[167:136] != next_q) begin" "end else if (1'b0) begin"
mutate "core: late setpoints queued" bridge_core.v "if (state == S_RUNNING && s_i < pend) begin" "if (1'b0) begin"
mutate "core: no link timeout" bridge_core.v "if (since_rx + 16'd1 >= link_timeout) begin
                    state <= S_ABORTED;" "if (1'b0) begin
                    state <= S_ABORTED;"
mutate "core: step 0 begins when the start frame arrives" bridge_core.v "next_boundary <= m_t2 + {32'd0, step_ticks};" "next_boundary <= tick + 64'd8;"
mutate "core: stop does not stop" bridge_core.v "                        state <= S_STOPPED;" "                        state <= S_RUNNING;"
mutate "core: start refused only on the state" bridge_core.v "if (state != S_CONFIGURED || fill == 0 || head_step != 32'd0) begin" "if (state != S_CONFIGURED) begin"
mutate "core: replies say idle whatever the state" bridge_core.v "{5'd0, state}, step, tick, m_t2};" "{5'd0, 3'd0}, step, tick, m_t2};"
mutate "capture: width one tick long" capture.v "done[c]    <= {tick[31:0] - start[c], start[c], c[7:0]};" "done[c]    <= {tick[31:0] - start[c] + 32'd1, start[c], c[7:0]};"
mutate "capture: polarity ignored" capture.v "wire [N-1:0] active = s2 ^ ~polarity[N-1:0];" "wire [N-1:0] active = s2;"
mutate "rotary: the gap's teeth are not missing" rotary.v "wire        tooth = (idx < (teeth - missing)) && !second_half;" "wire        tooth = !second_half;"
mutate "rotary: outputs keep moving at rest" rotary.v "        end else if (run) begin
            phase <= phase + {16'd0, inc};" "        end else begin
            phase <= phase + 48'd1000000000;"
mutate "sigma-delta: the carry dropped" sigma_delta.v "out <= acc[16];" "out <= acc[15];"
mutate "uart_rx: bits sampled at their edge" uart_rx.v "if (cnt == (CLKS_PER_BIT - 1) / 2) begin" "if (cnt == 0) begin"

echo
echo "$killed gateware mutations killed, $problems problems"
[[ $problems -eq 0 ]]
