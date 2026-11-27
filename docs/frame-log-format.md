# The frame log format

A frame log is the record of every CAN frame of one run: one row per frame that the application put on its bus or took off it, with **two times**. The application writes it through `otk_framelog_*` ([`core/include/opentick/framelog.h`](../core/include/opentick/framelog.h)). This page is the contract for the programs that read it, such as [open-rig](https://github.com/Open-Heavy-Machinery/open-rig) *(planned: its bus view)* or pandas. It says what open-tick guarantees and what it does not.

## An example

From [`examples/flywheel/delay.frames.csv`](../examples/flywheel/delay.frames.csv) (`otk-flywheel examples/flywheel/delay.csv --frames out.csv`):

```
# otk-frame-log 1; step_ns=10000000; bus_clock=sim; bus_clock_err_ns=0; source=otk-flywheel
t_s,t_bus_s,dir,link,id,flags,len,data,fault
0.010000000,0.010000000,to_dut,speed,100,,2,0000,
0.010000000,0.010000000,from_dut,,200,,1,00,
0.020000000,0.020000000,to_dut,speed,100,,2,0000,
0.020000000,0.020000000,to_dut,speed,101,,2,DC05,
```

## The two times

Both are on the run's own timebase: simulated time, the same axis as a trace's `t_s` ([trace-format.md](trace-format.md)) and a verdict file's seconds ([verdict-format.md](verdict-format.md)).

| Column | What it is | Present |
|---|---|---|
| `t_s` | **Logical time.** The simulated time of the step in which the frame crossed the machine's side. `to_dut`: the step that sent it. `from_dut`: the step that received it; for a frame lost on the way, the time the link lost it. **Always a multiple of `step_ns`**: the writer refuses anything else. | always |
| `t_bus_s` | **Bus time.** When the frame was on the side of the device under test, measured by the clock that the metadata line names (`bus_clock`). `to_dut`: when it was delivered towards the DUT. `from_dut`: when the DUT's frame was received from the bus. | empty when not measured, and for a `to_dut` frame that was lost (it never reached the bus) |

**`t_s` puts a frame on the time axis.** It is deterministic, and it is the axis of the trace and the verdicts; the oracle grades on it. `t_bus_s` is a measurement: show it beside the frame (and `t_bus_s − t_s` as a latency), never use it to move the frame.

Where `t_bus_s` comes from (`bus_clock`):

| `bus_clock` | Where | `t_bus_s` | `bus_clock_err_ns` |
|---|---|---|---|
| `sim` | in memory | the link's simulated times on the DUT's side; exact, so a link's delay shows exactly | `0` |
| `kernel` | SocketCAN, paced run | the kernel's timestamps (`SO_TIMESTAMPNS`), minus an anchor taken at t = 0 on the same clock (`otk_socketcan_clock_ns()`). A sent frame's time is that of the socket's own copy, which the kernel gives back when the frame leaves on the interface (`CAN_RAW_RECV_OWN_MSGS`). On a USB adapter this is when the frame reached or left the host, not when it was on the wire: the adapter's latency is in it | empty (not stated) unless the application measures it |
| `host` | SocketCAN, paced run, without kernel timestamps | the host's clock when the application read or wrote the frame | empty unless measured |
| `none` | a run that is not paced (free-run over SocketCAN) | always empty: wall time and simulated time are unrelated | empty |
| `bridge` | SocketCAN in a hard run (v0.4; `otk-flywheel --driver hard --can`) | the bridge's time since step 0, which is the run's clock in hard mode: each frame's kernel timestamp mapped REALTIME → MONOTONIC → bridge ([bridge-protocol.md §5](bridge-protocol.md#5-can-in-a-hard-run)). A sent frame's time is that of its own copy, as for `kernel` | the bound measured in that run: the clock mapping's largest bound plus the REALTIME → MONOTONIC step's error. Never a constant |

`otk_socketcan_bus_clock()` gives `none`, `kernel` or `host` for a socket and a run. When a frame has no kernel timestamp although the file says `kernel`, its `t_bus_s` is empty: a file never mixes clocks.

## What is guaranteed

- **Line 1, metadata.** `# otk-frame-log <version>; ` and then `key=value` pairs separated by `; `. This writer produces version `1`, with the keys `step_ns` (integer nanoseconds, > 0), `bus_clock` (`sim`, `kernel`, `host`, `bridge`, `none`), `bus_clock_err_ns` (integer nanoseconds, or empty: not stated) and, optionally, `source` (the program; no `;` in it). A reader ignores keys it does not know. Since it starts with `#`, `pandas.read_csv(path, comment="#")` skips it.
- **Line 2, header.** Exactly `t_s,t_bus_s,dir,link,id,flags,len,data,fault`. Readers find columns by name.
- **Rows.** One per frame. Comma-separated, **never a quote, a comma or a line break inside a field**, always nine fields, every line ends with `\n`. As in a trace, a reader can split on `,`.
- **Fields.**
  - `t_s`, `t_bus_s`: seconds with exactly nine decimals, printed from the integer nanoseconds (never through a `double`), so they are exact.
  - `dir`: `to_dut` (machine → device under test) or `from_dut` (device under test → machine).
  - `link`: the name of the link that carried the frame ([link-faults.md](link-faults.md)), or empty when no link did. Never a comma, quote, space or line break.
  - `id`: hexadecimal, upper case, no `0x`: 8 digits for a 29-bit identifier, 3 for an 11-bit one.
  - `flags`: `x` 29-bit, `f` CAN FD, `b` bit-rate switch, in that order; empty for a classic 11-bit frame.
  - `len`: the payload length, 0 to 64.
  - `data`: the payload, two upper-case hexadecimal digits per byte, no separators (empty when `len` is 0). As the receiver got it (after `corrupt`); for a lost frame, as it was sent.
  - `fault`: what the link did to the frame: empty (delivered unchanged, perhaps delayed), `drop`, `drop_one_in`, `bus_off` (lost when sent, or when it fell due while the bus was off), `overflow` (the link's line was full) or `corrupt` (delivered with one byte flipped). A delay changes no data and shows as `t_bus_s − t_s`.
- **Order.** Rows are in the order each frame's fate became known. With no delay, that is also `t_s` order. A delayed `to_dut` frame is written when it is delivered, after frames sent later: **a reader that needs time order sorts by `t_s`, keeping the file order for ties.**
- **Determinism.** In memory, with the free-run driver, two runs give the same file byte for byte (CI: `example_flywheel_frames_deterministic`).

## What is not in the file

- Error frames and remote (RTR) frames: the SocketCAN bridge skips them on receive.
- Signal-level transports (edges, pulse captures): open-tick v0.4 *(planned)*.
- What a frame means: names, PGNs and signals belong to the application's dictionary (for open-crank, a DBC file *(planned)*).
- The scenario's fault events themselves: they are in the scenario file. The log shows only what they did to frames.

## Versions

The version is the number after `# otk-frame-log`. A reader refuses a version it does not know. New metadata keys need no new version. A new column, or a change in the meaning of one, does.

## Writing one from an application

```c
OtkFrameLog log;
const OtkFrameLogConfig cfg = { otk_ns_from_ms(10), OTK_BUS_CLOCK_SIM, true, 0, "my-sim" };
otk_framelog_init(&log, fp, &cfg);

/* a frame the machine sends through a link, in memory */
OtkLinkInfo info;
if (!otk_link_push_ex(&link, &frame, t, &info)) {           /* lost when sent */
    const OtkFrameRow r = { t, OTK_FRAMELOG_NO_TIME, OTK_FRAME_TO_DUT, "speed", &frame,
                            otk_frame_fault_from_link(&info) };
    otk_framelog_frame(&log, &r);
}
while (otk_link_pop_ex(&link, now, &out, &info)) {          /* delivered, or lost when due */
    const bool lost = info.lost != OTK_LINK_LOST_NONE;
    const OtkFrameRow r = { info.pushed, lost ? OTK_FRAMELOG_NO_TIME : now, OTK_FRAME_TO_DUT, "speed", &out,
                            otk_frame_fault_from_link(&info) };
    otk_framelog_frame(&log, &r);
    if (!lost) { /* deliver out */ }
}
```

The writer refuses a row with an invalid frame, a `t` off the step grid, a bus time when `bus_clock` is `none`, a bus time on a lost `to_dut` frame, or a bad link name. Nothing is written for that row, and the first error is kept (`otk_framelog_ok()`, `otk_framelog_error()`), as for traces. `otk-flywheel` ([`examples/flywheel/flywheel.c`](../examples/flywheel/flywheel.c), `--frames`) shows the whole thing.
