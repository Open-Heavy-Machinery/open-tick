#!/usr/bin/env bash
# SPDX-License-Identifier: Apache-2.0
# The hard driver inside the step loop, on a Tang Nano 20K: otk-flywheel
# --driver hard for 60 s (examples/flywheel/hard60.csv), H = 16 steps of
# 10 ms, its SPEED frames on a private vcan0, each run from a freshly
# programmed bridge (findings F-17):
#   quiet   nothing else running on the host (F-30: start it only when the
#           machine is otherwise idle)
#   load    one busy loop per CPU
#
#   docs/measurements/2026-11-27-hard-loop/run.sh <open-tick build dir> <outdir> [quiet] [load]
#
# Each run writes its trace, its frame log (bus_clock=bridge) and its budget
# file (declared and measured, docs/bridge-protocol.md "Budget of a hard
# run"). vcan0 is made in a user namespace for the run (no sudo); the board is
# programmed outside it. It records the USB topology of the BL616, the
# bitstream's md5 and the commit, and refuses a tree with local changes, so
# that the commit it cites is the code that ran.
set -euo pipefail
here="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"
root="$(cd -- "$here/../../.." && pwd)"
build="$(cd -- "${1:?open-tick build directory}" && pwd)"
out="${2:?output directory}"; shift 2
what=("$@"); [[ ${#what[@]} -gt 0 ]] || what=(quiet load)
fly="$build/otk-flywheel"
[[ -x $fly ]] || { echo "run.sh: $fly not built" >&2; exit 2; }
git -C "$root" diff --quiet HEAD -- || { echo "run.sh: the tree has local changes; commit first" >&2; exit 2; }
commit=$(git -C "$root" rev-parse HEAD)

eval "$("$root/tools/fpga_toolchain.sh")"
"$root/gateware/tang_nano_20k/build.sh" "$root/build-gateware" >/dev/null
fs="$root/build-gateware/bridge.fs"
md5=$(md5sum "$fs" | cut -d' ' -f1)
uart=$(ls /dev/serial/by-id/usb-SIPEED_USB_Debugger_*-if01-port0 2>/dev/null | head -1)
[[ -n $uart ]] || { echo "run.sh: no SIPEED USB Debugger interface 1 in /dev/serial/by-id" >&2; exit 2; }
usbdev=$(basename "$(readlink -f "/sys/class/tty/$(basename "$(readlink -f "$uart")")/device/../..")")
hubs=$(tr -cd '.' <<<"${usbdev#*-}" | wc -c)
mkdir -p "$out"
{
    echo "date=$(date '+%Y-%m-%d %H:%M:%S %z')"
    echo "commit=$commit"
    echo "cpus=$(nproc) kernel=$(uname -r)"
    echo "usb_path=$usbdev hubs_between_root_and_bl616=$hubs speed_mbps=$(cat "/sys/bus/usb/devices/$usbdev/speed")"
    echo "bitstream md5=$md5"
    echo "openFPGALoader=$(openFPGALoader --Version 2>&1 | head -1)"
    echo "scenario=examples/flywheel/hard60.csv H=16 step=10ms can=vcan0 (user namespace)"
} > "$out/meta.txt"
echo "run.sh: BL616 at USB $usbdev, $hubs hub(s); bitstream $(cut -c1-12 <<<"$md5")" >&2

busy=()
stop_busy() { for p in "${busy[@]}"; do kill "$p" 2>/dev/null || true; done; busy=(); }
trap stop_busy EXIT
start_busy() {
    for _ in $(seq "$(nproc)"); do env -u PYTHONPATH python3 -I -S -c 'while True: pass' & busy+=($!); done
    sleep 2
}
run() { # name
    local name=$1 rc=0
    openFPGALoader -b tangnano20k "$fs" > "$out/$name.program.log" 2>&1
    sleep 1
    echo "== $name" >&2
    unshare -rn sh -c 'ip link add dev vcan0 type vcan && ip link set up vcan0 && exec "$@"' sh \
        "$fly" "$root/examples/flywheel/hard60.csv" --quiet --driver hard --dev "$uart" --H 16 --can vcan0 \
        --trace "$out/$name.csv" --frames "$out/$name.frames.csv" --budget "$out/$name.budget.csv" \
        --bitstream-md5 "$md5" --commit "$commit" > "$out/$name.txt" 2>&1 || rc=$?
    echo "exit=$rc" >> "$out/$name.txt"
    grep -E "^otk-flywheel: hard run|^exit=" "$out/$name.txt" >&2
}

for m in "${what[@]}"; do
    case $m in
    quiet) run quiet ;;
    load)
        start_busy
        run load
        sed -i "1i host load: $(nproc) busy python3 loops (one per CPU), started before the run" "$out/load.txt"
        stop_busy ;;
    *) echo "run.sh: unknown run $m" >&2; exit 2 ;;
    esac
done
(cd "$out" && md5sum ./* > md5.txt)
echo "run.sh: results in $out" >&2
