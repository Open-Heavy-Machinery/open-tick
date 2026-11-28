# Captures at 25 Hz and the crank's tooth period, on the board (2026-11-28)

The numbers behind the Tang Nano 20K's capture rate and crank jitter in [`gateware/tang_nano_20k/fidelity.csv`](../../../gateware/tang_nano_20k/fidelity.csv) ([bridge-protocol.md](../../bridge-protocol.md#7-the-fidelity-contract-version-1) §7). Before, the file guaranteed captures at 10 Hz, the rate the first harness check sent, and no crank jitter. open-crank's Level 2 asks for 20 Hz per cylinder (2400 rpm) and a crank jitter.

## Setup

- **Bridge:** Tang Nano 20K, bitstream md5 `6659c249a3a8c779c0f7bb9cac6e6fa5`, loaded into SRAM before each run; the BL616 on a root port (`3-2`, no hub).
- **DUT:** a NUCLEO-G431KB on open-crank's Level 2 harness ([WIRING-L2.md](https://github.com/Open-Heavy-Machinery/open-crank/blob/main/platforms/stm32g4/WIRING-L2.md)), running open-crank's `oc-stm32g4-wirecheck` from commit `dee0bdd` (open-crank#49). Its clock is its HSI.
- **Run:** open-crank's `tools/board/wirecheck.py --seconds 60 --inj-hz 25 --rpm R`, with `otk-hard-probe wires` from this repository (`24fb476`). The probe turns the 60−2 crank at R rpm and captures the six injector inputs. 3 s into the run, the NUCLEO is asked for 1375 bursts at 25 Hz, ending 2 s before the probe stops; in each burst injector k goes high for k × 100 µs. So all six channels capture at once, 150 pulses a second, and each must capture exactly 1375.
- **Tooth period:** the NUCLEO's TIM2 (32-bit, 170 MHz) captures every rising crank edge and sums the tooth periods, the gap excluded, once a second. Only seconds wholly inside the steady run are used.
- **Host:** nothing else using CPU (94.5 % idle).

## Contents

| File | What |
|---|---|
| `rpm1500.txt`, `rpm2400.txt`, `rpm6000.txt` | `wirecheck.py`'s output for each run |
| `rpm*/probe.txt` | `otk-hard-probe wires`'s output: captures per channel, widths, captures lost |
| `rpm*/nucleo.log` | the NUCLEO's lines, with the time since the probe started. The first lines of `rpm1500/nucleo.log`, at time 0, were queued in the ST-LINK's virtual COM port before the run and are not used |
| `meta.txt` | date, commits, binaries' md5, USB path |

## Results

| rpm | captures per channel | captures lost (bridge) | NUCLEO overruns | tooth period, mean | sd | max − min | largest deviation from the mean | sd / period |
|---|---|---|---|---|---|---|---|---|
| 1500 | 1375 of 1375, all six | 0 | 0 | 665 141.1 ns | 119.7 ns | 994.1 ns | 517.6 ns | 180 ppm |
| 2400 | 1375 of 1375, all six | 0 | 0 | 415 779.1 ns | 84.3 ns | 658.8 ns | 343.8 ns | 203 ppm |
| 6000 | 1375 of 1375, all six | 0 | 0 | 166 296.0 ns | 33.3 ns | 305.9 ns | 162.9 ns | 200 ppm |

- **Captures: 25 Hz per channel, all six at once, none lost** at every speed. That is the `max_rate_hz` of `capture0`–`capture5`.
- **Crank jitter: 520 ns,** the largest deviation measured (517.6 ns at 1500 rpm), rounded up.
  - **It is an upper bound for the bridge.** The spread grows in proportion to the period, about 200 ppm of it at every speed: frequency noise of the clock that measures, the NUCLEO's HSI. The bridge's DDS alone would put each edge within one 27 MHz tick (37 ns), whatever the speed. The NUCLEO's capture adds one 5.9 ns tick.
  - A DUT on a crystal would measure less. The row stays at what was measured.
- **Not measured:** the cam's jitter, the bridge's own latency, the analog outputs.
