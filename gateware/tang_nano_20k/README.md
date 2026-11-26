# The Tang Nano 20K bridge (gateware)

open-tick's first signal-level bridge. It runs on the Sipeed Tang Nano 20K (Gowin GW2AR-LV18QN88C8/I7), speaks the host ↔ bridge protocol v1 ([docs/bridge-protocol.md](../../docs/bridge-protocol.md)) over the board's BL616 USB-UART at 3 Mbaud, and puts on its pins:
- a rotary pattern: the 60−2 crank wheel and the cam;
- four analog outputs (sigma-delta);
- six captured injector pulses.

**Status: simulated and synthesised, not yet run on a board.**
- CI simulates every block, and the whole bridge over its UART, judged by open-tick's own codec.
- CI builds the bitstream twice and requires it to be the same.
- Running it against the hard driver on a real board is v0.4 step 3 *(planned)*.

## Build, simulate

```sh
tools/fpga_toolchain.sh --synthesis-only        # once: the pinned toolchain into ~/.cache/open-tick/fpga
gateware/tang_nano_20k/build.sh                 # bitstream in build-gateware/bridge.fs

cmake -S . -B build && cmake --build build --target otk-bridge-vectors
gateware/tang_nano_20k/sim/run.sh build         # Icarus Verilog >= 11
gateware/tang_nano_20k/sim/mutations.sh build   # every gateware mutation must be killed
```

To program the board's SRAM (never its flash), run `tools/fpga_toolchain.sh` without `--synthesis-only`. It also builds openFPGALoader 1.1.1 from its pinned tarball. Then `eval "$(tools/fpga_toolchain.sh)"` and `openFPGALoader -b tangnano20k build-gateware/bridge.fs`.

**The toolchain is open and pinned.** It is yosys, nextpnr-himbaechel and Apycula from pip (YoWASP), every file by SHA-256 in [tools/fpga-requirements.lock](../../tools/fpga-requirements.lock), with no sudo and nothing outside the cache. The same sources give the same bitstream: on two PCs for the spike (md5 `f1d6afb7…`), and in every CI run for this design.

## What it declares (configure's reply, §3.4)

| | Value |
|---|---|
| Tick | 27 MHz (37.037 ns), the board's crystal; the run's clock |
| Steps | `step_ticks` ≥ 2700 (100 µs); 27 000 for 1 ms. Below 2700, configure answers status 2 |
| Setpoint buffer | 64 setpoints (block RAM). A setpoint must be queued at least 2 ticks before its step begins |
| Requests and replies | payload up to 448 B (the BL616 budget); up to 32 captures per reply |
| Captures | 6 channels; 32 waiting at most. The width is exact; the start is 2 ticks after the edge (two flip-flops of synchronisation) |
| Analog | 4 first-order sigma-delta outputs, 16 bits of DC resolution, at 27 MHz (`analog_bits` = 16) |
| Rotary | 48-bit phase (2⁴⁸ = 720°); outputs registered on the 27 MHz clock |

**Framing on the bridge's side** (the host's parser does more, §3.1):
- **Resynchronisation:** after a header that is not one (version ≠ 1, or `len` above 448), the bridge hunts for the next sync byte.
- **Gaps:** a frame that stops for more than 1 ms between two bytes is abandoned.
- **Frames during a reply:** a frame that starts while the bridge is still answering is ignored. The host always waits for each reply.

## Blocks

| File | What |
|---|---|
| [`rtl/top.v`](rtl/top.v) | Pins and wiring; a 16-cycle power-on reset |
| [`rtl/uart_rx.v`](rtl/uart_rx.v), [`rtl/uart_tx.v`](rtl/uart_tx.v) | 8N1 at 3 Mbaud (9 clocks per bit). The receiver samples each bit in its middle, so it works with a sender a few per cent off (tested at ±3 %) |
| [`rtl/frame_rx.v`](rtl/frame_rx.v) | Hunts for the sync byte, checks the header and the CRC, and keeps the payload in block RAM |
| [`rtl/crc16.vh`](rtl/crc16.vh) | CRC-16/IBM-3740, the same as `otk_bridge_crc16()` |
| [`rtl/bridge_core.v`](rtl/bridge_core.v) | Configure, start, stop and exchange; the five states; the setpoint FIFO; the step engine (underruns, late setpoints, link timeout); replies assembled byte by byte |
| [`rtl/rotary.v`](rtl/rotary.v) | The crank and cam patterns from a 48-bit phase accumulator |
| [`rtl/capture.v`](rtl/capture.v) | Six channels, polarity and mask, a 32-deep FIFO, losses reported |
| [`rtl/sigma_delta.v`](rtl/sigma_delta.v) | One analog output |
| [`tang_nano_20k.cst`](tang_nano_20k.cst) | Pins (below) |

## Pins

| Signal | FPGA pin | Direction | Notes |
|---|---|---|---|
| clock | 4 | in | 27 MHz crystal |
| UART TX / RX | 69 / 70 | out / in | the BL616 |
| crank, cam | 73, 74 | out | `DRIVE=8` |
| analog 0–3 | 76, 86, 72, 71 | out | `DRIVE=8`; filter on the DUT's side |
| injector 0–5 | 27, 28, 25, 26, 29, 30 | in | `PULL_MODE=DOWN`: a DUT in reset reads as no injection. These pins are the LCD connector's; keep it empty |
| LEDs 0–5 | 15–20 | out | low = on: the state (0–2), crank, cam, a capture waiting |

The signal pins are header pins with no on-board function; every header bank is 3.3 V (Sipeed's pin map). **3.3 V logic only.** They follow the wiring proposed for a NUCLEO-G431KB as the DUT, which is not final until the wiring is decided (open-crank v0.4).

## Resources (CI's build)

nextpnr, GW2AR-18:
- 4 block RAMs: the frame buffer, and three for the setpoint FIFO;
- 36 RAM16SDP4: the capture FIFO;
- about 23 % of the LUTs, 2 multipliers (the crank pattern);
- maximum frequency about 76 MHz against the 27 MHz needed.

`build.sh` fails if 27 MHz is not met or if fewer than 4 block RAMs remain. Logic whose data reaches no output is removed without a warning (findings F-19).

## Tests

- **`sim/tb_units.v`:**
  - the UART, back to back and against a sender ±3 % off;
  - the CRC's check value;
  - the sigma-delta's exact density;
  - the rotary pattern: 58 teeth per revolution, the gap of three pitches, one cam pulse per 720°, phase continuity across a speed change, no edges at rest;
  - capture: width, polarity, mask, simultaneous ends, overflow, flush.
- **`sim/tb_bridge.v`:** the whole bridge, driven over its UART by a script that `otk-bridge-vectors gen` writes with the codec. `otk-bridge-vectors check` judges every reply with the same codec. Twenty exchanges cover:
  - the BL616's banner, an exchange while idle, configure and an unsupported configure;
  - start on an empty buffer, then fill and start (`t_start` checked);
  - while running, the applied setpoint against the step, a retransmission and a gap;
  - two injector pulses (channel, width and start to the tick), the crank turning;
  - underruns holding the last setpoint, late setpoints;
  - a corrupted frame, an unknown type;
  - the link timeout (aborted, outputs at rest), a second run, stop, and stop when stopped.
- **`sim/mutations.sh`:** 17 mutations of the gateware, one behaviour each. Every one must make `run.sh` fail.
