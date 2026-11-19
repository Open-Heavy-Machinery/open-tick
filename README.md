# open-tick

> The beat a simulated machine keeps. One clock, one step, any machine behind your microcontroller's pins, and an exact statement of how faithful it is.

Part of [Open Heavy Machinery](https://open-heavy-machinery.github.io): open tools to practise and develop embedded control for machines you do not have.

**Status: vision only. There is no code yet.** The decisions taken so far are in [VISION.md](VISION.md); the plan is in [ROADMAP.md](ROADMAP.md).

## What it is

To a microcontroller, the world is voltages on pins, edges with timing and frames on a bus. open-tick is the machine-independent core that produces that world from a simulation:

- **One clock.** Simulated time owned by the core, paced by the CPU (tests, CI), the wall clock (SIL), an emulator (Renode) or the bridge's oscillator (real silicon).
- **A step contract** that plants from any application plug into.
- **Scenarios and faults as data,** and an **oracle** that grades the device under test only by what it does on its transports.
- **Transports:** message transports such as CAN and UART, and signal-level transports such as quadrature encoders, tooth wheels, PWM capture, GPIO and I²C/SPI slaves.
- **Bridges** to a real microcontroller, from a USB adapter to a Raspberry Pi Pico or a Tang Nano 20K.
- **A fidelity contract:** each machine says what it needs, each bridge says what it guarantees, and a run that cannot be faithful is refused instead of passing quietly.

## The family

| Project | What it brings |
|---|---|
| [open-crank](https://github.com/Open-Heavy-Machinery/open-crank) | heavy-duty diesel (and later electrical) plants, a simulated engine ECU, J1939, MISRA-oriented reference firmware |
| open-robots *(planned)* | generic DC motors, encoders and IMUs for robots, with no J1939 |

Applications depend on open-tick, never the other way round.

## What it is not

Not a physics engine, not an MCU emulator, not power HIL, not a certified tool. The full list is in [VISION.md §7](VISION.md#7-non-goals). Never connect any of this to real machinery.

## License

Apache-2.0, like the rest of the family.
