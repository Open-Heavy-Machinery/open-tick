# The host ↔ bridge protocol, version 1

**Status: specification, not built.** This is the contract that open-tick v0.4's hard driver *(planned)* and the Tang Nano 20K gateware *(planned)* will implement, and that every later signal-level bridge (for example the Raspberry Pi Pico) will speak ([ROADMAP.md](../ROADMAP.md), v0.4). Nothing in this repository implements it yet.

It carries what crosses the USB link between the host, which runs the core and the plant, and a signal-level bridge, which produces and captures edges and levels on the pins of the device under test (DUT) ([VISION.md §5.2](../VISION.md#52-two-kinds-of-transport)). Message transports such as CAN stay on the host and do not use it.

Numbers marked **(BL616)** belong to the Tang Nano 20K's on-board USB link, a BL616 that presents itself as an FT2232. They were measured on 2026-11-26 ([Measured link](#measured-link-bl616)). Another link, for example an FT232H, declares its own.

## 1. Time

- **The bridge owns time.** It counts **ticks** of its oscillator (27 MHz on the Tang Nano 20K: 37.037 ns per tick) and **steps**. In a hard run, the bridge's tick is the run's one authoritative clock ([VISION.md §5.1](../VISION.md#51-one-clock-one-source-of-truth)).
- **The application sets the step.** It is 1 ms for open-crank's Level 2, that is 27 000 ticks. open-tick never chooses it.
- **Step 0** is the step at which the bridge applies the first setpoint. The core's time of step k is k × step, in nanoseconds.
- Ticks are counted in 64 bits and never wrap in practice.

## 2. Look-ahead

The host computes the plant ahead of the bridge and keeps the bridge's **setpoint buffer** near **H** steps. H is **16 ms by default**, and configurable.

- **Why H = 16 ms.** With a Python host and 60 s runs, H = 16 ms held with every CPU busy on two machines; H = 8 ms did not ([Measured link](#measured-link-bl616), F-16).
- **What the fidelity contract declares** ([§7](#7-the-fidelity-contract-version-1)):
  - the plant's lead, H;
  - the delay from the DUT to the plant: a capture, or a CAN frame from the DUT, reaches the plant at most H plus one round trip after it happened, about 22 ms (BL616).
- **What that delay costs.** It is dead time on the path from the DUT to the plant.
  - In a diesel, an injection turns into torque within about 4–5 ms: about 1 ms of ignition delay, then combustion up to 30–40° after top dead centre, and at 1500 rpm 1° is 0.11 ms. The plant therefore applies each injection about 17 ms later than an engine would.
  - **Mean-speed control is faithful within that dead time:** 6–18° of extra phase lag at 1–3 Hz, which the fidelity contract declares.
  - **Per-cylinder dynamics are not.** A six-cylinder engine at 1500 rpm fires every 13.3 ms, so the torque of one injection lands in the next cylinder's interval.
  - Only a link with a shorter round trip removes this; the BL616's is about 6 ms (F-14). A host in C may allow a smaller H; not measured yet.
- **An underrun is a step that finds the buffer empty.**
  - The bridge **holds the last setpoint it applied** (speed and analog values), so the tooth wheel stays phase-continuous and the DUT sees no step.
  - It counts the underrun and reports the count in every reply.
  - A run with any underrun is reported as **degraded**.
  - A longer silence is not an underrun but a lost link ([§3.5](#35-link-timeout)).

## 3. Exchanges

**Only the host starts an exchange.** It sends one request; the bridge answers each complete request exactly once and never sends anything unasked.

- **Why.** Sustained traffic in both directions at once loses data on the BL616 and can hang it until a power cycle (F-1). A ping-pong never has both directions busy.
- **Budget (BL616):** at most **448 B per direction per exchange**. One exchange then takes about 6 ms. A reply above about 512 B costs about 6 ms more per 512 B (F-13).

### 3.1 Framing

Every message, in both directions:

| Field | Type | Value |
|---|---|---|
| `sync` | u8 | `0xA5` |
| `version` | u8 | 1 |
| `type` | u8 | the message type: below |
| `seq` | u16 | request: chosen by the host, +1 per new request; reply: the request's |
| `len` | u16 | payload bytes |
| payload | `len` bytes | §3.2, §3.3 |
| `crc` | u16 | CRC-16/CCITT over `version` … end of payload |

| Type | Message | Payload |
|---|---|---|
| `0x01` / `0x81` | exchange request / reply | §3.2 / §3.3 |
| `0x02` / `0x82` | configure / its reply | §3.4 |
| `0x03` / `0x83` | start / its reply | §3.4 |
| `0x04` / `0x84` | stop / its reply | §3.4 |

Every reply has the type of its request plus `0x80` and the request's `seq`, with two exceptions:
- **A request whose CRC fails** gets an exchange reply (`0x81`) with `error.crc` set and the `seq` as received, and nothing is done. Its type cannot be trusted, and only the exchange reply carries every status field. If the `seq` does not match, the host sends the request again after its timeout.
- **A request of an unknown type** also gets an exchange reply, with `error.state` set.

- **Byte order:** all integers are little-endian and unsigned. There are no floating-point values on the wire.
- **CRC:** CRC-16/IBM-3740, also called CCITT-FALSE: polynomial `0x1021`, initial value `0xFFFF`, no reflection, no final XOR. Its Hamming distance is 4 up to 32 751 bits, so it catches every error of up to three bits in any message this protocol allows (≤ 1 KiB).
- **Resynchronisation.** A reader resynchronises on `sync`. It discards bytes until a header whose `len` fits and whose CRC checks. The BL616 prints a text banner on the UART after the FPGA is programmed (F-18), so a parser must expect junk before the first message.

### 3.2 Request (host → bridge)

| Field | Type | Meaning |
|---|---|---|
| `first_step` | u32 | the bridge step of the first setpoint in this request |
| `n` | u8 | number of setpoints that follow |
| setpoints | `n` × 12 B | one per step, in step order |

One setpoint, 12 bytes:

| Field | Type | Encoding |
|---|---|---|
| `crank_phase_inc` | u32 | phase added to the rotary-pattern oscillator **every tick**; the oscillator's 48-bit accumulator spans one 720° cycle |
| `analog[4]` | 4 × u16 | full scale: 0 = 0 V, 65 535 = the bridge's full scale (3.3 V). The bridge declares its real resolution in its fidelity contract |

**Rotary pattern (crank and cam).**
- A 48-bit phase accumulator gains `crank_phase_inc` every tick, and 2⁴⁸ is one 720° cycle.
- The accumulator's top 32 bits index the pattern tables: the 60−2 crank wheel over 360°, the cam over 720°.
- For a speed of *n* rpm, `crank_phase_inc` = round(*n* / 120 × 2⁴⁸ / 27 000 000). 2400 rpm is 208 499 983.
- One LSB is 1.15 × 10⁻⁵ rpm, and the largest speed is 49 438 rpm.
- A 32-bit accumulator would give only 0.75 rpm per LSB, which is why the accumulator is 48 bits.
- The bridge integrates speed between steps, so edges stay phase-continuous at any speed even though the plant steps every millisecond ([VISION.md §5.2](../VISION.md#52-two-kinds-of-transport)).

**The setpoints must continue the buffer:** `first_step` = the last step already queued + 1.
- **A request that repeats the last applied `seq`** is a retransmission (the host did not get the reply). The bridge answers again and applies nothing.
- **A request with any other gap or overlap** is refused: the bridge applies none of its setpoints and sets `error.sequence`.
- **A setpoint for a step that has already passed** is dropped and counted in `error.late`.
- **The bridge applies a request's setpoints only after its CRC checks.** A request whose CRC fails gets an exchange reply with `error.crc` set ([§3.1](#31-framing)), and nothing is applied. The host sends it again.

### 3.3 Reply (bridge → host)

| Field | Type | Meaning |
|---|---|---|
| `t2` | u64 | tick at which the request's first byte arrived |
| `t3` | u64 | tick at which this reply was queued |
| `step` | u32 | current step |
| `state` | u8 | 0 idle, 1 configured, 2 running, 3 stopped, 4 aborted ([§3.4](#34-configure-start-and-stop)) |
| `fill` | u16 | setpoints in the buffer |
| `underruns` | u32 | underruns since step 0, saturating at 2³² − 1 |
| `error` | u16 | bit field: `crc` (bit 0), `sequence` (1), `late` (2), `capture_overflow` (3), `aborted` (4), `state` (5: the message is not allowed in the current state); the counts are in the next field |
| `error_count` | u16 | setpoints dropped as late, plus captures lost to overflow, since the last reply |
| `applied` | 12 B | the setpoint applied at `step`, as in §3.2 |
| `n_cap` | u8 | captures that follow |
| captures | `n_cap` × 9 B | since the last reply, oldest first |

- **`applied` is there so that the host can check the bridge's behaviour.** In the spike, a FIFO whose data reached no output was optimised away without a warning (F-19). The host checks that the setpoint the bridge applied is the one it sent for that step.
- **Counters (F-17).** A counter's difference is trusted only while the counter is below its maximum. Every measured run starts from a freshly programmed bridge.

One capture, 9 bytes:

| Field | Type | Meaning |
|---|---|---|
| `channel` | u8 | the capture channel (one per injector) |
| `start` | u32 | the low 32 bits of the tick at which the pulse started |
| `width` | u32 | the pulse width, in ticks |

- **The host extends `start` to 64 bits** with `t3`: it takes the latest tick at or before `t3` whose low 32 bits are `start`. This is exact for any capture younger than 2³² ticks (159 s).
- **If more captures are waiting than fit in one reply,** the rest go in the next reply. If the bridge's capture memory is full, a new capture is lost, `error.capture_overflow` is set and the loss is counted.

### 3.4 Configure, start and stop

The bridge has five states:

| From | Message or event | To |
|---|---|---|
| idle, configured, stopped, aborted | configure | configured |
| configured | start | running |
| running | stop | stopped |
| running | link timeout ([§3.5](#35-link-timeout)) | aborted |

- **idle:** after the bitstream is loaded. Only `configure` is allowed.
- **configured:** exchange requests are allowed, so that the host can fill the buffer before step 0.
- **running:** steps are counted and setpoints applied.
- **stopped, aborted:** the outputs are at rest; only `configure` is allowed.

**At rest:**
- the phase increment is 0, so the crank and cam outputs hold their level;
- every analog output is at 0;
- captures are off;
- the buffer is empty.

**Configure** (`0x02`, 22 B; allowed in idle, configured, stopped and aborted):

| Field | Type | Meaning |
|---|---|---|
| `step_ticks` | u32 | ticks per step: 27 000 for 1 ms on a 27 MHz bridge |
| `crank_teeth` | u8 | teeth of the crank wheel, counting the missing ones (60 for 60−2) |
| `crank_missing` | u8 | missing teeth (2) |
| `crank_offset` | u32 | the phase, in the top 32 bits of the accumulator (2³² = 720°), at which the first tooth after the gap begins |
| `cam_start`, `cam_end` | 2 × u32 | the cam output is high from `cam_start` to `cam_end`, same unit |
| `capture_mask` | u8 | bit k enables capture channel k |
| `capture_polarity` | u8 | bit k = 1: channel k's pulse is high |
| `link_timeout_steps` | u16 | §3.5; 100 by default |

The crank pattern repeats every 360°. With p = (phase − `crank_offset`) mod 2³¹, tooth i = ⌊p × `crank_teeth` / 2³¹⌋. The output is high in the first half of each tooth's pitch, for i < `crank_teeth` − `crank_missing`.

The reply (`0x82`) carries:
- `status` u8: 0 accepted, 1 refused (wrong state), 2 refused (a value the bridge cannot do);
- what the bridge has: `tick_hz` u32, `n_capture` u8, `n_analog` u8, `analog_bits` u8 (effective resolution).

Configuring clears the buffer and every counter.

**Start** (`0x03`, no payload; allowed in configured, with at least one setpoint queued for step 0):
- step 0 begins `step_ticks` after the request's first byte arrived;
- the reply (`0x83`) carries `status` u8 and `t_start` u64, the tick at which step 0 begins;
- step k begins at `t_start` + k × `step_ticks`.

**Stop** (`0x04`, no payload; allowed in running):
- the outputs go to rest at once;
- the reply (`0x84`) carries `status` u8, the last `step` u32 and `underruns` u32.

The host stops every run it starts.

### 3.5 Link timeout

In running, if no request with a valid CRC arrives for `link_timeout_steps` steps (100 ms by default), the bridge:
- puts its outputs at rest;
- goes to aborted;
- sets `error.aborted` in every reply from then on.

An aborted run cannot be resumed: the host configures and starts a new one. This keeps a host that crashed or was unplugged from leaving the DUT with a spinning crank and frozen sensors. A few late exchanges are not a lost link: they show as underruns ([§2](#2-look-ahead)).

## 4. Clock mapping

Every exchange is a timing sample:
- **T1:** the host sends.
- **T2:** the bridge receives.
- **T3:** the bridge replies.
- **T4:** the host receives.

From each sample:
- offset = ((T2 − T1) + (T3 − T4)) / 2;
- bound = ((T4 − T1) − (T3 − T2)) / 2.

T2 and T3 are in the reply, in ticks.

- **The host keeps a line** (offset and drift) through the samples with the lowest delay in a sliding window. It tracks the drift continuously: −5.6 to −6.1 ppm on the board measured.
- **One sample is good to about half its round trip.** Back to back that is about 3 ms (BL616). Through the lowest-delay samples, the line is good to p99 45–85 µs (F-15).
- **The host's clock is `CLOCK_MONOTONIC`.** SocketCAN's kernel timestamps (`SO_TIMESTAMPNS`) are `CLOCK_REALTIME`. They are mapped to `CLOCK_MONOTONIC` first, and that step's own error is measured and added.

## 5. CAN in a hard run

CAN stays on the host (SocketCAN). In a hard run's frame log ([frame-log-format.md](frame-log-format.md)):
- **`bus_clock=bridge`.** Every CAN frame's kernel timestamp is mapped REALTIME → MONOTONIC → bridge ticks, so CAN frames and captures share one timebase.
- **`bus_clock_err_ns` is the bound measured in that run,** from its best samples and residuals, plus the REALTIME → MONOTONIC step. It is never a constant.
- **Frames to the DUT go out when the bridge reaches their step, not when the host computes them.** The plant computes step k about H steps early. The host maps the start of step k (`t_start` + k × `step_ticks`) to `CLOCK_MONOTONIC` and sends the frame then. Sent at once, every frame would reach the DUT about H early: the paced loop's one-step lead (F-5), multiplied by H.
- **Frames from the DUT reach the plant late.** A frame received at bridge time t is used by the first step the plant computes after it arrives, which is for about t + H. It is part of the DUT-to-plant delay that the fidelity contract declares ([§2](#2-look-ahead)).

## 6. Budget of one exchange (BL616)

Request with 16 setpoints: 9 B framing + 5 B header + 16 × 12 B = **206 B**, within 448.

Reply with 8 captures: 9 B framing + 43 B fixed fields + 1 B (`n_cap`) + 8 × 9 B = **125 B**.

- **Captures at the worst speed.** A six-cylinder four-stroke at 2400 rpm injects 120 times a second, about one capture per 6 ms exchange.
- **The largest request, 36 setpoints (446 B), still fits.** A host that falls behind can then catch up by more than two exchanges' worth of steps at once.

## 7. The fidelity contract, version 1

Both sides declare their limits in the same file format, in the style of open-tick's other formats: a metadata line, a fixed header, then one row per signal. Before a run, the runner checks the machine's file against the bridge's. It refuses a run that cannot be faithful and names the signal and the number that fails (ROADMAP v0.4, exit criterion 4).

```
# otk-fidelity 1; side=machine; source=open-crank 0.4
signal,direction,transport,max_rate_hz,resolution_ns,latency_ns,jitter_ns,logic_v
crank,to_dut,rotary,2400,100,,1000,3.3
cam,to_dut,rotary,20,100,,1000,3.3
inj1,from_dut,capture,20,1000,22000000,,3.3
oil_pressure,to_dut,analog,1000,,2000000,,3.3
```

- **`side`:** `machine` (what it requires) or `bridge` (what it guarantees).
- **`direction`:** `to_dut` or `from_dut`.
- **`transport`:** `rotary`, `capture`, `analog` or `level`.
- **The numbers** are whole numbers in the unit their column names. An empty cell means "no requirement" on the machine side and "not guaranteed" on the bridge side.
- **The bridge's metadata line** carries:
  - the look-ahead H (`lead_ns`);
  - the DUT-to-plant delay (`dut_to_plant_delay_ns`, at most H plus one round trip);
  - the link the numbers were measured on (`link`, for example `bl616-3mbaud`).

## 8. A hard run, in order

1. **Load the bitstream.** Counters then start from zero (F-17). The host drains the UART until it is silent, because the BL616 prints a banner (F-18).
2. **Configure:** the reply's capabilities are checked against the machine's fidelity file before anything runs (ROADMAP v0.4, exit criterion 4).
3. **Fill the buffer:** an exchange with `first_step` 0 and H setpoints.
4. **Start.** From then on, exchanges keep the buffer near H.
5. **Calibrate the analog outputs**, if the application can report what the DUT reads.
   - Why: the bridge's 3.3 V and the DUT's ADC reference are different supplies, so an analog value has a gain error of up to a few per cent.
   - How: for the first 100 steps, every analog channel is held at 10 % of full scale for 50 steps, then at 90 % for 50 steps. The DUT reports the values it read, in a frame the application defines (open-crank: a bench-only frame).
   - The host fits a gain and an offset per channel, uses them for the rest of the run and writes them into the trace's metadata.
   - Without calibration, the analog accuracy is the uncalibrated one the bridge declares.
   - Setpoints are best kept between 5 % and 95 % of full scale.
6. **Run the scenario.**
7. **Stop.** The run is degraded if it had any underrun, and aborted if the link timed out.

Every hard run records the USB path of the bridge (bus and ports, the hubs on the way, the link speed) in its trace's metadata. A bridge's link numbers hold for the topology they were measured on.

The example's numbers are illustrative. Each machine and bridge writes its own; open-crank's come with its v0.4.

## Measured link (BL616)

Tang Nano 20K, on-board BL616 USB debugger with Sipeed's firmware, UART at 3 Mbaud, a Python host. These are the numbers the decisions above rest on. Reports, sources and the checksums of the raw data are in [measurements/2026-11-26-bl616/](measurements/2026-11-26-bl616/).

**F-13: how much one exchange carries.** Back-to-back exchanges, 200 per size up to 448 B and 100 per size above; none lost or corrupted.

| Request B | Reply B | Round trip p50 / p99 |
|---|---|---|
| 0 | 0 | 5.98 / 6.10 ms |
| 448 | 448 | 5.95 / 6.05 ms |
| 1024 | 0 | 5.97 / 6.08 ms |
| 0 | 1024 | 17.9 / 18.1 ms |
| 4096 | 4096 | 65.8 / 66.0 ms |

**F-14: latency depends on phase.** With one exchange every 10 ms, the round trip is p1 0.26 ms, p50 3.3 ms, p99 6.2 ms. Back to back it is always about 6 ms. Inside the bridge, an exchange takes 26 µs. This is inferred, not documented: the BL616 forwards on a cycle of about 6 ms.

**F-15: clock mapping.** 6000 samples in 60 s.

| Samples used | Share | Residual around the line p99 | Drift |
|---|---|---|---|
| delay < 300 µs | 1.7 % | 45 µs | −6.05 ppm |
| delay < 500 µs | 4.1 % | 84 µs | −5.61 ppm |
| delay < 1000 µs | 11.1 % | 213 µs | −5.58 ppm |

Good samples came with gaps of up to 16 s.

**F-16: holding H.** 60 s per run, about 60 000 steps, with at most 30 setpoints of 2 B per request; the bridge reprogrammed before every run (F-17). Underruns, and the minimum buffer fill at the replies:

| H | Host A, idle | Host A, 22 busy loops | Host B (hub), idle | Host B (hub), 20 busy loops |
|---|---|---|---|---|
| 8 ms | 0 (min 7) | **82 (0.137 %)** (min 4) | 0 (min 6) | **59 (0.098 %)** (min 6) |
| 16 ms | 0 (min 14) | 0 (min 11) | 0 (min 15) | 0 (min 6) |
| 32 ms | 0 (min 25) | 0 (min 26) | 0 (min 30) | 0 (min 22) |

- No exchange was lost in any run. Every applied setpoint matched the one sent, except the first of each run, before any setpoint was applied.
- Host A and host B are two Linux PCs with 22 and 20 CPUs, using the same board.
- **The bitstream was identical on both** (md5 `f1d6afb79fe0b2791d68a22b6c75c2a4`), built with the same pinned toolchain.
- On host B the margin under load was smaller: 6 steps left at the worst moment, against 11 on host A.
- **Host B's runs above went through a USB 2.0 hub,** which the first version of this page did not say. Repeated with the BL616 on a root port of host B (`host-b-direct/`, with its `usb.txt`), the link behaved the same: the about 6 ms belong to the BL616, not to the path.

  | Host B | M1, 448/448 B p50 / p99 | M3 H = 8, busy | M3 H = 16, busy (min fill) | M3 H = 32, busy (min fill) |
  |---|---|---|---|---|
  | through a hub | not measured | 59 underruns | 0 (6) | 0 (22) |
  | root port | 5.90 / 6.24 ms | 78 underruns | 0 (8) | 0 (27) |

- **Clock mapping on host B's root port** (M2, `host-b-direct/m2_sync_analysis.txt`): round trip p1 0.54 ms, p50 3.6 ms, p99 6.6 ms. Low-delay samples were rarer than on host A: 6 of 6000 below 300 µs, and 51 (0.9 %) below 500 µs, with a residual p99 of 91 µs. The share of good samples depends on the phase between the host's requests and the BL616's cycle.

**Throughput, from the first spike** (8 B echo, then paced bulk, 115 200 to 3 000 000 baud): the round trip is about 6 ms at every rate, and 3 375 000 baud does not run. With echo, where both directions run at once, at most about 64 B are in flight per round trip. That is the limit of echo, not of a ping-pong (F-13).

## Still open (to be measured, not decided)

- **A smaller H with a host in C.** Every number above comes from a Python host.
- **Regular low-delay samples.** Scheduling each exchange just after the BL616's cycle (F-14) might give them at a steady rate instead of in bursts. Not tried. The hard driver will try it first: on host B's root port only 0.1 % of the samples had a delay below 300 µs.
- **The error of the REALTIME → MONOTONIC step** for SocketCAN timestamps. Not measured yet.

The F-numbers refer to the project's findings log, kept by the maintainers; the evidence for each is in [measurements/2026-11-26-bl616/](measurements/2026-11-26-bl616/).
