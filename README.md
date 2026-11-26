# open-tick

> The beat a simulated machine keeps. One clock, one step, any machine behind your microcontroller's pins, and an exact statement of how faithful it is.

Part of [Open Heavy Machinery](https://open-heavy-machinery.github.io) ([open-tick's page](https://open-heavy-machinery.github.io/open-tick/)): open tools to practise and develop embedded control for machines you cannot keep at home, starting with heavy machinery.

**Status: pre-alpha. v0.3 (transport faults) and v0.3.1 (frame logs) are done.** The machine-independent parts of [open-crank](https://github.com/Open-Heavy-Machinery/open-crank) live here as a library, and open-crank runs on it as a git submodule, with traces byte-identical to before the move (v0.1). Since v0.2 the acceptance oracle lives here too: open-crank's acceptance suite grades on it with verdicts identical to before the move, and so does the flywheel example ([docs/open-crank-integration.md](docs/open-crank-integration.md)). Since v0.3, links put transport faults (lost, delayed, jittered or corrupted frames, bus-off) between a sender and a receiver, driven by the scenario ([docs/link-faults.md](docs/link-faults.md)); open-crank's fault scenarios run on them. Anything marked *(planned)* does not exist yet. The decisions are in [VISION.md](VISION.md), the code in [ARCHITECTURE.md](ARCHITECTURE.md), the plan in [ROADMAP.md](ROADMAP.md).

## What it is

To a microcontroller, the world is voltages on pins, edges with timing and frames on a bus. open-tick is the machine-independent core that produces that world from a simulation:

- **One clock.** Simulated time is owned by whoever runs the machine, never read from a clock. It is paced by the CPU (tests, CI), the wall clock (SIL), *(planned)* an emulator (Renode) or a bridge's oscillator (real silicon).
- **Scenarios and faults as data.** Signal names come from the application, and a typo fails loudly with its line number.
- **Transports.** CAN and CAN FD today; *(planned)* the signal-level transports open-crank's Level 2 needs (tooth wheels, pulse capture, analog outputs); UART, quadrature encoders, GPIO and I²C/SPI slaves when a sibling application needs them.
- **Bridges.** An in-memory bridge and Linux SocketCAN (`vcan` or a USB-CAN adapter) today; *(planned)* a Tang Nano 20K on the pins for open-crank's Level 2, and a Raspberry Pi Pico when open-robots starts.
- **An oracle** that grades a device under test only by the frames it sends, against criteria written as data, with reaction times measured from the injected event. The application says what its frames mean; the criteria format is documented in [docs/criteria-format.md](docs/criteria-format.md). Its verdicts can also be written as a file for programs and viewers ([docs/verdict-format.md](docs/verdict-format.md)), which [open-rig](https://github.com/Open-Heavy-Machinery/open-rig) draws over the trace.
- **Traces** with columns the application registers, which refuse to shift a column silently. The file's shape is documented in [docs/trace-format.md](docs/trace-format.md).
- **Frame logs** (v0.3.1): every CAN frame of a run, with its logical time (the step, which places it on the trace's axis), its bus time (measured, with the clock named in the file) and what the links did to it ([docs/frame-log-format.md](docs/frame-log-format.md)).
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

`otk-flywheel` is the smallest machine that uses every module: a DC motor spins a flywheel, and a toy speed controller holds the speed over CAN, through the in-memory bridge. The scenario then cuts the speed frames. The controller must drop to its safe state and the flywheel coasts down. With `--criteria`, the oracle grades the controller by its duty frames alone and prints a pass/fail report.

```sh
./build/otk-flywheel examples/flywheel/step.csv
./build/otk-flywheel examples/flywheel/step.csv --trace flywheel.csv
./build/otk-flywheel examples/flywheel/step.csv --driver paced    # the same run, in wall-clock time
./build/otk-flywheel examples/flywheel/step.csv --criteria examples/flywheel/step.accept.csv
./build/otk-flywheel examples/flywheel/delay.csv --criteria examples/flywheel/delay.accept.csv  # SPEED delayed 300 ms (v0.3)
./build/otk-flywheel examples/flywheel/delay.csv --frames delay.frames.csv                      # every frame, two times (v0.3.1)
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
└── docs/        trace, criteria, verdict and frame log formats; link faults; open-crank integration
```

## Checks

- `tools/check_deps.sh`: `core/` uses ISO C only, and nothing includes an application.
- `tools/mutation_check.sh`: breaks 38 behaviours on purpose and requires the tests to notice each one.

CI runs both, plus gcc and clang in Debug and Release with warnings as errors, a sanitizer build, a core-only build on macOS, and the SocketCAN bridge on `vcan` when the runner allows it.

## What it is not

Not a physics engine, not an MCU emulator, not power HIL, not a certified tool. The full list is in [VISION.md](VISION.md), §7. Never connect any of this to real machinery.

## License

Apache-2.0, like the rest of the family.
