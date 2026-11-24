# Changelog

All notable changes to open-tick. The format follows [Keep a Changelog](https://keepachangelog.com/en/1.1.0/); versions follow [Semantic Versioning](https://semver.org/), with the pre-1.0 rule that any minor release may change the API.

## Unreleased

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
