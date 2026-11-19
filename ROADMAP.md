# Roadmap

Scope is deliberately narrow, as in open-crank. Each milestone is small enough to finish and useful on its own. **There are no dates**: a milestone is done when its exit criteria are met. The reasons behind the milestones are in [VISION.md](VISION.md).

## At a glance

| Version | Theme | Driven by |
|---|---|---|
| **v0.0** (this) | Vision and decisions, no code | — |
| **v0.1** | Extraction: open-crank runs on open-tick | open-crank |
| **v0.2** | Second consumer: open-robots in SIL | open-robots |
| **v0.3** | First signal-level bridge: Raspberry Pi Pico (PIO) | open-robots |
| **v0.4** | Tang Nano 20K bridge: tooth wheels and injector capture | open-crank Level 2 |
| **v0.5** | Profiles: YAML compiled to C | open-crank |
| **Track R** | Renode virtual-time driver (parallel) | open-crank |
| **Track S** | Register-mapped slaves: I²C / SPI (parallel, after v0.3) | open-robots |

## Definition of done (every release)

- CI is green on a clean machine.
- The README's build and run instructions work verbatim.
- Every consumer's tests still pass against the released open-tick.
- No claim in the docs is stronger than what the code and tests support.

---

## v0.1: Extraction

**Goal.** open-crank builds against open-tick, and behaves exactly as before.

**In scope.** The pieces listed in [VISION.md §6](VISION.md#6-what-moves-out-of-open-crank-and-what-has-to-change): timebase, paced and free-run drivers, in-memory and SocketCAN bridges, scenario parser with a registered signal table, trace writer with registered columns. The timebase decision (VISION.md, open question 2) is taken here.

**Exit criteria**

1. open-crank's closed-loop tests and all 18 mutation checks pass against open-tick.
2. Every scenario in open-crank produces a trace that is bit-identical to the one from before the move.
3. open-tick includes nothing from any application, and CI checks this.
4. open-crank's `vcan` demo works as its README describes.

## v0.2: Second consumer

**Goal.** A second, very different application runs on the same core, and what it forced to change is written down.

**In scope.** open-robots with the simplest possible machine: one DC motor with an encoder, exposed over a UART message transport in SIL through the host serial bridge. A DUT on a USB-UART link (STM32 or Arduino) controls its speed.

**Exit criteria**

1. open-crank and open-robots run on the same open-tick release.
2. A written list of what open-robots forced to change in open-tick, and why.

## v0.3: Raspberry Pi Pico bridge

**Goal.** A microcontroller reads simulated encoders through its own timers and interrupts, drives simulated motors with its own PWM, and cannot tell the difference within a stated budget.

**In scope.**

- Pico firmware (RP2040 or RP2350, PIO): rotary pattern output (quadrature first), pulse capture (PWM duty and direction).
- The hard driver: the bridge paces the core.
- Host-to-bridge protocol, version 1, shared with the Tang Nano 20K bridge later.
- Fidelity contract, version 1: machines declare requirements, the bridge declares guarantees, and the runner checks them before a run.
- open-robots grows to four motors.

**Exit criteria**

1. Host-to-bridge latency and throughput are measured and documented before anything else depends on them.
2. A DUT closes a speed loop on four simulated motors using odometry from the bridge's encoder edges.
3. A jitter and latency budget is stated, then measured in the trace of every hard run.
4. The runner refuses a run whose requirements exceed the bridge, and names the signal that fails.

## v0.4: Tang Nano 20K bridge

**Goal.** The signal-level transports open-crank's Level 2 needs, on an FPGA, behind the same host protocol as the Pico.

**In scope.** Tooth-wheel patterns (60−2 crank, cam over 720°) as rotary patterns; injector pulse capture with start and width timestamps; analog outputs. Tang Nano 20K gateware; the toolchain decision (VISION.md, open question 3) is taken here.

**Exit criteria**

1. open-crank's own v0.4 exit criteria are met through open-tick: a reference engine-ECU firmware achieves crank synchronisation and holds a requested speed.
2. Moving an application between the Pico and the Tang Nano 20K changes only configuration, not code.

## v0.5: Profiles

**Goal.** Machines are described by reviewable files instead of C structs.

**In scope.** A YAML-to-C compiler in open-tick; each application defines its own schema. Parameters, wiring, provenance and fidelity requirements only, never equations.

**Exit criteria**

1. open-crank's `d936la6` machine is expressed as a profile, and every scenario's trace is bit-identical to the hand-written version.
2. A profile with a missing provenance entry fails the build.

---

## Track R: Renode virtual-time driver (parallel)

Moves here from open-crank's Track R. Step the core in lockstep with an emulated DUT, so firmware runs in the loop in CI without a board. Exit: it runs headless in CI, and its trace matches the board's within a stated tolerance.

## Track S: Register-mapped slaves (parallel, after v0.3)

I²C and SPI devices answered by the bridge from a register map that the core updates each step. First device: an IMU for open-robots. Exit: a DUT reads the simulated IMU with its stock driver and gets values consistent with the plant.

---

## Relation to open-crank's roadmap

- open-crank v0.1.x to v0.3 (MISRA-oriented checks, reference board on the bus, acceptance suite, robustness) do not wait for open-tick. They are the closest real validation and continue in parallel.
- open-crank's acceptance suite (its v0.2.1) is where the oracle is proven before it moves here.
- open-crank v0.4 (Level 2) depends on open-tick v0.4.
- open-crank's Track R moves to open-tick.

## Backlog (not scheduled)

- FMU import for plants too complex to write by hand.
- Bridges to external simulators (MuJoCo, Gazebo) for plants such as humanoids.
- More message transports when an application needs them: RS-485, LIN, CANopen, EtherCAT.
- Python bindings for scripting scenarios.
- A trace viewer, before any dashboard.
