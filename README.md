# open-tick

> The beat a simulated machine keeps. One clock, one step, any machine behind your microcontroller's pins, and an exact statement of how faithful it is.

Part of [Open Heavy Machinery](https://open-heavy-machinery.github.io): open tools to practise and develop embedded control for machines you do not have.

**Status: pre-alpha, v0.1 (extraction) in progress.** The machine-independent parts of [open-crank](https://github.com/Open-Heavy-Machinery/open-crank) now exist here as a library, with tests. A trial port of open-crank on top of it produces byte-identical traces ([docs/open-crank-integration.md](docs/open-crank-integration.md)). Anything marked *(planned)* does not exist yet. The decisions are in [VISION.md](VISION.md), the code in [ARCHITECTURE.md](ARCHITECTURE.md), the plan in [ROADMAP.md](ROADMAP.md).

## What it is

To a microcontroller, the world is voltages on pins, edges with timing and frames on a bus. open-tick is the machine-independent core that produces that world from a simulation:

- **One clock.** Simulated time is owned by whoever runs the machine, never read from a clock. It is paced by the CPU (tests, CI), the wall clock (SIL), *(planned)* an emulator (Renode) or a bridge's oscillator (real silicon).
- **Scenarios and faults as data.** Signal names come from the application, and a typo fails loudly with its line number.
- **Transports.** CAN and CAN FD today; *(planned)* UART, then signal-level transports such as quadrature encoders, tooth wheels, PWM capture, GPIO and I²C/SPI slaves.
- **Bridges.** An in-memory bridge and Linux SocketCAN (`vcan` or a USB-CAN adapter) today; *(planned)* a Raspberry Pi Pico and a Tang Nano 20K on the pins.
- **Traces** with columns the application registers, which refuse to shift a column silently.
- *(planned)* **A fidelity contract.** Each machine says what it needs, each bridge says what it guarantees, and a run that cannot be faithful is refused instead of passing quietly.

## The family

| Project | What it brings |
|---|---|
| [open-crank](https://github.com/Open-Heavy-Machinery/open-crank) | heavy-duty diesel (and later electrical) plants, a simulated engine ECU, J1939, MISRA-oriented reference firmware |
| open-robots *(planned)* | generic DC motors, encoders and IMUs for robots, with no J1939 |

Applications depend on open-tick, never the other way round, and CI checks it.

## Building

Any host with a C11 compiler and CMake 3.20 or newer builds the core. The host zone (pacer, drivers, SocketCAN) needs Linux.

```sh
cmake -S . -B build
cmake --build build
ctest --test-dir build --output-on-failure
```

`test_socketcan` is reported as skipped unless a `vcan0` interface exists (see the comment at the top of [tests/test_socketcan.c](tests/test_socketcan.c)).

## Run the example

`otk-flywheel` is the smallest machine that uses every module: a DC motor spins a flywheel, and a toy speed controller holds the speed over CAN, through the in-memory bridge. The scenario then cuts the speed frames. The controller must drop to its safe state and the flywheel coasts down.

```sh
./build/otk-flywheel examples/flywheel/step.csv
./build/otk-flywheel examples/flywheel/step.csv --trace flywheel.csv
./build/otk-flywheel examples/flywheel/step.csv --driver paced    # the same run, in wall-clock time
```

## Using it from an application

```cmake
add_subdirectory(path/to/open-tick ${CMAKE_BINARY_DIR}/open-tick)   # or FetchContent
target_link_libraries(my_sim PUBLIC opentick::core)
target_link_libraries(my_sil PRIVATE opentick::host)                # Linux
```

As a subproject, open-tick builds only its libraries. See [ARCHITECTURE.md](ARCHITECTURE.md), §4.

## Repository layout

```text
open-tick/
├── README.md  VISION.md  ARCHITECTURE.md  ROADMAP.md  CHANGELOG.md  LICENSE
├── core/        ISO C11 only: timebase, signals, scenario, player, CAN, trace
├── host/        Linux: pacer, drivers, SocketCAN bridge
├── examples/    otk-flywheel
├── tests/       unit tests and their data
├── tools/       dependency and mutation checks
└── docs/        open-crank integration plan and trial run
```

## Checks

- `tools/check_deps.sh`: `core/` uses ISO C only, and nothing includes an application.
- `tools/mutation_check.sh`: breaks 18 behaviours on purpose and requires the tests to notice each one.

CI runs both, plus gcc and clang in Debug and Release with warnings as errors, a sanitizer build, a core-only build on macOS, and the SocketCAN bridge on `vcan` when the runner allows it.

## What it is not

Not a physics engine, not an MCU emulator, not power HIL, not a certified tool. The full list is in [VISION.md](VISION.md), §7. Never connect any of this to real machinery.

## License

Apache-2.0, like the rest of the family.
