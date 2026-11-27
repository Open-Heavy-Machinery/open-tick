# REALTIME → MONOTONIC with wide samples dropped, on the board (2026-11-27)

The two runs of [2026-11-27-hard-loop/](../2026-11-27-hard-loop/) again, after REALTIME → MONOTONIC began to read a wide sample again and drop it if it stays wide ([bridge-protocol.md](../../bridge-protocol.md#5-can-in-a-hard-run) §5). In the earlier quiet run, one interrupted read set that error to 19.3 µs, over its 10 µs budget.

## Setup

The same as [2026-11-27-hard-loop/](../2026-11-27-hard-loop/), except the commit:
- **Board:** Tang Nano 20K, bitstream md5 `6659c249a3a8c779c0f7bb9cac6e6fa5` (unchanged), loaded into SRAM before **each** run.
- **Link:** the BL616 on a root port (`3-2`, 480 Mbit/s, no hub), UART at 3 Mbaud.
- **Host:** host B, 20 CPUs, Linux 6.8, normal priority. `otk-flywheel` from commit `7c15eae`, Debug build.
- **Run:** `examples/flywheel/hard60.csv`, 6000 steps of 10 ms (60 s), H = 16, margin 600 µs, SPEED frames on a `vcan0` in a user namespace.
- **quiet:** nothing else running on the host. **load:** one busy loop per CPU (20), started before the run.
- **Repeat it with:** `run.sh <open-tick build dir> <outdir>`.

## Contents

| File | What |
|---|---|
| `quiet.txt`, `load.txt` | `otk-flywheel`'s report and exit code |
| `quiet.budget.csv`, `load.budget.csv` | the budget, declared and measured (§9), with the new row `realtime_to_monotonic_rejected` |
| `meta.txt` | date, commit, host, USB path, bitstream md5 |
| `raw-csv.md5` | md5 of each run's trace and frame log. They are kept by the maintainers, not in git |
| `run.sh` | the script that made the runs |

## Results

| Quantity | Budget | quiet | load (20 busy loops) | quiet, before | load, before |
|---|---|---|---|---|---|
| Result, exit code | | `ok`, 0 | `ok`, 0 | `ok`, 0 | `ok`, 0 |
| Underruns, late setpoints, wrong applied setpoints | 0 | 0, 0, 0 (of 8 965 checked) | 0, 0, 0 (of 8 081 checked) | 0, 0, 0 | 0, 0, 0 |
| Lowest buffer fill | | 16 | 15 | 16 | 15 |
| Exchanges | | 8 970 | 8 086 | 9 003 | 7 877 |
| Round trip p50 / p99 / max | p99 ≤ 1.3 ms | 898 µs / 1.08 ms / 6.97 ms | 909 µs / **7.04 ms** / 11.66 ms | 902 µs / 1.09 ms / 6.99 ms | 885 µs / **7.20 ms** / 10.75 ms |
| DUT-to-plant delay | ≤ 161.3 ms | 161.08 ms | **167.04 ms** | 161.09 ms | **167.20 ms** |
| Clock mapping bound | ≤ 500 µs | 467 µs | **1 469 µs** | 460 µs | 355 µs |
| REALTIME → MONOTONIC | ≤ 10 µs | 1.4 µs | 0.2 µs | **19.3 µs** | 0.5 µs |
| REALTIME → MONOTONIC samples dropped | | 0 | 0 | | |
| `bus_clock_err_ns` | ≤ 510 µs | 468 µs | **1 469 µs** | 480 µs | 356 µs |
| CAN send error p50 / p99 / max | p99 ≤ 600 µs | 351 / 389 µs / 3.14 ms | 298 µs / **5.01 ms** / 8.68 ms | 349 / 401 µs / 2.68 ms | 287 µs / **5.09 ms** / 7.51 ms |
| Latest planned send | | 787 µs | 5.08 ms | 110 µs | 5.56 ms |
| Forwarding cycle | | 5 999.5 µs | 6 000.1 µs | 5 999.3 µs | 6 001.0 µs |

"Before" is [2026-11-27-hard-loop/](../2026-11-27-hard-loop/). Rows above their budget are in bold; the budget files mark them `within=no`.

- **On the quiet host, every row is within its budget.** REALTIME → MONOTONIC is 1.4 µs, against 19.3 µs before. No sample had to be dropped, so no read in this run stayed wide after its retries. The budget does not count retries, so whether any read was retried is not recorded.
- **Both runs held H,** and the plant's trace is the same, byte for byte, in both runs and in the earlier ones (`raw-csv.md5`).
- **Under load, the clock mapping's bound was over its budget: 1 469 µs,** against 355 µs in the earlier load run, and `bus_clock_err_ns` with it. The width rule does not touch the mapping (§4). The budget file records only the run's largest bound, so when and why it grew is not known. The round trip and the CAN send error are over their budget, as §9 expects under load.
- **On the quiet host, two CAN frames of 6 000 were over 600 µs** (the worst 3.14 ms, at 2.11 s), as one was in the earlier quiet run. No planned send was more than 787 µs late, so most of the 3.14 ms came after the send was planned; the run's files do not show why.
