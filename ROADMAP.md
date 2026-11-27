# Roadmap

Scope is deliberately narrow, as in open-crank. Each milestone is small enough to finish and useful on its own. **There are no dates**: a milestone is done when its exit criteria are met. The reasons behind the milestones are in [VISION.md](VISION.md).

**Pull rule.** open-tick grows only when an application needs it in its current milestone. Today that application is open-crank, the organisation's priority. Every milestone below names the application milestone that pulls it. Work for sibling applications that have not started is listed at the end, without version numbers.

## At a glance

| Version | Theme | Pulled by |
|---|---|---|
| **v0.0** (done) | Vision and decisions | — |
| **v0.1** (done) | Extraction: open-crank runs on open-tick | open-crank v0.1 |
| **v0.2** (done) | Oracle: grading the DUT by what its transports show | open-crank v0.2.1 |
| **v0.2.1** (done) | Verdict files: the oracle's verdicts drawn over the trace | open-crank v0.3 |
| **v0.3** (done) | Transport faults | open-crank v0.3 |
| **v0.3.1** (done) | Frame logs: every frame of a run, with logical and bus time | open-rig v0.3 (bus view) |
| **v0.4** | Tang Nano 20K bridge, hard driver, fidelity contract v1 | open-crank v0.4 |
| **v0.5** | Profiles: YAML compiled to C | open-crank v0.5 |
| **Track R** | Renode virtual-time driver (parallel) | open-crank Track R |
| **When a sibling starts** | open-robots in SIL, Raspberry Pi Pico bridge, register-mapped slaves | open-robots |

## Definition of done (every release)

- CI is green on a clean machine.
- The README's build and run instructions work verbatim.
- Every consumer's tests still pass against the released open-tick.
- No claim in the docs is stronger than what the code and tests support.
- Every milestone names the application milestone that pulls it.
- Every change to a public API keeps `otk-flywheel` building and its tests passing, so the core does not drift towards J1939.

---

## v0.1: Extraction

**Goal.** open-crank builds against open-tick, and behaves exactly as before.

