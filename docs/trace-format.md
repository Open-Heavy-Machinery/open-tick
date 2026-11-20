# The trace format

A trace is the record of one run: one row per step, written by the application through `otk_trace_*` ([`core/include/opentick/trace.h`](../core/include/opentick/trace.h)). This page is the contract for programs that read it, such as [open-rig](https://github.com/Open-Heavy-Machinery/open-rig), a spreadsheet or pandas. It states what open-tick guarantees and what it does not.

## What is guaranteed

A trace is a text file in this shape:

```
t_s,rpm,duty_pct,dut_state
0.010,0.00,0.0,RUN
0.020,3.10,100.0,RUN
```

- **Lines.** The first line is the header; each following line is one row. Lines end with `\n`. The last line ends with `\n`.
- **Fields.** Comma-separated. **There is never a quote, a comma or a line break inside a field**: the writer refuses text that has one (and reports an error) instead of escaping it. A reader can split on `,` with no parser.
- **Columns.** The header names the columns, in order. Names are not empty, contain no comma, quote or line break, and are chosen by the application. **Every row has exactly as many fields as the header.** A row that is short is padded with empty fields and the writer reports an error, so the file stays aligned.
- **No shifting.** A value is written in the column the application registered for it, or not at all. A trace that would put a value in the wrong column is an error at write time, not a silently shifted file.
- **Time.** The first column of the traces written so far is `t_s`, the simulated time in seconds, which never decreases. open-tick does not require it: the column that carries time is the application's choice, and a reader should be told which one, or look for `t_s`.
- **Empty field.** An empty field means "no value in this row" (`otk_trace_empty`). It is a gap, not zero.

## What is not in the file

- **Types.** The header has names only. The application registers each column as real, integer, unsigned or text, but that does not reach the file. A reader decides per column: if every non-empty field is a number, it is a number column.
- **Units.** There are none in the file. By convention the **name ends in the unit**: `rpm`, `_nm`, `_pct`, `_bar`, `_kpa`, `_c`, `_s`, `_frac`. It is a convention of the applications, not a rule of the format.
- **Precision.** Real values are written with a fixed number of decimals chosen per column, so a value is rounded to that many decimals. Do not read the last digit as measured.
- **A version.** The format has no version field yet. It will get one before the first change that could break a reader.

## What readers should expect and tolerate

- **`nan` and `inf`.** A real value that is not finite is printed the way the C library prints it (`nan`, `-nan`, `inf`) and is not rejected. It means a bug upstream. Read it as a gap.
- **Windows line endings and a byte-order mark** are not written by open-tick, but a file that went through other tools may have them. Tolerate both.
- **Many columns.** open-crank's trace has 26. Reading by name, not by position, survives a new column.
- **Size.** One row per step: a 25 s run at 10 ms is 2,500 rows.

## Who reads it

- [open-rig](https://github.com/Open-Heavy-Machinery/open-rig): plots one trace, or two on the same time axis.
- open-crank's `tools/compare_traces.sh`: byte-for-byte comparison, which relies on the writer being deterministic.
- Spreadsheets and pandas (`pandas.read_csv` infers the types, and reads empty fields as NaN).
