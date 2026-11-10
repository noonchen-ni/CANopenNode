# Standalone CANopen slave smoke-test app

`xnet_canopen_device` is a C++17 executable that loads the integration shared
library at runtime. It does not link to NI-XNET or the integration library at
build time. The loaded library owns hardware sessions and protocol processing.
The deployment target is NI Linux RT AMD64 (x86-64); Windows builds support host
development and mock testing.

## Build

The integration's normal CMake build includes the app by default. Disable it
with `-DXNET_CANOPEN_BUILD_APP=OFF`. The target is `xnet_canopen_device`.

To build only the app, without building the stack or finding NI-XNET, run from
the integration directory:

```sh
cmake -S tests/app -B build-app -DCMAKE_BUILD_TYPE=Release
cmake --build build-app --config Release
```

For NI Linux RT AMD64 cross-compilation, also supply the target CMake toolchain.
The app and loaded library must match the target architecture and runtime ABI;
a desktop Linux x86-64 build is not automatically compatible with NI Linux RT.
The integration
library's NI-XNET and C++ runtime dependencies must be available to the system
dynamic loader. Build and deploy the app and library from matching sources:
there is no ABI-version query, and the app requires the current **48-byte,
twelve-uint32 status structure**, not the former 32- or 40-byte structures.

## Run

Linux, with an absolute deployed library path:

```sh
./build-app/xnet_canopen_device \
  --lib /absolute/path/libxnet_canopen.so \
  --config tests/app/device.config.json
```

Windows, using a matching DLL:

```powershell
.\build-app\Release\xnet_canopen_device.exe `
  --lib C:\absolute\path\xnet_canopen.dll `
  --config .\tests\app\device.config.json
```

Add `--duration 30` to stop after 30 seconds of monitoring. Otherwise the app
runs until Ctrl+C / SIGINT or SIGTERM. `--help` prints usage. Duration must be
a positive integer number of seconds.

## Runner configuration

The separate runner JSON contains exactly these required string fields:

```json
{
  "interface_name": "CAN1",
  "model_path": "../../examples/device.json",
  "persistence_path": "./device.lss.json"
}
```

This example is included as [device.config.json](device.config.json). The
referenced [model](../../examples/device.json) provides node ID, bitrate,
identity, heartbeat, worker period, OD entries and PDO configuration using the
library's existing schema. There is no second CANopen model schema in the app.

- CLI paths are resolved relative to the working directory.
- Model and persistence paths are resolved relative to the runner JSON's directory.
- The model must exist. The persistence parent directory must already exist and
  must be writable for LSS storage; the persistence file need not exist.
- The app never deletes persistence. Saved LSS settings may override model startup
  settings. Use a different persistence path when a fresh configuration is needed.
- Ensure exclusive access to the interface and persistence file.
- Native logging has a compile-time path; it cannot be changed in runner JSON.
  Initialization fails if logging cannot start. See the integration's
  [logging documentation](../../README.md#file-logging).

## Output and lifecycle

The app prints one UTC-timestamped status line to stdout immediately after start
and then every 500 ms, subject to OS scheduling and native call latency. All twelve
fields are included: runtime state, NMT state, active/pending node IDs,
active/pending bitrates, hexadecimal CAN errors, guarding timeout and both reset
counters, plus the raw NI-XNET CAN controller state and its validity indicator.
Runtime/NMT states include symbolic names and numeric values.
Startup details, sampled state changes, reset-counter deltas and errors go to
stderr. Valid controller samples are decoded on the first observation and on
sampled raw-state changes: communication state, TX/RX counters, last bus error,
transceiver error and sleep indication. A nonzero CAN-error flag transition also
prints the retained native diagnostic. Redirect stdout to save the status stream.

The startup summary shows resolved paths; the first status snapshot shows the
effective node ID and bitrate, including persisted settings. Latest native
diagnostics are retained by the library, so a displayed diagnostic is not
necessarily a new event. Polling is not a lossless transition/event recorder.

Signals only set a shutdown flag. Normal execution cancels polling, stops,
uninitializes and destroys the device, then unloads the library. Every cleanup
result is checked. Initialization failures also attempt cleanup. A failed
destroy retains the library until process exit rather than unloading potentially
live worker code. Native calls are synchronous; shutdown waits for any ongoing
call and library teardown rather than promising a fixed shutdown deadline.

Clean signal/timed shutdown returns zero. Invalid arguments/configuration,
loader/API failures, unexpected non-running runtime status and cleanup failures
return nonzero. Runtime failures do not trigger automatic restart. CAN error
flags and guarding conditions are reported without independently forcing exit.

This is a **passive application-layer slave**, not a CANopen master or drive model:

- The library still produces configured protocol traffic such as boot-up and heartbeat.
- A configured node starts Pre-operational; an external master controls NMT.
- Node ID 255 may legitimately wait for LSS configuration.
- The app does not write OD values, request TPDOs, inject faults, or implement CiA 402.

## Mock validation

With `XNET_CANOPEN_BUILD_TESTS=ON`, `XNET_CANOPEN_BUILD_LIBRARY=OFF` and the app
enabled, CMake builds a real-stack shared library backed by fake XNET and adds:

- `xnet_canopen_app_smoke`: status fields/cadence, config-relative paths,
  timed shutdown, bad config/library/model and invalid duration checks.
- `xnet_canopen_app_sigint`: sends Ctrl+C/SIGINT to an isolated child process and
  verifies successful native cleanup.
- `xnet_canopen_app_sigterm`: equivalent SIGTERM check on Unix.

These are hardware-independent checks, not NI-XNET hardware certification.
