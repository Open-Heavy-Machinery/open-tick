#!/usr/bin/env python3
"""Host side of spike 2 (pingpong.v): input to the open-tick v0.4 protocol spec.

Three measurements, all host-initiated ping-pong (never both directions
streaming, findings F-1):

  sizes  M1: round trip and losses for request payloads and reply fillers of
         growing size; stops at the first lost or corrupted exchange.
  sync   M2: small exchanges every PERIOD ms. NTP-style: T1 host send, T2
         bridge receive (first byte), T3 bridge reply queued, T4 host
         receive. offset = ((T2-T1)+(T3-T4))/2, delay = (T4-T1)-(T3-T2),
         error bound of one sample = delay/2. Also the drift of the 27 MHz
         crystal against the host's CLOCK_MONOTONIC (ppm), by a line through
         the offsets of the lowest-delay samples.
  hold   M3: keep the bridge's setpoint FIFO near H steps (1 step = 1 ms),
         sending a counter as setpoints; count underruns and check that the
         setpoint applied at each reply is the one sent for that step.

Spike code, not open-tick code. Uses the system python3 and pyserial, as
spike 1 did.
"""
import argparse
import math
import struct
import sys
import time

import serial

F_TICK = 27_000_000
HDR = 28


def now_ns():
    return time.clock_gettime_ns(time.CLOCK_MONOTONIC)


def pct(vals, p):
    if not vals:
        return float("nan")
    s = sorted(vals)
    return s[max(1, math.ceil(p / 100.0 * len(s))) - 1]


class Link:
    def __init__(self, dev, baud):
        self.port = serial.Serial(dev, baud, timeout=0.05)
        # The BL616's firmware prints a console banner on this UART after the
        # FPGA is programmed: drain until 300 ms of silence.
        quiet_since = time.monotonic()
        while time.monotonic() - quiet_since < 0.3:
            if self.port.read(4096):
                quiet_since = time.monotonic()
        self.port.reset_input_buffer()
        self.seq = 0

    def exchange(self, typ, payload=b"", rlen=0, timeout_s=0.5):
        """One ping-pong. Returns (reply dict or None, t1_ns, t4_ns)."""
        self.seq = (self.seq + 1) & 0xFF
        body = struct.pack("<BBHH", typ, self.seq, len(payload), rlen) + payload
        req = b"\xA5" + body + bytes([sum(body) & 0xFF])
        want = HDR + rlen + 1
        t1 = now_ns()
        self.port.write(req)
        got = b""
        deadline = time.monotonic() + timeout_s
        while len(got) < want and time.monotonic() < deadline:
            got += self.port.read(want - len(got))
        t4 = now_ns()
        if len(got) < want:
            self.port.reset_input_buffer()
            return None, t1, t4
        r = {
            "sync": got[0], "seq": got[1], "status": got[2],
            "t_rx": int.from_bytes(got[3:9], "little"),
            "t_tx": int.from_bytes(got[9:15], "little"),
            "step": int.from_bytes(got[15:19], "little"),
            "fill": int.from_bytes(got[19:21], "little"),
            "under": int.from_bytes(got[21:23], "little"),
            "chk_errs": got[23],
            "rlen": int.from_bytes(got[24:26], "little"),
            "cur": int.from_bytes(got[26:28], "little"),
        }
        ok = (r["sync"] == 0x5A and r["seq"] == self.seq and r["rlen"] == rlen
              and got[HDR:HDR + rlen] == bytes(i & 0xFF for i in range(rlen))
              and got[-1] == (sum(got[1:-1]) & 0xFF) and (r["status"] & 1))
        r["ok"] = ok
        return r, t1, t4


