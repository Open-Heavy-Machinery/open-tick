#!/usr/bin/env bash
# SPDX-License-Identifier: Apache-2.0
# Simulates the Tang Nano 20K bridge with Icarus Verilog:
#   1. tb_units: UART, CRC, sigma-delta, rotary pattern, capture
#   2. tb_bridge: the whole bridge over its UART, scripted and judged by
#      open-tick's codec (bridge_vectors.c), so the gateware and the C side
#      of the protocol cannot drift apart
#
#   gateware/tang_nano_20k/sim/run.sh <open-tick build dir>   (needs iverilog >= 11 and otk-bridge-vectors built)
set -euo pipefail
here="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"
rtl="$here/../rtl"
build="$(cd -- "${1:?open-tick build directory}" && pwd)"
vectors="$build/tests/otk-bridge-vectors"
[[ -x $vectors ]] || { echo "run.sh: $vectors not built (cmake --build $build --target otk-bridge-vectors)" >&2; exit 2; }
command -v iverilog >/dev/null || { echo "run.sh: iverilog not found" >&2; exit 2; }
work="$build/gateware-sim"
mkdir -p "$work"
status=0

echo "== tb_units"
iverilog -g2005 -Wall -I"$rtl" -o "$work/tb_units.vvp" "$here/tb_units.v" \
    "$rtl/uart_rx.v" "$rtl/uart_tx.v" "$rtl/sigma_delta.v" "$rtl/rotary.v" "$rtl/capture.v" 2>&1 | grep -v "timescale" || true
vvp -n "$work/tb_units.vvp" | tee "$work/tb_units.log" | grep -E "FAIL|SYNC_DELAY|tb_units:"
grep -qE "tb_units: [0-9]+ checks, 0 failed" "$work/tb_units.log" || status=1

echo "== tb_bridge"
"$vectors" gen "$work/stimulus.txt"
iverilog -g2005 -Wall -I"$rtl" -o "$work/tb_bridge.vvp" "$here/tb_bridge.v" "$rtl/top.v" "$rtl/bridge_core.v" \
    "$rtl/frame_rx.v" "$rtl/uart_rx.v" "$rtl/uart_tx.v" "$rtl/rotary.v" "$rtl/capture.v" "$rtl/sigma_delta.v" 2>&1 \
    | grep -v "timescale" || true
vvp -n "$work/tb_bridge.vvp" "+stim=$work/stimulus.txt" "+out=$work/tb_bridge.log" >/dev/null
echo "$(grep -c '^rx ' "$work/tb_bridge.log") replies logged"
"$vectors" check "$work/tb_bridge.log" || status=1

exit $status
