# Changelog

All notable changes to open-tick. The format follows [Keep a Changelog](https://keepachangelog.com/en/1.1.0/); versions follow [Semantic Versioning](https://semver.org/), with the pre-1.0 rule that any minor release may change the API.

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