def cmd_sizes(link, args, out):
    pairs = [(0, 0), (64, 0), (128, 0), (256, 0), (448, 0), (0, 64), (0, 128), (0, 256), (0, 448),
             (64, 64), (128, 128), (256, 256), (448, 448)]
    out.write("payload_B,reply_filler_B,n,lost,bad,rtt_min_us,rtt_p50_us,rtt_p99_us,rtt_max_us,goodput_B_per_s\n")
    for n_req, n_rep in pairs:
        rtts, lost, bad = [], 0, 0
        payload = bytes((i * 7) & 0xFF for i in range(n_req))
        t_start = now_ns()
        for _ in range(args.n):
            r, t1, t4 = link.exchange(2, payload, n_rep)
            if r is None:
                lost += 1
            elif not r["ok"]:
                bad += 1
            else:
                rtts.append((t4 - t1) / 1000.0)
        dt = (now_ns() - t_start) / 1e9
        goodput = (n_req + n_rep) * len(rtts) / dt
        line = (f"{n_req},{n_rep},{args.n},{lost},{bad},{pct(rtts, 0.0001):.0f},{pct(rtts, 50):.0f},"
                f"{pct(rtts, 99):.0f},{max(rtts) if rtts else float('nan'):.0f},{goodput:.0f}")
        out.write(line + "\n")
        out.flush()
        print(line, flush=True)
        if lost or bad:
            print("stopping at the first failure", flush=True)
            break


