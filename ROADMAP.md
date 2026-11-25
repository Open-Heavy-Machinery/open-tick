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

**Exit criteria**

1. Host-to-bridge latency and throughput are measured and documented before anything else depends on them.
2. open-crank's own v0.4 exit criteria are met through open-tick: a reference engine-ECU firmware achieves crank synchronisation and holds a requested speed.
3. A jitter and latency budget is stated, then measured in the trace of every hard run.
4. The runner refuses a run whose requirements exceed the bridge, and names the signal that fails.

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
