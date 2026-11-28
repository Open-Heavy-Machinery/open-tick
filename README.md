# open-tick

> The beat a simulated machine keeps. One clock, one step, any machine behind your microcontroller's pins, and an exact statement of how faithful it is.

Part of [Open Heavy Machinery](https://open-heavy-machinery.github.io) ([open-tick's page](https://open-heavy-machinery.github.io/open-tick/)): open tools to practise and develop embedded control for machines you cannot keep at home, starting with heavy machinery.

**Status: pre-alpha. v0.4.0 is done: the Tang Nano 20K bridge. A simulation can now drive a microcontroller's pins with microsecond timing (a 60−2 crank wheel and a cam from the plant's angle) and capture what it sends back (injector pulses), with the bridge's oscillator pacing the run, a budget of every run's timing, and a fidelity contract that refuses a run the bridge cannot hold. [open-crank](https://github.com/Open-Heavy-Machinery/open-crank) v0.4.0 runs its Level 2 on it: an engine ECU on a NUCLEO-G431KB, in a closed loop with the simulated engine. Before that: v0.3 (transport faults) and v0.3.1 (frame logs).** The machine-independent parts of [open-crank](https://github.com/Open-Heavy-Machinery/open-crank) live here as a library, and open-crank runs on it as a git submodule, with traces byte-identical to before the move (v0.1). Since v0.2 the acceptance oracle lives here too: open-crank's acceptance suite grades on it with verdicts identical to before the move, and so does the flywheel example ([docs/open-crank-integration.md](docs/open-crank-integration.md)). Since v0.3, links put transport faults (lost, delayed, jittered or corrupted frames, bus-off) between a sender and a receiver, driven by the scenario ([docs/link-faults.md](docs/link-faults.md)); open-crank's fault scenarios run on them. Anything marked *(planned)* does not exist yet. The decisions are in [VISION.md](VISION.md), the code in [ARCHITECTURE.md](ARCHITECTURE.md), the plan in [ROADMAP.md](ROADMAP.md).

## What it is

To a microcontroller, the world is voltages on pins, edges with timing and frames on a bus. open-tick is the machine-independent core that produces that world from a simulation:

- **One clock.** Simulated time is owned by whoever runs the machine, never read from a clock. It is paced by the CPU (tests, CI), the wall clock (SIL), *(planned)* an emulator (Renode) or a bridge's oscillator (real silicon).
- **Scenarios and faults as data.** Signal names come from the application, and a typo fails loudly with its line number.
- **Transports.** CAN and CAN FD; the signal-level transports open-crank's Level 2 needs (tooth wheels, pulse capture, analog outputs) on the Tang Nano 20K (v0.4.0; the analog outputs are not yet measured on a board); UART, quadrature encoders, GPIO and I²C/SPI slaves when a sibling application needs them.
- **Bridges.** An in-memory bridge, Linux SocketCAN (`vcan` or a USB-CAN adapter), and a Tang Nano 20K on the pins with the hard driver, where the bridge's oscillator paces the run (v0.4.0); *(planned)* a Raspberry Pi Pico when open-robots starts.
- **An oracle** that grades a device under test only by the frames it sends, against criteria written as data, with reaction times measured from the injected event. The application says what its frames mean; the criteria format is documented in [docs/criteria-format.md](docs/criteria-format.md). Its verdicts can also be written as a file for programs and viewers ([docs/verdict-format.md](docs/verdict-format.md)), which [open-rig](https://github.com/Open-Heavy-Machinery/open-rig) draws over the trace.
- **Traces** with columns the application registers, which refuse to shift a column silently. The file's shape is documented in [docs/trace-format.md](docs/trace-format.md).
- **Frame logs** (v0.3.1): every CAN frame of a run, with its logical time (the step, which places it on the trace's axis), its bus time (measured, with the clock named in the file) and what the links did to it ([docs/frame-log-format.md](docs/frame-log-format.md)).
- **A fidelity contract** (v0.4.0). Each machine says what it needs, each bridge says what it guarantees, and a run that cannot be faithful is refused instead of passing quietly ([docs/bridge-protocol.md §7](docs/bridge-protocol.md#7-the-fidelity-contract-version-1)).

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
./build/otk-flywheel examples/flywheel/jitter.csv --seed 2 --frames jitter.frames.csv         # SPEED jittered 0 to 30 ms; the seed picks the draws
```

### On the pins, with a Tang Nano 20K (v0.4)

The **bridge** is a Sipeed Tang Nano 20K (an FPGA board, about 25 USD) running this repository's gateware. Over its USB serial link the host sends it, one step ahead at a time, what each output should do (a tooth wheel's speed, analog levels); the bridge plays them on its pins against its own 27 MHz oscillator, captures the pulses the device under test sends back, and reports them with their exact ticks. The **hard driver** (`host/include/opentick/host/hard_loop.h`) keeps the plant 16 steps ahead of the bridge and steps it on the bridge's clock, not the PC's. Every run writes its **budget**: underruns, round trip, clock mapping, CAN timing, the delay from the device under test back to the plant ([docs/bridge-protocol.md](docs/bridge-protocol.md)).

```sh
eval "$(tools/fpga_toolchain.sh)"            # yosys, nextpnr and openFPGALoader, pinned, into ~/.cache (no sudo)
gateware/tang_nano_20k/build.sh              # the bitstream, into build-gateware/
openFPGALoader -b tangnano20k build-gateware/bridge.fs    # into the Tang's SRAM (freshly programmed: §8)
./build/otk-flywheel examples/flywheel/hard60.csv --driver hard --trace hard.csv \
    --dev /dev/serial/by-id/usb-SIPEED_USB_Debugger_<serial>-if01-port0
```

The flywheel then steps on the bridge's clock, its crank pattern turning at the flywheel's speed on pin 73, and the run writes `hard.budget.csv`, its budget declared and measured ([§9](docs/bridge-protocol.md#9-the-budget-of-a-hard-run)). With `--fidelity examples/flywheel/fidelity.csv --bridge-fidelity gateware/tang_nano_20k/fidelity.csv` the run is checked first against what the bridge guarantees, and refused (exit 5) if it asks for more. A whole machine on it: [open-crank's Level 2](https://github.com/Open-Heavy-Machinery/open-crank#level-2-your-firmware-as-the-engine-ecu), an engine ECU on a real board against the simulated engine.

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
- `tools/mutation_check.sh`: breaks 47 behaviours on purpose and requires the tests to notice each one. Eight of them are in the SocketCAN bridge's timestamps and own frames, and need a CAN interface (`vcan0`, or a private one with `unshare -rn`; see the script's header); CI gives them one.

CI runs both, plus gcc and clang in Debug and Release with warnings as errors, a sanitizer build, a core-only build on macOS, and the SocketCAN bridge on `vcan` when the runner allows it.

## What it is not

Not a physics engine, not an MCU emulator, not power HIL, not a certified tool. The full list is in [VISION.md](VISION.md), §7. Never connect any of this to real machinery.

## License

Apache-2.0, like the rest of the family.