def cmd_sync(link, args, out):
    out.write("t1_ns,t4_ns,t_rx_tick,t_tx_tick\n")
    rows = []
    period = args.period_ms / 1000.0
    next_t = time.monotonic()
    lost = 0
    end = time.monotonic() + args.seconds
    while time.monotonic() < end:
        r, t1, t4 = link.exchange(2)
        if r is None or not r["ok"]:
            lost += 1
        else:
            rows.append((t1, t4, r["t_rx"], r["t_tx"]))
            out.write(f"{t1},{t4},{r['t_rx']},{r['t_tx']}\n")
        next_t += period
        delay = next_t - time.monotonic()
        if delay > 0:
            time.sleep(delay)
    # NTP-style samples (bridge ticks at the nominal 27 MHz)
    s = []
    for t1, t4, a, b in rows:
        t2 = a * 1e9 / F_TICK
        t3 = b * 1e9 / F_TICK
        off = ((t2 - t1) + (t3 - t4)) / 2.0
        dly = (t4 - t1) - (t3 - t2)
        s.append((t1, off, dly))
    dl = [d / 1000.0 for _, _, d in s]
    # drift: least squares through the lowest-delay 10 % of each 2 s window
    best = []
    w0 = s[0][0]
    win = {}
    for t1, off, dly in s:
        win.setdefault(int((t1 - w0) // 2e9), []).append((dly, t1, off))
    for k in sorted(win):
        pts = sorted(win[k])[:max(1, len(win[k]) // 10)]
        best.extend((t1, off) for _, t1, off in pts)
    n = len(best)
    mx = sum(t for t, _ in best) / n
    my = sum(o for _, o in best) / n
    sxx = sum((t - mx) ** 2 for t, _ in best)
    slope = sum((t - mx) * (o - my) for t, o in best) / sxx if sxx else 0.0
    resid = [abs((o - my) - slope * (t - mx)) / 1000.0 for t, o in best]
    resid_all = [abs((o - my) - slope * (t - mx)) / 1000.0 for t, o, _ in s]
    rep = [
        f"exchanges {len(rows)} (lost or bad {lost}) over {args.seconds} s, one every {args.period_ms} ms",
        f"delay (T4-T1)-(T3-T2) us: min {min(dl):.0f} p50 {pct(dl, 50):.0f} p99 {pct(dl, 99):.0f} max {max(dl):.0f}",
        f"error bound of one sample (delay/2) us: best {min(dl) / 2:.0f}, p50 {pct(dl, 50) / 2:.0f}",
        f"bridge crystal against host CLOCK_MONOTONIC: {slope * 1e6:+.1f} ppm (lowest-delay 10 % per 2 s window, {n} points)",
        f"offset residual after the drift line, us: lowest-delay points p50 {pct(resid, 50):.0f} p99 {pct(resid, 99):.0f}; all points p50 {pct(resid_all, 50):.0f} p99 {pct(resid_all, 99):.0f}",
    ]
    return rep


def cmd_hold(link, args, out):
    out.write("t1_ns,step,fill,under,cur,sent_total\n")
    H = args.horizon
    nxt = 0               # next setpoint value (a 16-bit counter)
    sent = 0
    first = None          # (step, under) at the first reply after the start
    prev = None
    lost = mismatches = 0
    fills = []
    max_batch = args.max_batch
    end = time.monotonic() + args.seconds
    r, _, _ = link.exchange(2)  # read the state before the start
    base_under = r["under"] if r else 0
    fill = 0
    while time.monotonic() < end:
        k = max(0, min(max_batch, H - fill + args.lead))
        payload = b"".join(struct.pack("<H", (nxt + i) & 0xFFFF) for i in range(k))
        r, t1, t4 = link.exchange(1 if k else 2, payload)
        if r is None or not r["ok"]:
            lost += 1
            continue
        nxt += k
        sent += k
        fill = r["fill"]
        fills.append(fill)
        out.write(f"{t1},{r['step']},{fill},{r['under']},{r['cur']},{sent}\n")
        if first is None:
            first = (r["step"], r["under"])
        elif prev is not None:
            # setpoints consumed since the previous reply = steps - new underruns
            consumed = (r["step"] - prev["step"]) - (r["under"] - prev["under"])
            if ((prev["cur"] + consumed) & 0xFFFF) != r["cur"]:
                mismatches += 1
        prev = r
        if args.gap_ms:
            time.sleep(args.gap_ms / 1000.0)
    steps = prev["step"] - first[0] if prev and first else 0
    under = prev["under"] - base_under if prev else 0
    return [
        f"horizon H {H} steps (1 step = 1 ms), lead {args.lead}, at most {max_batch} setpoints ({2 * max_batch} B) per request, gap {args.gap_ms} ms",
        f"duration {args.seconds} s, steps {steps}, setpoints sent {sent}, exchanges lost or bad {lost}",
        f"underruns {under} ({under / steps * 100 if steps else 0:.3f} % of steps)",
        f"FIFO fill at replies: min {min(fills) if fills else 0} p1 {pct(fills, 1)} p50 {pct(fills, 50)} max {max(fills) if fills else 0}",
        f"applied setpoint mismatches {mismatches} (each reply's cur_sp against the counter sent)",
    ]


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("mode", choices=["sizes", "sync", "hold"])
    ap.add_argument("--dev", default="/dev/ttyUSB1")
    ap.add_argument("--baud", type=int, default=3_000_000)
    ap.add_argument("--n", type=int, default=200, help="sizes: exchanges per size")
    ap.add_argument("--seconds", type=float, default=30.0)
    ap.add_argument("--period-ms", type=float, default=10.0, help="sync: one exchange every PERIOD ms")
    ap.add_argument("--horizon", type=int, default=16, help="hold: target FIFO fill, steps")
    ap.add_argument("--lead", type=int, default=6, help="hold: extra setpoints for the steps consumed during one round trip")
    ap.add_argument("--max-batch", type=int, default=30, help="hold: setpoints per request (2 B each)")
    ap.add_argument("--gap-ms", type=float, default=0.0, help="hold: pause between exchanges")
    ap.add_argument("--csv", required=True)
    ap.add_argument("--report", required=True)
    args = ap.parse_args()
    link = Link(args.dev, args.baud)
    with open(args.csv, "w") as out:
        if args.mode == "sizes":
            cmd_sizes(link, args, out)
            rep = ["see the CSV"]
        elif args.mode == "sync":
            rep = cmd_sync(link, args, out)
        else:
            rep = cmd_hold(link, args, out)
    head = f"pingpong_bench {args.mode} on {args.dev} at {args.baud} baud, {time.strftime('%Y-%m-%d %H:%M:%S')}"
    text = "\n".join([head] + rep) + "\n"
    with open(args.report, "w") as f:
        f.write(text)
    print(text, end="")


if __name__ == "__main__":
    sys.exit(main())
