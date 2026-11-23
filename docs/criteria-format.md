# The criteria format

A criteria file says what a device under test (DUT) must do during one scenario, judged only by the CAN frames it sends. The acceptance oracle ([`core/include/opentick/oracle.h`](../core/include/opentick/oracle.h)) reads it and grades a run against it. The format belongs to open-tick since v0.2.0 and is defined by [`core/src/oracle.c`](../core/src/oracle.c); this page is the contract for people who write criteria and for applications that load them. It states what open-tick guarantees and what it does not.

## Observables come from the application

Criteria do not name CAN identifiers or bytes. They name **observables**, which the application declares in a table (`OtkOracleTable`) together with a decoder that turns a frame into new values for them. For each observable the application gives:

- a **name**, used in conditions;
- a **stale time**: once the last update is older than that, the observable falls back to its default;
- a **default**: a number, or a gap (no value). The default also holds before the first update.

The decoder also decides **which frames count**. open-tick does not know who sent a frame; an application that shares the bus with other nodes filters in its decoder (open-crank, for example, counts only frames whose J1939 source address is the DUT's). `otk-flywheel` has one observable, decoded from the DUT's DUTY frame (`0x200`):

| Observable | Value | Stale after | Default |
|---|---|---|---|
| `duty_pct` | the commanded duty, 0.5 % per bit | 100 ms | 0 |

The stale time is the application's statement of what silence means. In the flywheel the machine treats a duty command older than 100 ms as zero, so the oracle does too: a DUT that goes quiet is graded as a DUT that commands zero.

## Criteria files

[`examples/flywheel/step.accept.csv`](../examples/flywheel/step.accept.csv) grades `otk-flywheel` on [`step.csv`](../examples/flywheel/step.csv):

```
# Acceptance criteria for step.csv: what any speed controller on this
# flywheel must do, judged only by its DUTY frames (0x200). Format:
# docs/criteria-format.md.
id,from,until,tolerance_ms,condition,text
D1,begin,end,0,duty_pct <= 100,duty never above 100 %
R1,1.0,fault.speed_lost=1,0,duty_pct > 0,drives while its speed feedback is fresh
F1,fault.speed_lost=1+200ms,end,0,duty_pct == 0,no drive within 200 ms of losing the speed frames
```

- Lines starting with `#` and blank lines are ignored, also when indented. The first other line is the header, exactly `id,from,until,tolerance_ms,condition,text`. Lines may end in `\n` or `\r\n`.
- There is no quoting. `text` is everything after the fifth comma, so it may contain commas. It must not be empty.
- `id`: letters, digits, `_`, `.` or `-`, unique in the file.
- `from` and `until` bound the window, **inclusive**. Each is a reference with an optional offset `+Nms` or `-Nms` (whole milliseconds):
  - `begin`: t = 0;
  - `end`: the scenario's `end` event;
  - a time in seconds, such as `1.0`;
  - `<signal>=<value>`: the **first** event of the scenario that sets that signal to that value, such as `fault.speed_lost=1`. The signal must be in the application's signal table.

  A reference is the exact time of the event in the scenario, in nanoseconds; a time in seconds is rounded to the nearest nanosecond, as scenario times are. A window must not start before t = 0, end after `end`, or be empty.
- `tolerance_ms`: whole milliseconds.
- `condition`: `term (&& term)*`, optionally followed by `-> term (&& term)*` (implication: if the left side holds, the right side must hold). A term is `observable op number`, with `op` one of `== != < <= > >=`. **A term on a gap is false**, `!=` included.

## Evaluation

The application calls the oracle once per step: first with the frames the DUT sent in that step, then to evaluate the step. Each step covers **one step period, set by the application** when it loads the criteria (`otk-flywheel` and open-crank use 10 ms).

- At every step `t` inside a window, the condition is evaluated with the values after that step's frames, and the defaults of the observables that are stale.
- The criterion **fails** if the condition is false at consecutive steps that cover **more** than `tolerance_ms`. With 0, one false step fails; with a 10 ms step and 20, two consecutive false steps pass and three fail. Separate false runs do not add up.
- A window that the run did not reach to its end **fails**: its last step must be within one step period of `until`. The step at `end` is graded too, so a runner must evaluate it, with the frames the DUT sent in the last step.

## Errors

A wrong header, a line without six fields, a bad or duplicate id, an unknown observable or signal, an event the scenario does not have, an invalid operator or number, a bad tolerance, a window that starts before t = 0, ends after `end` or is empty, and a line or text that is too long are format errors. Loading returns false with a message that gives the line number, such as `line 4: unknown observable 'rpm'`, and the oracle then holds no criteria. A file with no criteria is an error too. What the application does with the error (an exit code, a message) is up to each runner.

**Limits:** 64 criteria per file, 16 observables per table, 8 terms per condition (both sides of `->` together), lines of 511 characters, ids of 15 characters, texts of 255 characters. An offset is at most 4,000,000,000 ms and a tolerance at most 3,600,000 ms.

## Report

One line per criterion: `PASS` or `FAIL`, the id, the text and the window in seconds. A failure adds the first instant of the violation (the start of the false run that failed) and the values of the condition's observables then. A window that no step reached, or that the run did not cover to its end, says so.

When `from` is an event plus a positive offset, the line also gives the **reaction time**: how long after the event the condition had been holding without interruption up to `from`. It is printed in whole milliseconds when it is exact, and with six decimals when it is not (an event that does not fall on a whole millisecond, such as `reaction: 9.500000 ms after go=1`). If the condition does not hold at `from`, the line says `reaction: not holding at …` instead.

`otk-flywheel examples/flywheel/step.csv --criteria examples/flywheel/step.accept.csv` ends with:

```
PASS  D1  duty never above 100 %  [0.00 s .. 5.00 s]
PASS  R1  drives while its speed feedback is fresh  [1.00 s .. 4.00 s]
PASS  F1  no drive within 200 ms of losing the speed frames  [4.20 s .. 5.00 s]  reaction: 110 ms after fault.speed_lost=1
otk-flywheel: PASS (3 of 3 criteria)
```

A criterion that asked for no drive within 50 ms of the fault would fail, because the toy controller needs 110 ms:

```
FAIL  F0  no drive within 50 ms of losing the speed frames  [4.05 s .. 5.00 s]  reaction: not holding at 4.05 s  first violation at 4.05 s: duty_pct = 88
```

The last line of each example (`otk-flywheel: PASS …`) is the runner's own verdict, not part of the oracle's report.

## What is not guaranteed

- **A version.** The format has no version field yet. It will get one before the first change that could break an existing file.
- **The limits.** The numbers above may change; a change is announced in the [CHANGELOG](../CHANGELOG.md).
