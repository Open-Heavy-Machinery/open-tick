# Changelog

All notable changes to open-tick. The format follows [Keep a Changelog](https://keepachangelog.com/en/1.1.0/); versions follow [Semantic Versioning](https://semver.org/), with the pre-1.0 rule that any minor release may change the API.

## [Unreleased]

### Added

- `otk-flywheel --seed <n>`: the seed of the speed link's PRNG, which draws the jitter (default 1, as before, so every existing output is unchanged). [examples/flywheel/jitter.csv](examples/flywheel/jitter.csv) adds 0 to 30 ms of jitter to SPEED for a second. `ctest`: no `--seed` is seed 1, the same seed gives the same frame log, seeds 1 and 2 give different ones, and a seed that is not a whole number is refused. One more mutation ("flywheel: --seed is ignored"; 47 in all).

- `tools/mutation_check.sh`: eight SocketCAN mutations (46 in all): kernel timestamps (`SO_TIMESTAMPNS`, the reported time and its nanoseconds, the anchor), the socket's own frames (`CAN_RAW_RECV_OWN_MSGS`, the `own` flag, the plain receive skipping them) and the bus clock's name. `test_socketcan` kills each one. They run only with a CAN interface; `OTK_MUTATION_NEED_CAN=1` makes a missing one a problem.

### Changed

- CI: the mutation job sets up `vcan0` and runs with `OTK_MUTATION_NEED_CAN=1`, so the SocketCAN mutations always run there.

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
