#!/usr/bin/env bash
# Repeat spike 2's measurements on this machine, each from a freshly
# programmed FPGA (findings F-17):
#   M1 sizes: up to 448 B (200 exchanges each) and the large sizes (100 each)
#   M2 sync:  one exchange every 10 ms for 60 s, then m2_analysis.py
#   M3 hold:  H = 8, 16, 32 steps, idle and with one busy loop per CPU, 60 s each
#
#   docs/measurements/2026-11-26-bl616/rerun_spike2.sh <outdir> [m1] [m2] [m3]     (default: all three)
#
# It records the USB topology of the Tang's BL616 (sysfs path, hubs between
# it and the root port, link speed): a hub on the path changes the link's
# latency, and a result without it cannot be compared.
# Toolchain and pyserial come from tools/fpga_toolchain.sh, pinned, installed
# into ~/.cache/ohm-fpga: the directory spike2/build.sh was written for (it is
# kept as it ran). The system Python is used only for the busy loops, with no
# packages. The bitstream must be spike 2's (md5 below), or nothing runs.
# The UART is found by its USB id, never by a local symlink.
set -euo pipefail
here="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"
root="$(cd -- "$here/../../.." && pwd)"
spike="$here/spike2"
out="${1:?outdir}"; shift
what=("$@"); [[ ${#what[@]} -gt 0 ]] || what=(m1 m2 m3)
BAUD=3000000
FS_MD5=f1d6afb79fe0b2791d68a22b6c75c2a4
SECONDS_PER_RUN=60
LARGE_PAIRS=1024/0,0/1024,1024/1024,2048/2048,4096/4096

eval "$(OTK_FPGA_DIR="$HOME/.cache/ohm-fpga" "$root/tools/fpga_toolchain.sh")"
"$spike/build.sh" "$BAUD" >/dev/null
fs=$HOME/.cache/ohm-fpga/build/pingpong_$BAUD/pingpong.fs
got=$(md5sum "$fs" | cut -d' ' -f1)
[[ $got == "$FS_MD5" ]] || { echo "rerun_spike2.sh: bitstream md5 $got, expected $FS_MD5" >&2; exit 1; }

uart=$(ls /dev/serial/by-id/usb-SIPEED_USB_Debugger_*-if01-port0 2>/dev/null | head -1)
[[ -n $uart ]] || { echo "rerun_spike2.sh: no SIPEED USB Debugger interface 1 in /dev/serial/by-id" >&2; exit 2; }
# The USB device behind the tty: /sys/bus/usb/devices/<bus>-<port>[.<port>...]
usbdev=$(basename "$(readlink -f "/sys/class/tty/$(basename "$(readlink -f "$uart")")/device/../..")")
hubs=$(tr -cd '.' <<<"${usbdev#*-}" | wc -c)
mkdir -p "$out"
{
    echo "date=$(date '+%Y-%m-%d %H:%M:%S %z')"
    echo "cpus=$(nproc) kernel=$(uname -r)"
    echo "uart=$(basename "$uart" | sed -E 's/_[0-9A-Za-z]+(-if0[0-9])/_<serial>\1/') -> $(readlink -f "$uart")"
    echo "usb_path=$usbdev hubs_between_root_and_bl616=$hubs speed_mbps=$(cat "/sys/bus/usb/devices/$usbdev/speed")"
    echo "bitstream md5=$got"
    echo "openFPGALoader=$(openFPGALoader --Version 2>&1 | head -1)"
    sed 's/^/toolchain: /' "$HOME/.cache/ohm-fpga/MANIFEST.txt"
    echo "lsusb -t:"
    lsusb -t
} > "$out/meta.txt"
echo "rerun_spike2.sh: BL616 at USB $usbdev, $hubs hub(s) on the path" >&2

busy=()
stop_busy() { for p in "${busy[@]}"; do kill "$p" 2>/dev/null || true; done; busy=(); }
trap stop_busy EXIT
program() { openFPGALoader -b tangnano20k "$fs" > "$out/$1.program.log" 2>&1; sleep 1; }
bench() { python -I "$spike/pingpong_bench.py" "$@" --dev "$uart" --baud "$BAUD"; }

for m in "${what[@]}"; do
    case $m in
    m1)
        echo "== m1_sizes" >&2
        program m1_sizes
        bench sizes --n 200 --csv "$out/m1_sizes.csv" --report /dev/null >/dev/null
        program m1_sizes_large
        bench sizes --n 100 --pairs "$LARGE_PAIRS" --csv "$out/m1_sizes_large.csv" --report /dev/null >/dev/null
        cat "$out/m1_sizes.csv" "$out/m1_sizes_large.csv" >&2
        ;;
    m2)
        echo "== m2_sync" >&2
        program m2_sync
        bench sync --period-ms 10 --seconds "$SECONDS_PER_RUN" --csv "$out/m2_sync.csv" --report "$out/m2_sync.txt" >/dev/null
        python -I "$spike/m2_analysis.py" "$out/m2_sync.csv" > "$out/m2_sync_analysis.txt"
        cat "$out/m2_sync_analysis.txt" >&2
        ;;
    m3)
        for H in 8 16 32; do
            for load in idle load; do
                name=m3_hold_H${H}$([[ $load == load ]] && echo _load || true)
                echo "== $name" >&2
                program "$name"
                if [[ $load == load ]]; then
                    for _ in $(seq "$(nproc)"); do
                        env -u PYTHONPATH python3 -I -S -c 'while True: pass' & busy+=($!)
                    done
                    sleep 2
                fi
                bench hold --horizon "$H" --seconds "$SECONDS_PER_RUN" --csv "$out/$name.csv" --report "$out/$name.txt" >/dev/null
                if [[ $load == load ]]; then
                    sed -i "2i host load: $(nproc) busy python3 loops (one per CPU), started before the run" "$out/$name.txt"
                fi
                stop_busy
                grep -E "underruns|FIFO fill" "$out/$name.txt" >&2
            done
        done
        ;;
    *) echo "rerun_spike2.sh: unknown measurement '$m'" >&2; exit 2 ;;
    esac
done
(cd "$out" && md5sum ./* > md5.txt)
echo "rerun_spike2.sh: results in $out" >&2
