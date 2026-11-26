#!/usr/bin/env bash
# Build pingpong.v for the Tang Nano 20K with the pip toolchain of spike 1
# (~/.cache/ohm-fpga/venv, ../2026-11-26_tang_nano_spike/requirements.lock).
#   build.sh <baud> [outdir]
set -euo pipefail
here="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"
baud="${1:?baud}"
out="${2:-$HOME/.cache/ohm-fpga/build}/pingpong_${baud}"
f_clk=27000000
clks=$(( (f_clk + baud / 2) / baud ))
v="$HOME/.cache/ohm-fpga/venv/bin"
mkdir -p "$out"
echo "pingpong, baud $baud, CLKS_PER_BIT $clks, out $out"
env -u PYTHONPATH "$v/yowasp-yosys" -q -l "$out/yosys.log" -p "read_verilog $here/pingpong.v; chparam -set CLKS_PER_BIT $clks pingpong; synth_gowin -top pingpong -json $out/synth.json"
env -u PYTHONPATH "$v/yowasp-nextpnr-himbaechel-gowin" --json "$out/synth.json" --write "$out/pnr.json" \
  --device GW2AR-LV18QN88C8/I7 --vopt family=GW2A-18C --vopt cst="$here/tang_nano_20k.cst" \
  --freq 27 --seed 1 --log "$out/nextpnr.log" -q
env -u PYTHONPATH PYTHONNOUSERSITE=1 "$v/gowin_pack" -d GW2A-18C -o "$out/pingpong.fs" "$out/pnr.json"
grep -E "Max frequency for clock" "$out/nextpnr.log" | tail -1
grep -iE "DPX9|SDPX9|DPB|SDPB" "$out/yosys.log" | tail -3 || true
md5sum "$out/pingpong.fs"
