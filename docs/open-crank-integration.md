# Integrating open-tick into open-crank

This records how open-crank came to consume open-tick: a trial run on a copy first, then the real pull request, [open-crank#1](https://github.com/Open-Heavy-Machinery/open-crank/pull/1), merged on 2026-11-19 as `ff4b02c`. The second move, the acceptance oracle in v0.2, is recorded [at the end](#v02-the-oracle).

## Result in open-crank's CI

open-crank#1 added two CI jobs, which ran on the pull request itself:

| Job | Result |
|---|---|
| `traces`: every scenario run with `main` and with the pull request, compared byte for byte | **6 of 6 identical**: `cold_start`, `engine_status_loss`, `hoist_step`, `override_ignored`, `overspeed`, `speed_command_loss` |
| `vcan-demo`: `open-crank-mc` and `open-crank-sil` as two processes on `vcan0` | the controller reaches RUN and never faults; 1500 rpm within 1 % before the hoist load (7.0–7.9 s) and under it (11–12 s) |

Every other job (gcc and clang in Debug and Release, sanitizers, Cortex-M4F portability, 18 mutations, README verbatim) passed as well. These results close open-tick v0.1's exit criteria.

## Trial run (2026-11-19)

The port was applied to a copy of open-crank `main` at `bbe87f5`, built against this branch of open-tick, and compared with the unmodified copy on the same machine and compiler (gcc 11.4, Release, `-DOC_WERROR=ON`).

| Check | Result |
|---|---|
| open-crank builds with warnings as errors | yes |
| open-crank's tests (`ctest`) | 10 of 10 pass |
| Traces of all six scenarios (`open-crank-run --trace`), compared byte for byte | **identical**: `cold_start`, `engine_status_loss`, `hoist_step`, `override_ignored`, `overspeed`, `speed_command_loss` |
| `tools/mutation_check.sh` | 18 killed, 0 problems (one mutation replaced; see below) |
| `tools/check_portable.sh` (`portable/` for Cortex-M4F) | OK, unchanged: 2574 B of text, `memset` the only external symbol |
| `vcan` demo (`open-crank-sil` with `open-crank-mc`) | **not run**: no `vcan` on the trial machine |

The trial covered exit criteria 1 and 2 of open-tick v0.1 for a copy; open-crank#1 then met all four in CI.

## What changed in open-crank

`portable/` is untouched.

**Build**

- The top-level `CMakeLists.txt` adds open-tick from the `external/open-tick` submodule, before open-crank's warning flags, and fails with `git submodule update --init` when the submodule is missing.
- `opencrank_sim` links `opentick::core`; `open-crank-sil` and `open-crank-mc` link `opentick::host`. The `opencrank_socketcan` library is gone. `open-crank-mc` links only `portable`, `can_otk` and open-tick: no longer the simulator.

**Signals and scenarios** (`sim/runtime`)

- `signals.c` keeps the `OcSignalId` enum and registers its names as `OC_SIGNAL_TABLE`, with `end` left out because open-tick reserves it. `oc_signal_from_name` and `oc_signal_name` keep their meaning.
- `scenario.h` becomes `typedef OtkScenario OcScenario` plus two wrappers that pass `OC_SIGNAL_TABLE`. The parser in `scenario.c` is deleted.
- Event times are nanoseconds: `events[i].t` and `end` replace `t_ms` and `end_ms`.

**Bench and loop**

- `OcBench` holds an `OtkPlayer` instead of `scenario` and `scenario_next`. `apply_due_events` becomes `otk_player_advance(&b->player, otk_ns_from_ms(b->t_ms))`. The bench keeps its own `uint32_t` millisecond time, as does everything below it.
- `OcLoop` uses two `OtkCanQueue`s of capacity 32 (the old `OC_LOOP_QUEUE`) instead of its own arrays. Frames are converted at the queue boundary.
- New `sim/can_otk` library: `oc_frame_to_otk` and `oc_frame_from_otk` convert between `J1939Frame` and `OtkCanFrame`. The second rejects 11-bit and CAN FD frames, which is the filtering the old SocketCAN glue did, and fills unused bytes with `0xFF` as before.

**Trace**

- `oc_trace_header(FILE *)` and `oc_trace_row(FILE *, ...)` become `oc_trace_open(OtkTrace *, FILE *)` and `oc_trace_row(OtkTrace *, ...)`. The 26 columns are declared in a table with the same formats, which is why the traces are byte-identical.
- `open-crank-run` reports a trace error and exits non-zero if the writer reports one.

**Platforms**

- `platforms/linux-socketcan/pacer.*` and `socketcan.*` are deleted. `open-crank-sil` and `open-crank-mc` use `OtkPacer` and `OtkSocketCan`, converting frames with `can_otk.h`.

**Tests and tools**

- `test_scenario.c`: four expectations change, all to nanoseconds, plus the error text "time 1 s is earlier" instead of "time 1.000 s is earlier".
- `tools/mutation_check.sh`: the mutation "scenario: unknown signal names accepted" targeted the parser, which now lives in open-tick, where its mutation check kills the same mutation. It is replaced by one that breaks open-crank's own binding: "scenario: events never reach the bench's signals". The count stays at 18.

## Decisions taken

1. **Source: a git submodule** (`external/open-tick`). The other options were `FetchContent` with a pinned tag and a sibling checkout. A submodule pins the exact commit, so a commit of open-crank always means the same build, which the trace comparison relies on. open-crank records this as decision D18.
2. **`can_otk` is a library of its own inside `sim/`.** Moving it to `platforms/` was considered, but open-crank's in-memory loop (`sim/runtime/loop.c`, used by the tests and `open-crank-run`) needs it, and `sim/` may not depend on `platforms/`. As its own library it still keeps the simulator out of `open-crank-mc`.
3. **Documentation in open-crank** was updated in the same pull request: ARCHITECTURE.md §3 and §4.6, the README's layout and build instructions, and the roadmap (v0.4 depends on open-tick v0.4; Track R's virtual-time driver is open-tick's).
4. **CI compares traces.** The `traces` job (decision D19) runs on every open-crank pull request. A pull request that changes behaviour on purpose carries the `behaviour-change` label.

## v0.2: the oracle

The acceptance oracle was written and proven in open-crank v0.2.1 (`sim/oracle/`), then moved here: [#8](https://github.com/Open-Heavy-Machinery/open-tick/pull/8) (merged on 2026-11-23 as `6917b2f`, tagged `v0.2.0`) and [open-crank#26](https://github.com/Open-Heavy-Machinery/open-crank/pull/26) (`9da4e27`).

- **What moved:** `oracle.h` and `oracle.c` into `core/`, renamed `otk_oracle_` / `OtkOracle` / `OTK_ORACLE_`, with time in `OtkNs` in the API. The criteria file format did not change; it is now open-tick's ([criteria-format.md](criteria-format.md)). The oracle's unit tests moved with it (`test_oracle`).
- **What stayed in open-crank:** the J1939 observables and the filter for the device under test (`sim/accept/`), the criteria of each scenario and the two runners (in memory and on a CAN interface).
- **Result:** the seven reports of `open-crank-accept-loop` and the output of `tools/accept_mutants.sh` identical byte for byte before and after; `ctest` 18/18; 19 mutations killed; on `vcan` in CI, seven PASS, 0 late steps and the same reaction times as before.
- **Second consumer:** `otk-flywheel --criteria` grades the toy controller with [examples/flywheel/step.accept.csv](../examples/flywheel/step.accept.csv): 3 of 3 PASS, F1 reaction 110 ms.
