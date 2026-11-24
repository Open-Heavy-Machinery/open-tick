# The verdict format

A verdict file says what the acceptance oracle concluded about each criterion of a run, in a shape a program can read. It has one row per criterion: verdict, how the criterion ended, its window, its reaction and its first violation. Its times are on the trace's time axis, so a viewer can draw them over the trace: [open-rig](https://github.com/Open-Heavy-Machinery/open-rig) does (planned, its v0.2).

The oracle writes it with `otk_oracle_write_verdicts()` (`opentick/oracle.h`), after the last `otk_oracle_step()` of the run. The verdict file and the text report of `otk_oracle_report()` come from the same state through the same functions in `core/src/oracle.c`, so they cannot disagree. The format is open-tick's since v0.2.1, and `core/src/oracle.c` defines it. The criteria it reports on are described in [criteria-format.md](criteria-format.md).

## What is guaranteed

`otk-flywheel examples/flywheel/step.csv --criteria examples/flywheel/step.accept.csv --verdicts step.verdicts.csv` writes [examples/flywheel/step.verdicts.csv](../examples/flywheel/step.verdicts.csv):

```
id,verdict,status,from_s,until_s,event,event_s,reaction,reaction_ms,fail_s,text
D1,PASS,ok,0.000000,5.000000,,,,,,duty never above 100 %
R1,PASS,ok,1.000000,4.000000,,,,,,drives while its speed feedback is fresh
F1,PASS,ok,4.200000,5.000000,fault.speed_lost=1,4.000000,measured,110,,no drive within 200 ms of losing the speed frames
```

- **Lines.** The first line is the header, exactly as above. Then one row per criterion, in the order of the criteria file. Lines end with `\n`, and the last line ends with `\n`. There are no comment lines and no blank lines.
- **Fields.** Comma-separated, never quoted. `text` is always the last column and takes the rest of the line, so it may contain commas, as in the criteria file. No other field contains a comma: ids, event labels, keywords and numbers cannot.
- **Columns.**

| Column | Content |
|---|---|
| `id` | the criterion's id, as in the criteria file |
| `verdict` | `PASS` or `FAIL`, the same as the text report |
| `status` | `ok`: passed. `violated`: the condition was false for longer than the tolerance. `not_evaluated`: no step fell inside the window. `not_covered`: the run ended before the window did. These are the four outcomes the text report describes. Only `ok` is a `PASS`. |
| `from_s`, `until_s` | the window, resolved and inclusive, in simulated seconds with six decimals |
| `event` | when `from` is an event plus an offset (the criterion measures a reaction): the event, as `<signal>=<value>`. Empty otherwise |
| `event_s` | the event's time, in seconds with six decimals; empty when `event` is |
| `reaction` | `measured`; `not_measured` (the run never reached `from`); `not_holding` (the condition did not hold at `from`). Empty when `event` is |
| `reaction_ms` | when `measured`: from the event to the start of the uninterrupted run of the condition that held at `from`. Whole milliseconds when exact, otherwise six decimals (`9.500000`), the same rule as the text report. Empty otherwise |
| `fail_s` | when `violated`: the start of the false run that failed, in seconds with six decimals. Empty otherwise |
| `text` | the criterion's text, as in the criteria file; never empty |

- **Time.** Seconds are simulated time, on the same axis as a trace's `t_s` ([trace-format.md](trace-format.md)). The end of a measured reaction is at `event_s + reaction_ms / 1000`.
- **Empty field.** An empty field means "does not apply to this criterion". It never means zero: a reaction of 0 ms is written `0`.

## What is not in the file

- **The observables' values at the failure.** The text report prints them; the verdict file does not, so its columns stay fixed.
- **The condition, the tolerance and the observables' timelines.** The criteria file has the first two; the trace has whatever the application recorded.
- **Totals.** Count the rows: a run passes when every `verdict` is `PASS`.
- **A version.** The format has no version field yet. It will get one before the first change that could break a reader.

## What readers should expect and tolerate

- **Find columns by header name.** Then take `text` as everything after the comma that ends the field before it. A future column would be added before `text`, so a reader that counts commas from the start breaks and a reader that uses the header does not.
- **Windows line endings and a byte-order mark** are not written by open-tick, but a file that went through other tools may have them. Tolerate both.
- **Windows that no step reached** (`not_evaluated`) still have their `from_s` and `until_s`. Draw them, but nothing was measured inside them.

## Who reads it

- [open-rig](https://github.com/Open-Heavy-Machinery/open-rig) *(planned, its v0.2)*: shades each window over the trace and marks the event, the end of the reaction and the first violation.
- open-crank *(planned)*: `open-crank-accept --verdicts` and `tools/board/bench.sh`, next to each trace.
- Spreadsheets and scripts: split each line on its first ten commas.
