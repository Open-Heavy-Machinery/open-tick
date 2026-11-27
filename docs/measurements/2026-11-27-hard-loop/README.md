# The hard driver's step loop on the board (2026-11-27)

`otk-flywheel --driver hard` against the Tang Nano 20K running this repository's gateware: the core stepping on the bridge's clock, the plant H steps ahead, and its CAN frames sent at their step (v0.4, step 4). These runs measure the budget that [bridge-protocol.md](../../bridge-protocol.md#9-the-budget-of-a-hard-run) §9 states (ROADMAP v0.4, exit criterion 3).

## Setup

- **Board:** Tang Nano 20K, gateware from `gateware/tang_nano_20k/` (bitstream md5 `6659c249a3a8c779c0f7bb9cac6e6fa5`, the same as in [2026-11-26-hard-driver/](../2026-11-26-hard-driver/)), loaded into SRAM before **each** run.
- **Link:** the board's BL616 on a root port of the USB tree (`3-2`, 480 Mbit/s, no hub), UART at 3 Mbaud.
- **Host:** host B of the [BL616 measurements](../2026-11-26-bl616/), 20 CPUs, Linux 6.8, at normal priority. `otk-flywheel` from commit `1a3e3fc`, Debug build.
- **Run:** [`examples/flywheel/hard60.csv`](../../../examples/flywheel/hard60.csv), 6000 steps of 10 ms (60 s), H = 16 steps (160 ms), margin 600 µs; its SPEED frames on a `vcan0` in a user namespace, logged on the bridge's clock.
- **quiet:** nothing else running on the host. **load:** one busy loop per CPU (20), started before the run.
- [`meta.txt`](meta.txt) records the above as the run saw it.
- **Repeat it with:** `run.sh <open-tick build dir> <outdir>`. It builds the bitstream, programs the board before each run, refuses a tree with local changes, and writes one report and one budget file per run.

## Contents

| File | What |
|---|---|
| `quiet.txt`, `load.txt` | `otk-flywheel`'s report and exit code |
| `quiet.budget.csv`, `load.budget.csv` | the budget, declared and measured (§9) |
| `meta.txt` | date, commit, host, USB path, bitstream md5 |
| `raw-csv.md5` | md5 of each run's trace and frame log. They are kept by the maintainers, not in git |
| `rt_gap.c`, `rt_gap.txt` | why the quiet run's REALTIME → MONOTONIC row is above its budget (below) |
| `run.sh` | the script that made the runs |

## Results

| Quantity | Budget | quiet | load (20 busy loops) |
|---|---|---|---|
| Result, exit code | | `ok`, 0 | `ok`, 0 |
| Underruns, late setpoints, wrong applied setpoints | 0 | 0, 0, 0 (of 8 998 checked) | 0, 0, 0 (of 7 873 checked) |
| Timeouts | | 0 | 0 |
| Lowest buffer fill | | 16 | 15 |
| Exchanges | | 9 003 | 7 877 |
| Round trip p50 / p99 / max | p99 ≤ 1.3 ms | 902 µs / 1.09 ms / 6.99 ms | 885 µs / **7.20 ms** / 10.75 ms |
| DUT-to-plant delay | ≤ 161.3 ms | 161.09 ms | **167.20 ms** |
| Clock mapping bound (largest in the run) | ≤ 500 µs | 460 µs | 355 µs |
| REALTIME → MONOTONIC | ≤ 10 µs | **19.3 µs** | 0.5 µs |
| `bus_clock_err_ns` | ≤ 510 µs | 480 µs | 356 µs |
| CAN send error p50 / p99 / max | p99 ≤ 600 µs | 349 / 401 µs / 2.68 ms | 287 µs / **5.09 ms** / 7.51 ms |
| Latest planned send | | 110 µs | 5.56 ms |
| Forwarding cycle | | 5 999.3 µs | 6 001.0 µs |

Rows above their budget are in bold; the budget files mark them `within=no`.

- **Both runs held H.** No underrun, no late setpoint, and every applied setpoint checked was the one sent. The plant's trace is the same, byte for byte, in both runs (`raw-csv.md5`).
- **Under load, the round trip and the CAN send error are over their budget,** as §9 expects: the sender wakes up late (up to 5.6 ms), so some exchanges miss their forwarding instant and wait a whole cycle. Fewer exchanges fit in the run (7 877 against 9 003), and the buffer still never fell below 15 of 16.
- **On the quiet host, REALTIME → MONOTONIC is over its budget: 19.3 µs against 10 µs.** The error published is the largest half width of any sample, plus how far the offset moved (§5). One sample whose two REALTIME reads are G apart adds about G: G/2 of half width, and up to G/2 of offset. On this host, about 1 triple read in 10 000 is interrupted for 12–20 µs, while the rest take under 1 µs (`rt_gap.txt`, on an idle host). It is not the time service: Linux slews REALTIME and MONOTONIC alike. The load run happened to have no such sample. The bound holds as defined; the 10 µs budget did not account for a rare interrupted read.
- **`bus_clock_err_ns` stayed within budget in both runs,** because the mapping bound left room for it.
- **On the quiet host, one CAN frame of 6 000 was 2.68 ms late** (the step at 59.86 s). Every other frame was within 600 µs. The run does not show why: no planned send was more than 110 µs late, so the frame was delayed after its send was planned. Under load, 1 246 frames were over 600 µs.
