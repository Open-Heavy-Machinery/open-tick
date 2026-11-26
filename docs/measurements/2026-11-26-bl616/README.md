# The Tang Nano 20K's USB link, measured (2026-11-26)

This folder is the evidence for open-tick v0.4's exit criterion 1: **the host-to-bridge latency and throughput, measured and documented before anything depends on them** ([ROADMAP.md](../../../ROADMAP.md), v0.4). The protocol built on these numbers is [bridge-protocol.md](../../bridge-protocol.md).

It is **spike code, not open-tick code.**
- It is kept as it ran, so that the numbers can be checked and repeated.
- It is not built or tested by CI, and nothing in open-tick depends on it.

## Setup

- **Board:** Tang Nano 20K (Gowin GW2AR-LV18QN88C8/I7) and its on-board BL616 USB debugger, Sipeed's firmware, which presents itself as an FT2232. Bitstreams were loaded into SRAM only, never into flash.
- **Link:** the BL616's UART interface (interface 1), 8N1.
- **Host:** Python 3 with pyserial 3.5, on Linux. Host A has 22 CPUs; host B has 20 CPUs, Ubuntu 22.04, Python 3.10.12.
- **Toolchain:** yosys → nextpnr-himbaechel → gowin_pack from pip (YoWASP), with every file pinned by SHA-256 in [requirements.lock](requirements.lock). openFPGALoader 1.1.1, built from its release tarball (SHA-256 `ca965f933c52a2a9dbb318df4d4de70fac5f095a8e64523f81036ab467a4b567`), only programs the board.
- **Reproducible bitstream:** `spike2/build.sh 3000000` gave the same bitstream on both hosts, md5 `f1d6afb79fe0b2791d68a22b6c75c2a4`.

## Contents

| Path | What |
|---|---|
| `spike1/link_*.txt` | First spike, host A: UART echo at 115 200 to 3 375 000 baud. 1000 round trips of 8 B, then paced throughput. Its gateware is not kept here; the results are. |
| `spike2/pingpong.v`, `tb_pingpong.v`, `tang_nano_20k.cst`, `build.sh` | Second spike: a host-initiated ping-pong bridge with a 1 ms setpoint FIFO and a tick counter, its testbench, pins and build. `build.sh` expects the venv at `~/.cache/ohm-fpga/venv`. |
| `spike2/pingpong_bench.py` | The host side: `sizes` (M1), `sync` (M2), `hold` (M3). |
| `spike2/m1_sizes.txt`, `m2_sync*.txt`, `m3_hold_*.txt`, `m3_mismatch_check.txt` | Host A's reports. |
| `host-b/m3_hold_*.txt` | M3 repeated on host B. USB serial numbers are masked as `<serial>`. |
| `raw-csv.md5` | md5 of every raw CSV behind these reports. The CSVs (about 4.7 MB) are kept by the maintainers, not in git. |
| `requirements.lock` | The pinned toolchain. |

## What was measured, and the result

| Measurement | Result | Finding |
|---|---|---|
| Throughput per exchange (M1) | about 448 B each way in one exchange of about 6 ms; a reply above about 512 B costs about 6 ms more per 512 B | F-13 |
| Round trip (M2, one exchange every 10 ms) | p1 0.26 ms, p50 3.3 ms, p99 6.2 ms; back to back always about 6 ms; 26 µs inside the bridge | F-14 |
| Clock mapping (M2) | lowest-delay samples: residual p99 45–85 µs; drift −5.6 to −6.1 ppm | F-15 |
| Holding a 1 ms setpoint buffer (M3) | H = 16 ms: 0 underruns idle and with every CPU busy, on both hosts; H = 8 ms fails under load | F-16 |
| Full duplex (first spike) | sustained traffic both ways loses data and can hang the BL616 | F-1 |

The tables are in [bridge-protocol.md](../../bridge-protocol.md#measured-link-bl616).

## Repeating M3

Every run starts from a freshly programmed bridge: the counters of an earlier run would hide underruns (F-17).

```sh
spike2/build.sh 3000000                       # md5 must be f1d6afb79fe0b2791d68a22b6c75c2a4
openFPGALoader -b tangnano20k ~/.cache/ohm-fpga/build/pingpong_3000000/pingpong.fs
python3 spike2/pingpong_bench.py hold --dev /dev/serial/by-id/<the BL616's interface 1> \
    --baud 3000000 --horizon 16 --seconds 60 --csv h16.csv --report h16.txt
```

For the loaded runs, start one busy loop per CPU before the run, for example `python3 -c 'while True: pass' &` repeated `nproc` times.
