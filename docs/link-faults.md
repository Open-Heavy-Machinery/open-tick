# Link faults

A link is one direction of a CAN path between a sender and a receiver, for example the machine's frames on their way to the device under test (DUT). It can lose, delay, reorder and corrupt frames, and it can go bus-off. A scenario turns these faults on and off like any other signal, so a fault is data, and the acceptance oracle can time the DUT's reaction from the event without new syntax.

The API is in [`core/include/opentick/link.h`](../core/include/opentick/link.h). The links are in `core/`, so they work wherever a frame passes through the application's process: in memory, as in `otk-flywheel`, or between a SocketCAN socket and the bench in a simulator's process.

## Using a link

The sender pushes every frame it sends at simulated time `t` with `otk_link_push(link, frame, t)`. The receiver pops the frames whose delivery time has come with `otk_link_pop(link, now, &frame)`, in delivery order. With no fault active, a link is transparent: a frame pushed at `t` is popped at `t`, and frames come out in the order they went in. `otk-flywheel`'s `step.csv` gives the same trace, report and verdicts as before it had a link, byte for byte.

Memory is fixed, with no `malloc`. A link holds up to `OTK_CAN_QUEUE_MAX` (64) frames in flight. A frame pushed into a full link is lost and counted in `overflows`. An application should treat an overflow as a broken run, as `otk-flywheel` does: it exits with status 1. A frame is in flight while it is delayed, so the delay a link can hold depends on the traffic. 300 ms on a 10 ms frame needs about 30 places.

## The faults

Each fault except `bus_off` has its own filter, set by the application with `otk_link_set_filter()`. A filter has an identifier, a mask and a format (11-bit or 29-bit). A frame matches when its format is the same and its identifier equals the filter's identifier on the bits of the mask. Without a filter, a fault applies to every frame. The values come from the scenario; the filters are set in code.

| Fault | Scenario signal | Value | What happens to a matching frame |
|---|---|---|---|
| drop | `fault.link.<name>.drop` | 0 or 1 | lost |
| drop one in N | `fault.link.<name>.drop_one_in` | N, 0 for off | the Nth, 2Nth, … matching frame is lost. The count starts again when N changes |
| delay | `fault.link.<name>.delay_ms` | ms | delivered that much later |
| jitter | `fault.link.<name>.jitter_ms` | ms | delivered later by a random time between 0 and the value |
| corrupt | `fault.link.<name>.corrupt` | N, 0 for off | byte N (1-based) is XORed with `0xFF`. A frame shorter than N bytes passes unchanged |
| bus-off | `fault.link.<name>.bus_off` | 0 or 1 | nothing passes in this direction while it is on, matching or not |

The application names the link and puts its six signals in its signal table with `OTK_LINK_SIGNAL_NAMES("<name>")`. In its apply function, `otk_link_signal()` stores a value, and the application passes the values to the link with `otk_link_set_faults()` each step. A negative value is off, and N rounds to the nearest whole number. An application that registers no links behaves as before.

Faults apply when the frame is pushed, in this order: bus-off, drop, drop one in N, then the line is checked for room, then corrupt, delay and jitter. A frame already in flight keeps its delivery time when a fault changes.

### Order

A frame is never delivered before an earlier frame with the same identifier and format that is still in the link. Its delivery time is the later of two: its own (`t + delay + jitter`), and the delivery time of the latest frame with the same identifier still in flight. Frames with different identifiers may overtake each other.

**Why.** On a real bus, one sender's frames with one identifier come out of its transmit queue in order, while frames with different identifiers can be reordered by arbitration. The same rule also settles what happens when a delay drops while frames are in flight. The frames sent after the change wait for the earlier ones, and then they all arrive together, as a queue would flush them. Frames with the same delivery time come out in the order they were pushed.

### Jitter is deterministic

The jitter comes from the link's own PRNG (splitmix64), seeded by the application in `otk_link_init()`. It draws only for frames that match the jitter filter while jitter is on. The same scenario with the same seed therefore gives the same trace, byte for byte, in free-run. `otk-flywheel` uses seed 1.

### Corrupt: the CRC passed, the content is wrong

A frame whose CAN CRC is wrong never reaches any receiver on a real bus: every node rejects it and the sender retransmits it. To the receiver it is a lost or a late frame, so that case is `drop` or `delay`. `corrupt` simulates what can get through: a frame that is valid on the bus but whose content is wrong. It could come from a faulty sender, a gateway or memory corruption. This is what an application-level check catches, such as a J1939 message checksum (TSC1, SPN 4207).

The scenario chooses the byte because it matters which one is flipped. In TSC1 the last byte holds both the counter (SPN 4206) and the checksum. Flipping it would also be caught by a receiver that checks only the counter, so a test of the checksum must flip a data byte.

### Bus-off is simulated

While `bus_off` is on, every frame pushed is lost, and so is every frame in flight whose delivery time comes while it is on. `otk_link_bus_off()` reports the state. This is the effect of a bus-off on the other side of the link: nothing arrives. It is not a real bus-off of a CAN controller. A real one needs error frames, which the links cannot produce and which slcan adapters cannot produce either.

## A constant delay cannot be seen from arrival times

[`examples/flywheel/delay.csv`](../examples/flywheel/delay.csv) delays the SPEED frames (`0x100`) by 300 ms from t = 3 s to t = 4 s. The toy controller judges its speed feedback stale when no SPEED frame has *arrived* for more than 100 ms.

- The first delayed frame arrives 300 ms after the event, so for 300 ms nothing arrives. The controller drops to its safe state 110 ms after the event, and [`delay.accept.csv`](../examples/flywheel/delay.accept.csv) asks for no drive from the event + 150 ms to the event + 280 ms (G1).
- After that, frames arrive every 10 ms again, only 300 ms old. The controller drives again on stale speed, and the loop overshoots. No timeout on arrival can see that. Only something inside the frame can: a counter, which the receiver compares with the frames it expected, or a timestamp.

So the criteria ask only for what any controller must do: no drive inside the gap. A better controller that detected the stale frames would also pass them.

## What links do not simulate

- **A real bus-off, error frames and error counters.** These need a CAN controller on a real or emulated bus.
- **Arbitration and bus load.** A link is one direction between one sender and one receiver, not a bus. It does not know the bit rate, and frames do not delay each other except by the order rule above.
- **A corrupted CAN CRC.** As explained above, on a real bus that frame is lost.
- **Timing finer than the application's step.** A frame due between two steps is popped at the next step, as the player applies events.
