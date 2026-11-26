#!/usr/bin/env bash
# SPDX-License-Identifier: Apache-2.0
# Builds the Tang Nano 20K bridge: yosys -> nextpnr-himbaechel -> gowin_pack,
# with the pinned open toolchain (tools/fpga_toolchain.sh, every file pinned
# by SHA-256). Fixed seed: the same sources give the same bitstream.
#
#   gateware/tang_nano_20k/build.sh [outdir]      (default build-gateware/ at the top of the repository)
#
# Prints the resources used and the maximum frequency, and fails if the
# design does not meet 27 MHz or if block RAM is missing (findings F-19: logic
# whose data reaches no output is removed without a warning).
set -euo pipefail
here="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"
root="$(cd -- "$here/../.." && pwd)"
out="${1:-$root/build-gateware}"
eval "$("$root/tools/fpga_toolchain.sh" --synthesis-only)"
mkdir -p "$out"
rtl="$here/rtl"
yowasp-yosys -q -l "$out/yosys.log" -p "read_verilog -I$rtl $rtl/top.v $rtl/bridge_core.v $rtl/frame_rx.v $rtl/uart_rx.v $rtl/uart_tx.v $rtl/rotary.v $rtl/capture.v $rtl/sigma_delta.v; synth_gowin -top top -json $out/synth.json"
yowasp-nextpnr-himbaechel-gowin --json "$out/synth.json" --write "$out/pnr.json" \
    --device GW2AR-LV18QN88C8/I7 --vopt family=GW2A-18C --vopt cst="$here/tang_nano_20k.cst" \
    --freq 27 --seed 1 --log "$out/nextpnr.log" -q
PYTHONNOUSERSITE=1 gowin_pack -d GW2A-18C -o "$out/bridge.fs" "$out/pnr.json"

fmax=$(grep -E "Max frequency for clock" "$out/nextpnr.log" | tail -1)
echo "$fmax"
grep -E "^Info:\s+(LUT4|DFF|BSRAM|RAM16SDP4|MULT18X18):" "$out/nextpnr.log" | sed 's/^Info:\s*//'
bram=$(sed -nE 's/^Info:\s+BSRAM:\s+([0-9]+)\/.*/\1/p' "$out/nextpnr.log" | tail -1)
md5sum "$out/bridge.fs"
grep -q "PASS at 27" <<<"$fmax" || { echo "build.sh: 27 MHz not met" >&2; exit 1; }
# The frame buffer (512 x 8) and the setpoint FIFO (64 x 96) are 4 block RAMs.
[[ ${bram:-0} -ge 4 ]] || { echo "build.sh: ${bram:-0} block RAMs, expected 4: the setpoint FIFO or the frame buffer was removed (F-19)" >&2; exit 1; }
