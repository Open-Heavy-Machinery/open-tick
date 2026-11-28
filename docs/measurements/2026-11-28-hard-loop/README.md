# The hard loop at the 1.2 ms margin (2026-11-28)

open-tick v0.4.0: `otk-flywheel --driver hard` on the Tang Nano 20K, with the default margin raised from 600 to 1200 µs, the first exchange after the start that no longer gives way to a CAN frame, and the DUT-to-plant delay declared as (H + 1) × step + 2 × cycle + round trip. The budget and its table are in [docs/bridge-protocol.md §9](../../bridge-protocol.md#9-the-budget-of-a-hard-run).

**Run:** `docs/measurements/2026-11-27-hard-loop/run.sh build <out>`, the same script as on 2026-11-27: `examples/flywheel/hard60.csv`, 60 s of 10 ms steps, H = 16, CAN on `vcan0` in a user namespace, the bridge reprogrammed before each run; `quiet`, then `load` with one busy Python loop per CPU (20).

- open-tick commit `b6840f0e6c4aabf107b91a2b06cbb05e5484a087`; bitstream md5 `6659c249a3a8c779c0f7bb9cac6e6fa5` (rebuilt by the script, identical to v0.4.0-beta.1's).
- The Tang on a root USB port of `david-asus` (`3-2`, 480 Mbit/s, no hub); 20 CPUs, kernel 6.8.0.
- Both runs `ok`: 0 underruns, 0 late, 0 wrong applied setpoints. Quiet: every row within its budget (round trip p99 1.70 ms, mapping bound 804 µs, CAN send error p99 698 µs). Loaded: the round trip, the DUT-to-plant delay and the CAN send error over their budget, as stated for a busy host.
- **A first quiet run on the same commit ended `aborted`** (`abort=io` at step 3595): the BL616 reset its USB (the kernel's `usb 3-2: USB disconnect`). It is not kept here; its line is quoted in §9.

| File | What |
|---|---|
| `meta.txt` | date, commit, CPUs and kernel, the USB path, the bitstream, openFPGALoader |
| `quiet.txt`, `load.txt` | `otk-flywheel`'s output (the output directory written as `<out>`) |
| `quiet.budget.csv`, `load.budget.csv` | the budget of each hard run |
| `quiet.csv`, `load.csv` | the traces |
| `quiet.frames.csv`, `load.frames.csv` | the frame logs (`bus_clock=bridge`) |

`md5sum -c md5.txt` checks the files.
