# The hard driver's first measurements on the board (2026-11-26)

The host side of the hard driver ([host/include/opentick/host/hard.h](../../../host/include/opentick/host/hard.h)), in C, measured against the Tang Nano 20K running this repository's gateware. These numbers set the defaults in [bridge-protocol.md](../../bridge-protocol.md) §2 and §4.1.

## Setup

- **Board:** Tang Nano 20K, gateware from `gateware/tang_nano_20k/` (bitstream md5 `6659c249a3a8c779c0f7bb9cac6e6fa5`), loaded into SRAM before **every** run, so that every counter starts at zero.
- **Link:** the board's BL616 on a root port of the USB tree (`3-2`, 480 Mbit/s, no hub), UART at 3 Mbaud.
- **Host:** host B of the [BL616 measurements](../2026-11-26-bl616/), 20 CPUs, Linux 6.8, `otk-hard-probe` from commit `435f8e5`, at normal priority (this user has no real-time priority).
- [`meta.txt`](meta.txt) records the above as the run saw it.
- **Repeat it with:** `run.sh <open-tick build dir> <outdir>`. It builds the bitstream, programs the board before each run, starts the busy loops for the loaded runs, and writes one report per run.

## Contents

| File | What |
|---|---|
| `cycle.txt` | Back to back for 10 s: round trip, and the BL616's forwarding cycle from the replies' arrival times |
| `sweep.txt` | One exchange every two cycles plus 50 µs for 40 s, so that the send time walks through the cycle: round trip by how long before a forwarding instant the request was written |
| `locked_{200,400,600,900}us.txt` | Exchanges scheduled with that margin, plus the 223 µs of their bytes on the wire, before each forwarding instant, 30 s each |
| `hold_H{6,7,8,10,12,16}.txt`, `..._load.txt` | 1 ms steps for 60 s, the buffer kept near H, margin 600 µs; quiet, then with one busy loop per CPU |
| `raw-csv.md5` | md5 of each run's CSV (one row per exchange: T1, T4, T2, T3, how late the send was, step, fill, underruns, error). The CSVs (9 MB) are kept by the maintainers, not in git |

## Results

The tables are in [bridge-protocol.md](../../bridge-protocol.md#measured-link-bl616), "The hard driver in C, on the board". In short:

- **Only the replies wait.** A request reaches the bridge within about 30 µs of spread. A reply waits for the BL616's next forwarding instant, every 5 999.3 µs on the host's clock.
- **Scheduled against that cycle,** the round trip falls from about 6 ms to p50 616–814 µs with 400–600 µs of margin, at one exchange per cycle. Good clock-mapping samples then come all the time (longest gap 1.3 s, against 16 s before).
- **H with the host in C:**
  - a quiet host holds H = 12 ms with no underrun;
  - with every CPU busy, H = 16 ms gives one underrun in about 60 000 steps;
  - H = 6 ms fails either way: one exchange per 6 ms cycle cannot keep less than about 7 setpoints ahead.

## Not kept here

The first full campaign of the same day was run while the same PC was compiling. Its quiet runs are not quiet, so it is not used. It is kept by the maintainers, labelled as such.
