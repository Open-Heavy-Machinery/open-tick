# Changelog

All notable changes to open-tick. The format follows [Keep a Changelog](https://keepachangelog.com/en/1.1.0/); versions follow [Semantic Versioning](https://semver.org/), with the pre-1.0 rule that any minor release may change the API.

## [0.4.0] (2026-11-28): the Tang Nano 20K bridge, with Level 2's ECU in the loop

v0.4's four exit criteria are met. Criterion 2, the one beta.1 left open, is met through this bridge by open-crank v0.4.0: its reference engine ECU, on a NUCLEO-G431KB, synchronises on the 60−2 crank and the cam this bridge generates from the plant's angle and holds a requested speed in a closed loop, with every one of its injections captured here (open-crank `docs/measurements/2026-11-28-hil/`, open-crank#55). Those runs also found three things in the hard loop, fixed below.

### Changed

- **The hard loop's default margin: 600 → 1200 µs** (`OTK_HARD_DEFAULT_MARGIN_NS`). At 600 µs, replies that carry captures missed the BL616's forwarding instant often enough to cost up to 225 underruns in a 25 s run at 1 ms steps; at 1200 µs none did. The round trip and the clock mapping's bound grow by about 0.6 and 0.4 ms; `otk-flywheel` declares 2.2 ms, 1000 µs and a CAN send error of 1100 µs ([docs/bridge-protocol.md §9](docs/bridge-protocol.md#9-the-budget-of-a-hard-run), [docs/measurements/2026-11-28-hard-loop/](docs/measurements/2026-11-28-hard-loop/)).
- **The DUT-to-plant delay as it is** (§2): (H + 1) × step + 2 × forwarding cycle + round trip, `otk_hard_dut_to_plant_ns()`: 31.2 ms at 1 ms steps (was declared as H + a round trip, 17.3 ms; captures came up to 30 ms late). `otk-flywheel --fidelity`, the budget's `dut_to_plant_delay_ns` and `gateware/tang_nano_20k/fidelity.csv`'s metadata use it.
- **`gateware/tang_nano_20k/fidelity.csv`**, measured again on the board ([docs/measurements/2026-11-28-fidelity/](docs/measurements/2026-11-28-fidelity/)): the six captures at 25 Hz each (was 10 Hz), none lost at 1500, 2400 and 6000 rpm with all six at once; the crank's `jitter_ns` 520 (was empty), an upper bound read by a NUCLEO-G431KB on its HSI. Open-crank's Level 2 asks for 20 Hz per cylinder and 1000 ns of crank jitter.

### Added

- **The captures' lag in every hard run's budget** (`capture_lag_max_ns`, `OtkHardStats.capture_lag_max`): the most steps the application had computed beyond a capture's step when the capture came in, against the declared DUT-to-plant delay.
- **Why a run was aborted** (`OtkHardAbort`, `otk_hard_abort_name()`): the first cause, the steps computed then, and the last reply's error and state, in `OtkHardStats`, in the budget's metadata line (`abort=…`) and in `otk-flywheel`'s output. On the reference bench every abort seen was `io`: the BL616 reset its USB.

### Fixed

- **The first exchange after the start no longer gives way to a CAN frame.** With 1 ms steps the bridge holds only the fill's H setpoints, and a frame queued for step 0 cost one underrun a run.

### Known limitations

- **The BL616 resets its USB now and then** (about every 15 minutes of hard runs on the reference bench, 2026-11-28): the run under way ends `aborted` with `abort=io`. The cause is not known.
- **At 1 ms steps with H = 16, a run is `degraded` now and then** (2 of 8 in open-crank's Level 2 runs, 3 underruns each): a sender that wakes about 1 ms late loses a forwarding cycle. A real-time priority for the sender (`SCHED_FIFO`) is a deferred improvement.
- The analog outputs are not yet measured on a board.

## [0.4.0-beta.1] (2026-11-28): the Tang Nano 20K bridge, before Level 2's ECU

A pre-release: v0.4's exit criteria 1, 3 and 4 are met, and criterion 2 (a reference engine-ECU firmware on open-crank, through this bridge) is open. It is tagged so that open-crank's Level 2 work can pin the hard driver; v0.4.0 follows when criterion 2 is met.

### Added

- **The fidelity contract's checker** (`core/include/opentick/fidelity.h`; v0.4 exit criterion 4), ISO C in `core/`, no allocation: parses version 1 files (§7) with the line and column of any error, matches each machine row to the next free bridge row of its direction and transport, and returns the first signal that fails, with its column and both numbers. A requirement the bridge leaves empty is refused. `test_fidelity`; eleven more mutations, 85 in all.
- **`otk-flywheel --fidelity <machine.csv> --bridge-fidelity <bridge.csv>`** (with `--driver hard`): checked before the bridge is opened, with this run's lead and DUT-to-plant delay; a refusal exits 5. `examples/flywheel/fidelity.csv` is the flywheel's.
- **`gateware/tang_nano_20k/fidelity.csv`**, the Tang Nano 20K's guarantees: only what was measured (crank to 5800 Hz and cam to 50 Hz at 6000 rpm, six captures at 10 Hz) or follows from its clock (37 ns) and banks (3.3 V).
- **`otk-hard-probe wires [--rpm R]`:** a harness check on the bridge. A hold run with the crank at R rpm and every capture channel on; at the end, per channel, the pulses captured and their widths, and the captures lost. Used by open-crank's Level 2 wiring check.
- **The hard driver's step loop** (`host/include/opentick/host/hard_loop.h`; v0.4, step 4), Linux. The core steps on a bridge's clock, with the plant H steps ahead of it:
  - the run of `docs/bridge-protocol.md` §8: a bridge that is not freshly programmed is refused, the configuration and the bridge's capabilities are checked, the first H setpoints are sent, the bridge is started, and it is stopped in the application's last step;
  - one exchange per forwarding cycle, scheduled against it, with every setpoint computed since the last;
  - every reply checked: underruns, late setpoints, the applied setpoint;
  - captures extended to 64 bits and handed over with their step;
  - a run's outcome, `ok`, `degraded` or `aborted`.
- **`OTK_DRIVER_HARD`** in `driver.h`, with `otk_driver_attach_hard()` and `otk_driver_failed()`. The names are `free-run`, `paced` and `hard`.
- **CAN on the bridge's clock** (§5):
  - frames queued for a step go out when the bridge begins it;
  - kernel timestamps are mapped REALTIME → MONOTONIC → bridge, and that step's error is measured in every run (`OtkRtMono`);
  - frame logs with `bus_clock=bridge` and a measured `bus_clock_err_ns`.
- **The budget of a hard run** (§9, v0.4 exit criterion 3):
  - stated for the BL616 link;
  - every hard run writes it next to its trace, declared and measured (`otk_hard_budget_write()`), with the USB path, the bitstream's md5 and the commit.
- **`otk-flywheel --driver hard --dev <tty>`:**
  - the flywheel's speed turns the bridge's rotary pattern and sets analog output 0;
  - `--can <if>` sends its SPEED frames at their step, and `--frames` logs them on the bridge's clock;
  - exit codes 0 ok, 3 degraded, 4 aborted;
  - a 60 s scenario, `examples/flywheel/hard60.csv`.
- **Tests without a board:**
  - `test_hard_loop`, against a fake bridge on a pseudo-terminal (`tests/fake_bridge.c`);
  - `test_hard_can` on `vcan`;
  - `example_flywheel_hard`, with the fake as a program (`otk-fake-bridge`);
  - 14 more mutations, 72 in all.
- `docs/measurements/2026-11-27-hard-loop/run.sh`: the board runs (60 s quiet and 60 s under load, H = 16, CAN on `vcan0`), each from a freshly programmed bridge.
- **The step loop measured on the board** ([docs/measurements/2026-11-27-hard-loop/](docs/measurements/2026-11-27-hard-loop/); v0.4 exit criterion 3):
  - both runs `ok`: no underrun, no late setpoint, every applied setpoint checked was the one sent, and the same plant trace byte for byte;
  - quiet: round trip p50 902 µs, p99 1.09 ms, mapping bound 460 µs, CAN send error p99 401 µs;
  - with every CPU busy: round trip p99 7.20 ms and CAN send error p99 5.09 ms, over their budget as stated;
  - REALTIME → MONOTONIC 19.3 µs on the quiet host, over its 10 µs budget: one interrupted sample sets the whole run's bound (`rt_gap.c` measures how often).

### Changed

- **REALTIME → MONOTONIC drops its wide samples** (`docs/bridge-protocol.md` §5). A sample whose two REALTIME reads are more than 2 µs apart (`OTK_RT_MONO_MAX_WIDTH_NS`) is read again, up to 3 tries; one still too wide is dropped and counted (`OtkRtMono.rejected`, the budget's new row `realtime_to_monotonic_rejected`). A single interrupted read no longer sets the run's bound; a real step of REALTIME still does. The declared 10 µs is unchanged. `otk_rt_mono_add()` returns whether the sample was accepted, and `otk_rt_mono_sample_from()` takes a clock for tests. Two more mutations, 74 in all.
  - Measured on the board again ([docs/measurements/2026-11-27-rt-mono/](docs/measurements/2026-11-27-rt-mono/)): REALTIME → MONOTONIC 1.4 µs quiet and 0.2 µs under load, no sample dropped; on the quiet host every row of the budget is within it. Under load, the clock mapping's bound measured 1 469 µs, over its 500 µs (355 µs in the earlier load run); not explained yet.
- `docs/frame-log-format.md`: `bus_clock=bridge` is built; its `bus_clock_err_ns` is the bound measured in the run, not the clock's resolution.
- `test_driver`: `hard` is now a driver name.

- **The hard driver's host side** (`host/include/opentick/host/hard.h`; v0.4, step 3), Linux:
  - the serial link: one request, one reply, banner and stale replies skipped;
  - the clock mapping (§4), leaving out bins with no good sample;
  - the link's forwarding cycle, estimated from the replies' arrival times;
  - `otk_hard_wait_until()`: sleep, then spin, timer slack 1 ns;
  - defaults H = 16 steps and a 600 µs margin.
- **`otk-hard-probe`**, which measures a bridge with it: `cycle`, `sweep`, `locked`, `hold`.
- **Measured on the board** ([docs/measurements/2026-11-26-hard-driver/](docs/measurements/2026-11-26-hard-driver/)):
  - only the BL616's replies wait, for a forwarding instant every 5 999.3 µs;
  - exchanges scheduled against that cycle come back in about 1 ms instead of up to 6;
  - with the host in C, H = 12 ms holds on a quiet host, and H = 16 ms with every CPU busy;
  - the gateware ran on the board for the first time, and held every run.
- `docs/bridge-protocol.md` §4.1: asymmetric links. The bound published stays symmetric, setpoints are aimed from the fast direction, and exchanges are scheduled against the link's cycle.
- `test_hard` (no board needed) and five more mutations, 58 in all.

- **The Tang Nano 20K bridge's gateware** (`gateware/tang_nano_20k/`; v0.4, step 2). Simulated and synthesised, not yet run on a board.
  - It covers protocol v1's bridge side: framing and CRC, configure/start/stop/exchange, the five states, a 64-setpoint FIFO in block RAM, steps with underruns, late setpoints and the link timeout.
  - Signals: the 60−2 crank and cam from a 48-bit phase, six injector captures, four sigma-delta analog outputs.
  - Pins as proposed for a NUCLEO-G431KB as the DUT; resources about 23 % of the LUTs and 4 block RAMs, 76 MHz against 27.
- **Gateware tests** (CI job `gateware`):
  - `sim/run.sh` runs `tb_units` (UART including ±3 % baud error, CRC, sigma-delta, rotary pattern, capture) and `tb_bridge`. `tb_bridge` is the whole bridge over its UART, scripted by `otk-bridge-vectors gen` and judged by `otk-bridge-vectors check`, both from the codec.
  - `sim/mutations.sh`: 17 mutations, all killed.
  - The bitstream is built twice and compared.
- **`tools/fpga_toolchain.sh`:** the open FPGA toolchain (yosys, nextpnr-himbaechel, Apycula from pip; openFPGALoader 1.1.1 from its tarball), every file pinned by SHA-256 (`tools/fpga-requirements.lock`). It installs into a user cache, without sudo and isolated from the user's Python. `--synthesis-only` is for CI.

- **The host ↔ bridge protocol v1 codec** (`core/include/opentick/bridge.h`; v0.4, step 1), against [docs/bridge-protocol.md](docs/bridge-protocol.md):
  - framing with CRC-16/IBM-3740, and every message's payload encoded and decoded: exchange, configure, start, stop and their replies;
  - a stream parser that resynchronises on the sync byte;
  - the speed ↔ phase-increment conversion (48-bit phase, 720°) and analog setpoints;
  - the extension of a capture's 32-bit tick to 64 bits.

  Pure ISO C in `core/`, no allocation. `ctest`: `test_bridge`. Six more mutations, 53 in all.

- `otk-flywheel --seed <n>`: the seed of the speed link's PRNG, which draws the jitter (default 1, as before, so every existing output is unchanged). [examples/flywheel/jitter.csv](examples/flywheel/jitter.csv) adds 0 to 30 ms of jitter to SPEED for a second. `ctest`: no `--seed` is seed 1, the same seed gives the same frame log, seeds 1 and 2 give different ones, and a seed that is not a whole number is refused. One more mutation ("flywheel: --seed is ignored"; 47 in all).

- `tools/mutation_check.sh`: eight SocketCAN mutations (46 in all): kernel timestamps (`SO_TIMESTAMPNS`, the reported time and its nanoseconds, the anchor), the socket's own frames (`CAN_RAW_RECV_OWN_MSGS`, the `own` flag, the plain receive skipping them) and the bus clock's name. `test_socketcan` kills each one. They run only with a CAN interface; `OTK_MUTATION_NEED_CAN=1` makes a missing one a problem.

### Changed

- CI: the mutation job sets up `vcan0` and runs with `OTK_MUTATION_NEED_CAN=1`, so the SocketCAN mutations always run there.
- **A faster mutation check:**
  - `tools/mutation_check.sh` stops each mutation's suite at the first failing test (`ctest --stop-on-failure`) and names it from that same run, instead of running the whole suite twice;
  - `OTK_MUTATION_SHARD=i/n` runs one share of the mutations; CI runs four shards in parallel;
  - CI skips the mutations on a pull request whose every change is Markdown or under `docs/`; `main` always runs them.

## [0.3.1] (2026-11-26): frame logs

### Added

- **Frame logs** (v0.3.1): `opentick/framelog.h`, `otk_framelog_init()` and `otk_framelog_frame()`. One row per CAN frame with two times: `t_s`, the step in which the frame crossed the machine's side (it places the frame on the trace's axis), and `t_bus_s`, when it was on the device under test's side, measured by the clock that the metadata line names (`sim`, `kernel`, `host`, `bridge`, `none`) with its error bound. Plus `dir`, the link's name, the frame, and what the link did to it.
- [docs/frame-log-format.md](docs/frame-log-format.md): the frame log format as a contract (version 1).
- Links report what they did to each frame: `otk_link_push_ex()` and `otk_link_pop_ex()` (`OtkLinkInfo`: the reason for a loss, corruption, push and due times). `pop_ex` also returns frames lost to bus-off when they fall due.
- SocketCAN timestamps: `otk_socketcan_enable_timestamps()`, `otk_socketcan_recv_ex()` (kernel timestamp, own frame), `otk_socketcan_clock_ns()`, `otk_socketcan_run_time()` and `otk_socketcan_bus_clock()`.
- `otk-flywheel --frames <file>`. It writes [examples/flywheel/delay.frames.csv](examples/flywheel/delay.frames.csv) for `delay.csv`, checked byte for byte (`example_flywheel_frames`) and between two runs (`example_flywheel_frames_deterministic`).
- `test_framelog`; `test_link` and `test_socketcan` extended; eleven mutations in `tools/mutation_check.sh` (38 in all).

### Changed

- `otk_link_push()` and `otk_link_pop()` are now `push_ex` and `pop_ex` without the report. Their behaviour is unchanged: `test_link`'s existing tests are untouched, and the flywheel's traces and verdicts are byte-identical. `OtkLinkSlot` has two more fields (`pushed`, `corrupted`); `OtkSocketCan` has two more (`timestamps`, `own_msgs`), set by `otk_socketcan_open()`.
- README: the mutation count was out of date (18); it is 38.

## [0.3.0] (2026-11-25): transport faults

### Added

- **Links and transport faults** (v0.3): `opentick/link.h`.
  - A link is one direction of a CAN path, with `otk_link_push()` and `otk_link_pop()`.
  - Its faults are drop, drop one in N, delay, jitter, corrupt and bus-off. Each fault except bus-off has an identifier filter.
  - A frame never overtakes an earlier frame with the same identifier.
  - Memory is fixed: a delay line of `OTK_CAN_QUEUE_MAX` frames, with overflows counted.
  - Jitter comes from the link's own seeded PRNG, so free-run stays deterministic.
- Scenario signals `fault.link.<name>.drop`, `.drop_one_in`, `.delay_ms`, `.jitter_ms`, `.corrupt` and `.bus_off`, through `OTK_LINK_SIGNAL_NAMES()` and `otk_link_signal()`.
- [docs/link-faults.md](docs/link-faults.md): what each fault simulates and what not, and why `corrupt` means "the CRC passed, the content is wrong". It also explains why a constant delay cannot be seen from arrival times.
- `otk-flywheel` sends the machine's frames through a link named `speed` (filter `0x100`). [examples/flywheel/delay.csv](examples/flywheel/delay.csv) delays SPEED by 300 ms, and [examples/flywheel/delay.accept.csv](examples/flywheel/delay.accept.csv) asks for no drive inside the gap. Tests: `example_flywheel_delay_accept` and `example_flywheel_delay_deterministic`. `step.csv` gives the same trace, report and verdicts as before, byte for byte.
- `test_link`, and eight link mutations in `tools/mutation_check.sh` (27 in all).

## [0.2.1] (2026-11-25): verdict files

### Added

- **Verdict files** (v0.2.1): `otk_oracle_write_verdicts()` writes one CSV row per criterion, with its verdict, status (`ok`, `violated`, `not_evaluated`, `not_covered`), window, event, reaction and first violation, in seconds on the trace's time axis. It reads the same state as `otk_oracle_report()`, through the same functions.
- [docs/verdict-format.md](docs/verdict-format.md): the verdict format as a contract.
- `otk-flywheel --verdicts <file>` (with `--criteria`). It writes [examples/flywheel/step.verdicts.csv](examples/flywheel/step.verdicts.csv), checked byte for byte by the test `example_flywheel_verdicts` and in CI. Without `--verdicts`, the output and the trace are unchanged.
- `test_oracle`: the verdict file is checked row by row against the text report, for every status and every reaction state, and for a write error.
- `tools/mutation_check.sh`: "verdicts: reaction written from the window start, not the event" (19 mutations).

### Changed

- `otk_oracle_report()` now reads the outcome and the reaction through the same functions as the verdict writer. Its output is unchanged: every existing report test passes, and `otk-flywheel`'s report is identical byte for byte.

## [0.2.0] (2026-11-23): oracle

### Added

- [docs/trace-format.md](docs/trace-format.md): the shape of a trace file as a contract for readers: what is guaranteed (no quotes or commas in a field, one field per column in every row, an empty field is a gap) and what is not (types, units, a version).
- **core**: `oracle.h`, the acceptance oracle, which grades a run from the frames a device under test sends, against criteria written as data. Moved from open-crank v0.2.1 (`sim/oracle/`), renamed `otk_oracle_` / `OtkOracle` / `OTK_ORACLE_`, with time in `OtkNs` in the API (`otk_oracle_parse`, `otk_oracle_load`, `otk_oracle_frame`, `otk_oracle_step`, and an observable's `stale`). The criteria file keeps its format, in milliseconds.
- [docs/criteria-format.md](docs/criteria-format.md): the criteria format as a contract: syntax, windows, tolerance, evaluation, errors, limits and report, and what is not guaranteed (a version, the limits).
- `otk-flywheel --criteria`: the oracle grades the toy controller by its DUTY frames, with [examples/flywheel/step.accept.csv](examples/flywheel/step.accept.csv); run as a test (`example_flywheel_accept`) and in CI. Without `--criteria` the trace is unchanged.
- `test_oracle`: the oracle's tests from open-crank (synthetic table), plus a reaction measured from an event that does not fall on a whole millisecond.

### Changed

- Against the oracle in open-crank v0.2.1: with scenario events on whole milliseconds nothing changes. An event that does not fall on a whole millisecond is now measured from its exact time instead of being rounded up to the next millisecond, and a reaction time that is not a whole number of milliseconds is reported with its fraction (`reaction: 9.500000 ms after go=1`).

## [0.1.0] (2026-11-19): extraction

### Added

- Vision and roadmap ([VISION.md](VISION.md), [ROADMAP.md](ROADMAP.md)).
- **core** (ISO C11, any host):
  - `timebase.h`: simulated time as `uint64_t` nanoseconds;
  - `signals.h`: signal tables registered by the application, with `end` reserved;
  - `scenario.h`: the CSV scenario parser from open-crank v0.1, now driven by a signal table and timed in nanoseconds;
  - `player.h`: applies scenario events at the start of each step;
  - `can.h`: CAN and CAN FD frames, validity checks, and a bounded queue for in-memory bridges;
  - `trace.h`: CSV trace writer with registered columns and sticky errors.
- **host** (Linux):
  - `pacer.h`: wall-clock pacing that measures how late it wakes up;
  - `driver.h`: free-run and paced drivers behind one loop shape;
  - `socketcan.h`: SocketCAN bridge for classic CAN and CAN FD.
- `otk-flywheel`, an example that uses every module, run as a test for determinism and pacing.
- Tests, a dependency check (`tools/check_deps.sh`), a mutation check with 18 mutations (`tools/mutation_check.sh`), and CI: gcc and clang, Debug and Release, sanitizers, core-only build on macOS, SocketCAN on `vcan`.
- [ARCHITECTURE.md](ARCHITECTURE.md), and the record of open-crank's integration ([docs/open-crank-integration.md](docs/open-crank-integration.md)): open-crank runs on this release as a git submodule since [open-crank#1](https://github.com/Open-Heavy-Machinery/open-crank/pull/1).

### Extracted from open-crank v0.1 (`bbe87f5`)

| open-crank | open-tick | Changed |
|---|---|---|
| `sim/runtime/src/scenario.c` | `core/src/scenario.c` | signal table instead of a fixed enum; nanoseconds |
| `apply_due_events` in `sim/runtime/src/bench.c` | `core/src/player.c` | none in behaviour |
| `sim/runtime/src/trace.c` (writer part) | `core/src/trace.c` | columns registered; error checks |
| `sim/runtime/src/loop.c` (queues) | `core/src/can.c` | generic frames; capacity set at runtime |
| `platforms/linux-socketcan/pacer.c` | `host/src/pacer.c` | nanoseconds; lateness measured; only `EINTR` retried |
| `platforms/linux-socketcan/socketcan.c` | `host/src/socketcan.c` | 11-bit frames and CAN FD; J1939 filtering moved to open-crank |
