#!/usr/bin/env bash
# SPDX-License-Identifier: Apache-2.0
# The hard driver's first measurements on a Tang Nano 20K, with the C host
# (otk-hard-probe), each run from a freshly programmed bridge (findings F-17):
#   cycle        back to back: round trip, the BL616's forwarding cycle
#   sweep        the round trip against when, in that cycle, a request is sent
#   locked L     every exchange sent so that its bytes are on the wire and L us of
#                margin are left before a forwarding instant: L = 200, 400, 600, 900
#   hold H       1 ms steps, the buffer kept near H: H = 6, 7, 8, 10, 12, 16,
#                locked with 600 us of margin, idle and with one busy loop per CPU
#
#   docs/measurements/2026-11-26-hard-driver/run.sh <open-tick build dir> <outdir> [cycle] [sweep] [locked] [hold]
#
# It records the USB topology of the BL616, the bitstream's md5 and the commit.
# The bitstream is built by gateware/tang_nano_20k/build.sh; the toolchain
# (and openFPGALoader) comes from tools/fpga_toolchain.sh.
set -euo pipefail
here="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"
root="$(cd -- "$here/../../.." && pwd)"
build="$(cd -- "${1:?open-tick build directory}" && pwd)"
out="${2:?output directory}"; shift 2
what=("$@"); [[ ${#what[@]} -gt 0 ]] || what=(cycle sweep locked hold)
probe="$build/otk-hard-probe"
[[ -x $probe ]] || { echo "run.sh: $probe not built" >&2; exit 2; }

eval "$("$root/tools/fpga_toolchain.sh")"
"$root/gateware/tang_nano_20k/build.sh" "$root/build-gateware" >/dev/null
fs="$root/build-gateware/bridge.fs"
uart=$(ls /dev/serial/by-id/usb-SIPEED_USB_Debugger_*-if01-port0 2>/dev/null | head -1)
[[ -n $uart ]] || { echo "run.sh: no SIPEED USB Debugger interface 1 in /dev/serial/by-id" >&2; exit 2; }
usbdev=$(basename "$(readlink -f "/sys/class/tty/$(basename "$(readlink -f "$uart")")/device/../..")")
hubs=$(tr -cd '.' <<<"${usbdev#*-}" | wc -c)
mkdir -p "$out"
{
    echo "date=$(date '+%Y-%m-%d %H:%M:%S %z')"
    echo "commit=$(git -C "$root" rev-parse --short HEAD)$(git -C "$root" diff --quiet HEAD -- || echo ' (with local changes)')"
    echo "cpus=$(nproc) kernel=$(uname -r)"
    echo "usb_path=$usbdev hubs_between_root_and_bl616=$hubs speed_mbps=$(cat "/sys/bus/usb/devices/$usbdev/speed")"
    echo "bitstream md5=$(md5sum "$fs" | cut -d' ' -f1)"
    echo "openFPGALoader=$(openFPGALoader --Version 2>&1 | head -1)"
} > "$out/meta.txt"
echo "run.sh: BL616 at USB $usbdev, $hubs hub(s); bitstream $(md5sum "$fs" | cut -c1-12)" >&2

busy=()
stop_busy() { for p in "${busy[@]}"; do kill "$p" 2>/dev/null || true; done; busy=(); }
trap stop_busy EXIT
start_busy() {
    for _ in $(seq "$(nproc)"); do env -u PYTHONPATH python3 -I -S -c 'while True: pass' & busy+=($!); done
    sleep 2
}
run() { # name, then the probe's arguments
    local name=$1; shift
    openFPGALoader -b tangnano20k "$fs" > "$out/$name.program.log" 2>&1
    sleep 1
    echo "== $name" >&2
    "$probe" --dev "$uart" --csv "$out/$name.csv" "$@" > "$out/$name.txt" 2>&1 || true
    grep -vE "^drained|^bridge:" "$out/$name.txt" >&2
}

for m in "${what[@]}"; do
    case $m in
    cycle)  run cycle --seconds 10 cycle ;;
    sweep)  run sweep --seconds 40 --walk-ns 50000 sweep ;;
    locked) for l in 200 400 600 900; do run "locked_${l}us" --seconds 30 --lead-ns "${l}000" locked; done ;;
    hold)
        for h in 6 7 8 10 12 16; do
            run "hold_H${h}" --seconds 60 --H "$h" --lead-ns 600000 hold
            start_busy
            run "hold_H${h}_load" --seconds 60 --H "$h" --lead-ns 600000 hold
            sed -i "1i host load: $(nproc) busy python3 loops (one per CPU), started before the run" "$out/hold_H${h}_load.txt"
            stop_busy
        done ;;
    *) echo "run.sh: unknown measurement $m" >&2; exit 2 ;;
    esac
done
(cd "$out" && md5sum ./* > md5.txt)
echo "run.sh: results in $out" >&2
