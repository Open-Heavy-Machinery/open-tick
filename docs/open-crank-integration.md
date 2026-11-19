# Integrating open-tick into open-crank

This is the plan for the open-crank pull request that makes open-crank consume open-tick, and the record of a trial run of it. The pull request itself is opened after open-tick v0.1 is merged; nothing here changes open-crank yet.

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

This covers exit criteria 1 and 2 of open-tick v0.1 (see [ROADMAP.md](../ROADMAP.md)) for the trial copy. They count as met when the real pull request passes open-crank's CI with the same comparison.

## What changes in open-crank

24 files; `portable/` is untouched.

**Build**

- The top-level `CMakeLists.txt` adds open-tick as a subproject (`add_subdirectory`). How the source arrives is still open; see below.
- `opencrank_sim` links `opentick::core`; `open-crank-sil` and `open-crank-mc` link `opentick::host`. The `opencrank_socketcan` library goes away.

**Signals and scenarios** (`sim/runtime`)

- `signals.c` keeps the `OcSignalId` enum and registers its names as `OC_SIGNAL_TABLE`, with `end` left out because open-tick reserves it. `oc_signal_from_name` and `oc_signal_name` keep their meaning.
- `scenario.h` becomes `typedef OtkScenario OcScenario` plus two wrappers that pass `OC_SIGNAL_TABLE`. The parser in `scenario.c` is deleted.
- Event times are nanoseconds: `events[i].t` and `end` replace `t_ms` and `end_ms`.

**Bench and loop**

- `OcBench` holds an `OtkPlayer` instead of `scenario` and `scenario_next`. `apply_due_events` becomes `otk_player_advance(&b->player, otk_ns_from_ms(b->t_ms))`. The bench keeps its own `uint32_t` millisecond time, as does everything below it.
- `OcLoop` uses two `OtkCanQueue`s of capacity 32 (the old `OC_LOOP_QUEUE`) instead of its own arrays. Frames are converted at the queue boundary.
- New `sim/runtime/.../can_otk.h`: `oc_frame_to_otk` and `oc_frame_from_otk` convert between `J1939Frame` and `OtkCanFrame`. The second rejects 11-bit and CAN FD frames, which is the filtering the old SocketCAN glue did, and fills unused bytes with `0xFF` as before.

**Trace**

- `oc_trace_header(FILE *)` and `oc_trace_row(FILE *, ...)` become `oc_trace_open(OtkTrace *, FILE *)` and `oc_trace_row(OtkTrace *, ...)`. The 26 columns are declared in a table with the same formats, which is why the traces are byte-identical.
- `open-crank-run` reports a trace error and exits non-zero if the writer reports one.

**Platforms**

- `platforms/linux-socketcan/pacer.*` and `socketcan.*` are deleted. `open-crank-sil` and `open-crank-mc` use `OtkPacer` and `OtkSocketCan`, converting frames with `can_otk.h`.

**Tests and tools**

- `test_scenario.c`: four expectations change, all to nanoseconds, plus the error text "time 1 s is earlier" instead of "time 1.000 s is earlier".
- `tools/mutation_check.sh`: the mutation "scenario: unknown signal names accepted" targeted the parser, which now lives in open-tick, where its mutation check kills the same mutation. It is replaced by one that breaks open-crank's own binding: "scenario: events never reach the bench's signals". The count stays at 18.

## Decisions for the pull request

1. **How open-crank gets open-tick's source.**

   | Option | Trade-off |
   |---|---|
   | git submodule | pinned commit, works offline; contributors must clone with `--recursive` |
   | `FetchContent` with a pinned tag | nothing to remember when cloning; needs network at configure time |
   | sibling checkout (the trial used this) | the simplest, but CI and users would need two clones by hand |

   The CMake side is the same for all three; only the line that locates the source differs. A submodule matches "a commit in open-crank always means the same build", which the trace comparison relies on.
2. **Where `can_otk` lives.** In the trial it is in `opencrank_sim`, so `open-crank-mc` (the reference controller, a device under test) links the simulator library only for frame conversion. Moving `can_otk` to a small host-side library in `platforms/` would keep the controller program free of the simulator.
3. **Documentation in open-crank.** ARCHITECTURE.md §4.6 (runtime), the README's repository layout and the roadmap's Track R move to, or point at, open-tick.
4. **CI.** Add a job that compares the six traces against the ones from the commit before the pull request, so "bit-identical" is checked by CI and not only by this trial.