**In scope.** The pieces listed in [VISION.md §6](VISION.md#6-what-moves-out-of-open-crank-and-what-has-to-change): timebase, paced and free-run drivers, in-memory and SocketCAN bridges, scenario parser with a registered signal table, trace writer with registered columns. The timebase decision (VISION.md, open question 2) is taken here.

**Status: done** (2026-11-19). Every exit criterion was met in CI on [open-crank#1](https://github.com/Open-Heavy-Machinery/open-crank/pull/1), merged as `ff4b02c`. The record is in [docs/open-crank-integration.md](docs/open-crank-integration.md).

**Exit criteria**

- [x] 1. open-crank's closed-loop tests and all 18 mutation checks pass against open-tick. *One mutation moved to open-tick with the parser and was replaced by one on open-crank's binding to the player.*
- [x] 2. Every scenario in open-crank produces a trace that is bit-identical to the one from before the move. *All six, compared byte for byte by open-crank's new `traces` CI job against `main`.*
- [x] 3. open-tick includes nothing from any application, and CI checks this. *[`tools/check_deps.sh`](tools/check_deps.sh).*
- [x] 4. open-crank's `vcan` demo works as its README describes. *open-crank's new `vcan-demo` CI job runs it as two processes: the controller reaches RUN and holds 1500 rpm within 1 % with and without the hoist load.*

## v0.2: Oracle

**Goal.** Grade any DUT only by what is observable on its transports, with reaction times measured from the injected event and invariants checked over the whole scenario.

**Pulled by** open-crank v0.2.1. The oracle is proven there first, in open-crank's acceptance suite over the bus, and then moved here, the same way the runtime moved in v0.1.

**Status: done** (2026-11-23, tagged `v0.2.0`). The oracle moved in [#8](https://github.com/Open-Heavy-Machinery/open-tick/pull/8) and open-crank consumes it since [open-crank#26](https://github.com/Open-Heavy-Machinery/open-crank/pull/26). The record is in [docs/open-crank-integration.md](docs/open-crank-integration.md#v02-the-oracle).

**Exit criteria**

- [x] 1. open-crank's acceptance verdicts are identical before and after the move. *The seven reports of `open-crank-accept-loop` and the output of its `tools/accept_mutants.sh`, byte for byte; on `vcan` in CI, the same seven PASS and the same reaction times.*
- [x] 2. `otk-flywheel`'s scenario is graded by the oracle as well, as a second consumer that is not J1939. *[examples/flywheel/step.accept.csv](examples/flywheel/step.accept.csv): three criteria (D1, R1, F1), 3 of 3 PASS, run in `ctest` and CI.*

## v0.2.1: Verdict files

**Goal.** The oracle's verdicts in a file that a program can read and a viewer can draw over the trace. The file holds, for each criterion, where its window started and ended, where the reaction ended, and where the criterion first failed.

**Pulled by** open-crank v0.3. Its fault scenarios will make criteria fail on purpose, and debugging them means seeing where on the trace each window, reaction and violation is. Today that is only in the text report, in words.

**In scope.** `otk_oracle_write_verdicts()`, written from the same state as the text report; the format as a contract ([docs/verdict-format.md](docs/verdict-format.md)); `otk-flywheel --verdicts`; open-rig draws the file over the trace (in open-rig's own repository).

**Status: done** (2026-11-25, tagged `v0.2.1`).

**Exit criteria**

- [x] 1. The verdict file of every test case and of `otk-flywheel` agrees with the text report. *[#10](https://github.com/Open-Heavy-Machinery/open-tick/pull/10): `test_oracle` checks every status and reaction state row by row against the report; `example_flywheel_verdicts` compares `otk-flywheel`'s file with [examples/flywheel/step.verdicts.csv](examples/flywheel/step.verdicts.csv) byte for byte, in ctest and CI.*
- [x] 2. open-rig draws the flywheel's verdicts over its trace. *[open-rig#2](https://github.com/Open-Heavy-Machinery/open-rig/pull/2) (open-rig v0.2): windows, events, reaction ends and first violations, with open-tick's file byte for byte; its CI loads a failing example and checks it.*

## v0.3: Transport faults

**Goal.** Transport-level faults for every application: lost, corrupted and delayed frames, bus-off and jitter. Domain faults (an engine ECU that ignores TSC1) stay in the application.

**Pulled by** open-crank v0.3.

**In scope.**
- Links (`opentick/link.h`), one per direction: drop, drop one in N, delay, jitter (seeded), corrupt one byte, and a simulated bus-off. Each fault except bus-off has its own identifier filter.
- Scenario signals `fault.link.<name>.*`.
- Same-identifier order kept.
- The rules and limits in [docs/link-faults.md](docs/link-faults.md).
- `otk-flywheel` with a speed link and `delay.csv`.

Out of scope: a real bus-off, error frames, arbitration and bus load.

**Status: done** (2026-11-25, tagged `v0.3.0`).

**Exit criteria**

- [x] 1. open-crank v0.3's fault scenarios run on them. *[open-crank#33](https://github.com/Open-Heavy-Machinery/open-crank/pull/33): the bench has two links, `mc` and `engine`; four scenarios (`engine_status_delay`, `engine_status_jitter`, `speed_command_intermittent`, `mc_bus_off`) use delay, jitter, drop one in N and bus-off, in memory, on `vcan` and, in a first run, on the reference board. `corrupt` waits for open-crank v0.3.1 (TSC1 checksum).*
- [x] 2. Each fault has a mutation check that fails if the fault is not applied. *[#12](https://github.com/Open-Heavy-Machinery/open-tick/pull/12): `tools/mutation_check.sh` has one per fault (drop, drop one in N, delay, jitter, corrupt, bus-off), plus the order rule and the filters; 27 mutations in all.*
- [x] 3. `otk-flywheel` uses at least one. *[#12](https://github.com/Open-Heavy-Machinery/open-tick/pull/12): the `speed` link, with [examples/flywheel/delay.csv](examples/flywheel/delay.csv) and its criteria.*

## v0.3.1: Frame logs

**Goal.** Every CAN frame of a run in a file, on the trace's time axis, with what the links did to it, so that a viewer can show the bus next to the trace and the verdicts.

**Pulled by** open-rig v0.3 (a bus view) and open-crank's fault scenarios, where what matters happens on the bus: a frame dropped, delayed or corrupted, and what the device under test did next.

**In scope.** `otk_framelog_*` in `core/`; the format as a contract with two times, logical (`t_s`, which places the frame on the axis) and bus (`t_bus_s`, measured, with the clock that measured it named in the file) ([docs/frame-log-format.md](docs/frame-log-format.md)); links that report what they did to each frame (`otk_link_push_ex()`, `otk_link_pop_ex()`); kernel timestamps from SocketCAN, for received frames and for the socket's own sent frames; `otk-flywheel --frames`.

**Out of scope.** Signal-level transports, and the `bridge` clock, which come with v0.4's hard driver; error and RTR frames.

**Status: done** (2026-11-26, tagged `v0.3.1`).

**Exit criteria**

- [x] 1. In memory, a link's delay shows exactly as `t_bus_s − t_s`; lost frames carry their reason; two free-run runs give the same file byte for byte. *[#15](https://github.com/Open-Heavy-Machinery/open-tick/pull/15): `test_framelog` (delay exact, every loss with its reason, corruption, determinism); [examples/flywheel/delay.frames.csv](examples/flywheel/delay.frames.csv), 99 SPEED frames at 0.300000000 inside the delay, checked byte for byte and between two runs (`example_flywheel_frames`, `example_flywheel_frames_deterministic`).*
- [x] 2. Over SocketCAN (`vcan` in CI), received and sent frames carry kernel timestamps, and code that does not ask for them sees exactly what it saw before. *[#15](https://github.com/Open-Heavy-Machinery/open-tick/pull/15): `test_socketcan`'s `test_timestamps_and_own_frames` on `vcan` in CI (run 45741473095); `otk_socketcan_recv()` skips the socket's own frames.*
- [x] 3. Existing traces and verdict files are unchanged, byte for byte. *[#15](https://github.com/Open-Heavy-Machinery/open-tick/pull/15): `step` and `delay` traces and verdicts compared between `main` and the branch; `example_flywheel_verdicts` unchanged; `test_link`'s existing tests untouched.*

## v0.4: Tang Nano 20K bridge

**Goal.** The signal-level transports open-crank's Level 2 needs, on an FPGA. This is open-tick's first signal-level bridge, so it also brings the hard driver, the host-to-bridge protocol and the fidelity contract.

**Pulled by** open-crank v0.4.

**In scope.**

- Tooth-wheel and cam patterns (60−2 crank, cam over 720°) as rotary patterns. Rotary patterns stay one primitive ([VISION.md §5.2](VISION.md#52-two-kinds-of-transport)); this version implements only the tooth-wheel and cam patterns, and quadrature comes with the Raspberry Pi Pico bridge.
- Injector pulse capture with start and width timestamps.
- Analog outputs.
- The hard driver: the bridge paces the core.
- Host-to-bridge protocol, version 1, shared with every signal-level bridge that follows.
- Fidelity contract, version 1: machines declare requirements, the bridge declares guarantees, and the runner checks them before a run.
- Tang Nano 20K gateware; the toolchain decision (VISION.md, open question 3) is taken here.

**Status:**
- the host-to-bridge protocol, version 1, is specified ([docs/bridge-protocol.md](docs/bridge-protocol.md)), and its codec is in `core/` (`opentick/bridge.h`);
- the Tang Nano 20K gateware is built and simulated against the codec, not yet run on a board ([gateware/tang_nano_20k/](gateware/tang_nano_20k/README.md));
- the toolchain decision is taken: the open toolchain, pinned;
- the hard driver's host side is built and measured on the board: link, clock mapping, exchanges scheduled against the BL616's cycle ([docs/measurements/2026-11-26-hard-driver/](docs/measurements/2026-11-26-hard-driver/)). With the host in C, H = 16 ms holds with every CPU busy, and H = 12 ms on a quiet host;
- stepping a plant on the bridge's clock (the hard driver's step loop) and CAN on that clock are built and tested against a fake bridge and on `vcan`; `otk-flywheel --driver hard` runs on them, and has run on the board for 60 s with H = 16, quiet and with every CPU busy, without an underrun ([docs/measurements/2026-11-27-hard-loop/](docs/measurements/2026-11-27-hard-loop/));
- the fidelity contract is not built.

**Exit criteria**

- [x] 1. Host-to-bridge latency and throughput are measured and documented before anything else depends on them. *[docs/bridge-protocol.md](docs/bridge-protocol.md#measured-link-bl616), with the reports, sources and raw-data checksums in [docs/measurements/2026-11-26-bl616/](docs/measurements/2026-11-26-bl616/): about 448 B each way per exchange of about 6 ms; round trip 0.26–6.2 ms depending on phase; clock mapping good to p99 45–85 µs; a 16 ms look-ahead held with every CPU busy on two hosts.*
- [ ] 2. open-crank's own v0.4 exit criteria are met through open-tick: a reference engine-ECU firmware achieves crank synchronisation and holds a requested speed.
- [x] 3. A jitter and latency budget is stated, then measured in the trace of every hard run. *Stated in [docs/bridge-protocol.md §9](docs/bridge-protocol.md#9-the-budget-of-a-hard-run); every hard run writes its measurement next to its trace (`out.budget.csv`). Measured on the board in [docs/measurements/2026-11-27-hard-loop/](docs/measurements/2026-11-27-hard-loop/): on a quiet host every row is within budget except REALTIME → MONOTONIC (19.3 µs against 10 µs, one interrupted sample); with every CPU busy, the round trip and the CAN send error are over it, as stated. Again with wide REALTIME samples dropped ([docs/measurements/2026-11-27-rt-mono/](docs/measurements/2026-11-27-rt-mono/)): on a quiet host every row is within budget (REALTIME → MONOTONIC 1.4 µs); with every CPU busy, the clock mapping's bound was over it too (1 469 µs against 500 µs; still open).*
- [x] 4. The runner refuses a run whose requirements exceed the bridge, and names the signal that fails. *The fidelity contract's checker (`opentick/fidelity.h`, [docs/bridge-protocol.md §7](docs/bridge-protocol.md#7-the-fidelity-contract-version-1)); `otk-flywheel --driver hard --fidelity <machine.csv> --bridge-fidelity <bridge.csv>` refuses before it opens the bridge, exits 5, and names the signal, its line, the column and both numbers. The Tang Nano 20K's file is `gateware/tang_nano_20k/fidelity.csv`, with measured numbers only.*

## v0.5: Profiles

**Goal.** Machines are described by reviewable files instead of C structs.

**Pulled by** open-crank v0.5 (two engines on one machine), which needs a machine described by parameters and wiring instead of a hand-built bench.

**In scope.** A YAML-to-C compiler in open-tick; each application defines its own schema. Parameters, wiring, provenance and fidelity requirements only, never equations.

**Exit criteria**

1. open-crank's two-engine machine and its `d936la6` engine are expressed as profiles, and every scenario's trace is bit-identical to the hand-written version.
2. A profile with a missing provenance entry fails the build.

---

## Track R: Renode virtual-time driver (parallel)

**Pulled by** open-crank Track R.

Moves here from open-crank's Track R. Step the core in lockstep with an emulated DUT, so firmware runs in the loop in CI without a board. Exit: it runs headless in CI, and its trace matches the board's within a stated tolerance.

---

## Relation to open-crank's roadmap

Each open-crank milestone, and what it pulls from open-tick:

- **v0.1.x and v0.2:** nothing; they do not wait for open-tick.
- **v0.2.1** → open-tick v0.2 (oracle).
- **v0.3** → open-tick v0.3 (transport faults).
- **v0.4** → open-tick v0.4 (Tang Nano 20K bridge).
- **v0.5** → open-tick v0.5 (profiles).
- **Track R** → open-tick Track R (Renode virtual-time driver).

## When a sibling starts

These milestones keep their scope and exit criteria. They get a version number when open-robots, or another sibling application, actually starts. Until then nothing here is built.

### Second consumer: open-robots in SIL

**Goal.** A second, very different application runs on the same core, and what it forced to change is written down.

**In scope.** open-robots with the simplest possible machine: one DC motor with an encoder, exposed over a UART message transport in SIL through the host serial bridge. A DUT on a USB-UART link (STM32 or Arduino) controls its speed.

**Exit criteria**

1. open-crank and open-robots run on the same open-tick release.
2. A written list of what open-robots forced to change in open-tick, and why.

### Raspberry Pi Pico bridge

**Goal.** A microcontroller reads simulated encoders through its own timers and interrupts, drives simulated motors with its own PWM, and cannot tell the difference within a stated budget.

**In scope.**

- Pico firmware (RP2040 or RP2350, PIO): quadrature output as a rotary pattern, and pulse capture (PWM duty and direction).
- open-robots grows to four motors.

**Exit criteria**

1. A DUT closes a speed loop on four simulated motors using odometry from the bridge's encoder edges.
2. Moving an application between the Pico and the Tang Nano 20K changes only configuration, not code.

### Register-mapped slaves (after the Pico bridge)

I²C and SPI devices answered by the bridge from a register map that the core updates each step. First device: an IMU for open-robots. Exit: a DUT reads the simulated IMU with its stock driver and gets values consistent with the plant.

## Backlog (not scheduled)

- FMU import for plants too complex to write by hand.
- Bridges to external simulators (MuJoCo, Gazebo) for plants such as humanoids.
- More message transports when an application needs them: RS-485, LIN, CANopen, EtherCAT.
- Python bindings for scripting scenarios.
- A trace viewer, before any dashboard.
