# NI-XNET CANopen simulator: first edition

This is a C++17 shared-library wrapper around the existing CANopenNode C stack,
intended for NI Linux RT ARM64 and a VeriStand Custom Device. All integration
sources are in this directory. No existing CANopenNode source files are changed.

Each handle owns one simulated device, one physical XNET interface, two raw
frame-stream sessions, one runtime Object Dictionary, and one processing worker.
The library owns all frame I/O. A CAN database is not needed by XNET.

## Files

- [include/xnet_canopen.h](include/xnet_canopen.h): public C ABI.
- [src/model.cpp](src/model.cpp): version-1 JSON validation and OD construction.
- [src/driver.cpp](src/driver.cpp): XNET sessions and CANopen driver boundary.
- [src/device.cpp](src/device.cpp): lifecycle, worker, LSS, channels, diagnostics.
- [src/persistence.cpp](src/persistence.cpp): local JSON storage with atomic replacement.
- [src/logging.cpp](src/logging.cpp): private spdlog file backend and per-device tags.
- [src/logging_config.h](src/logging_config.h): compile-time fixed log path.
- [examples/device.json](examples/device.json): complete working model.
- [Kinco_simulation_assessment.md](Kinco_simulation_assessment.md): scoped device-simulation gaps and external-model data contract.
- [tests/](tests/): fake XNET API, real stack protocol tests, and C header/ABI checks.

The supplied NI header, JSON single header, and vendored spdlog headers are used
unchanged. spdlog and its bundled fmt are compiled header-only; no additional
spdlog/fmt shared library is required on the deployment target.

## Build

Requires CMake 3.16+, a C99/C++17 toolchain with `std::filesystem` support
(GCC 9+ recommended), threads, and the target NI-XNET shared library.
Build natively on the target, or use your NI Linux RT ARM64 CMake toolchain.
Do not link a host-architecture XNET library into an ARM64 build.

From this directory on the build machine:

```sh
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release \
  -DXNET_LIBRARY=<absolute-path-to-target-XNET-shared-library>
cmake --build build
```

For cross-compilation, also pass
`-DCMAKE_TOOLCHAIN_FILE=<your-NI-Linux-RT-ARM64-toolchain-file>`.
The output is `libxnet_canopen.so`. Deploy it with its matching NI-XNET and C++
runtime dependencies. No real hardware access occurs at CMake configuration time.

Hardware-independent tests:

```sh
cmake -S . -B build-tests -DXNET_CANOPEN_BUILD_LIBRARY=OFF \
  -DXNET_CANOPEN_BUILD_TESTS=ON -DCMAKE_BUILD_TYPE=Debug
cmake --build build-tests
ctest --test-dir build-tests --output-on-failure
```

The mock tests also support Windows/MSVC for development, not for deployment.
Linux GCC builds can additionally use `-fsanitize=address,undefined` in the C,
C++, and executable linker flags.

## Lifecycle and ownership

1. `xnet_canopen_create(&handle)`.
2. `xnet_canopen_initialize(handle, interface, model_path, persistence_path)`.
3. Bind channel groups.
4. `xnet_canopen_start(handle)`.
5. Read/write channels, inspect status, request TPDOs, or inject faults.
6. `xnet_canopen_stop(handle)` when communication should cease.
7. `xnet_canopen_uninitialize(handle)` before loading another model.
8. `xnet_canopen_destroy(handle)`; it also stops/uninitializes if necessary.

All paths/name strings are copied without normalization during initialization.
The caller is responsible for correct interface names and paths. Supply an explicit
writable persistence file path per device. Its parent directory must already
exist before storing LSS configuration. The file itself need not exist.

Successful initialization does not start communication. `start()` and `stop()`
are idempotent while initialized. `start()` after `stop()` reinitializes protocol
state, emits boot-up, and waits in NMT Pre-operational for the master. OD values
and batch IDs survive stop/start. Stop flushes network queues, so stale frames
do not survive. An NMT Stop command is different: the worker remains running.

Reset Communication restores communication defaults and retains application
values and pending LSS settings. Reset Node restores all OD defaults and reloads
saved/startup LSS settings. Both rebuild services without changing the host
handle or OD structure; batch access resolves current extensions each call.

Uninitialize releases sessions, OD, and stack and invalidates
all batch IDs. IDs are not reused within a handle. Diagnostics survive until
explicitly cleared or the handle is destroyed.

Operations are serialized per handle; the worker uses the same recursive OD
mutex. Persistence I/O releases the OD mutex while retaining worker ownership
of protocol processing. Callers may update channels during an LSS store, but
teardown joins the worker before freeing its state.

The caller must coordinate destroy with every other call and must never pass a
stale, fabricated, or already destroyed handle. A failed destroy retains the
handle so cleanup can be retried. Null handles return an argument error.

The caller (for example, LabVIEW) must enforce interface and persistence-path
exclusivity, including across library unload/reload cycles. The library does not
track duplicate interfaces or paths. NI-XNET remains responsible for detecting
conflicting external usage.

## JSON model version 1

See [examples/device.json](examples/device.json). This is an explicit simulator
model, not a direct serialization of an arbitrary EDS.

Required root fields:

| Field | Meaning |
| --- | --- |
| `schema_version` | Integer `1` |
| `node_id` | `1..127`, or `255` for LSS commissioning |
| `bitrate_kbps` | Whole-number startup bitrate from `1` to `1000` kbit/s; must be supported by the target XNET interface |
| `identity` | `vendor_id`, `product_code`, `revision_number`, `serial_number`, each uint32 |
| `objects` | Array of application VAR/ARRAY/RECORD descriptions |

Optional fields: `device_type` (default 0), `heartbeat_ms` (1000),
`worker_period_us` (1000; range 100..100000), `rpdos`, and `tpdos` (empty),
plus `node_guarding`, `sync`, and `emcy` objects described below.
Vendor ID and serial number must be nonzero; use an appropriate unique identity
for each device on the physical network. The example identity is illustrative.

Unknown fields, invalid types, and out-of-range defaults are errors. Input files
are limited to 4 MiB; custom objects to 4096. Index/subindex and unsigned numeric
values may use JSON integers or decimal/`0x` strings. Signed values use JSON
integers. Keep 64-bit integers exact when generating JSON; do not round-trip them
through a floating-point channel.

### Objects and values

Custom object indices are `0x2000..0xBFFF`. Standard communication objects are
generated by the builder, not redefined in `objects`:
`0x1000, 0x1001, 0x1003, 0x1005..0x1007, 0x100C..0x100E, 0x1014, 0x1015, 0x1017..0x1019,
0x1200`, and configured PDO communication/mapping records.

- VAR: `index`, optional `kind: "var"`, plus value fields below.
- ARRAY: `index`, `kind: "array"`, common value fields without `value`, and
  `values: [...]`. Up to 254 elements; subindex 0 is generated read-only count.
- RECORD: `index`, `kind: "record"`, and `members: [...]`. Each member has an
  explicit `subindex` and value fields. Subindex 0 must be read-only UNSIGNED8
  containing the highest subindex, with no local-write/PDO permission.

Array elements/record fields are addressed separately. Arrays are internally
lowered to CANopenNode RECORD descriptors with independent aligned value storage;
their external OD index/subindex semantics remain array-like. Layouts and
addresses are frozen until uninitialization.

Value fields:

| Field | Meaning |
| --- | --- |
| `type` | `BOOLEAN`, `UNSIGNED8/16/32/64`, `INTEGER8/16/32/64`, `REAL32/64`, `VISIBLE_STRING`, `OCTET_STRING` |
| `value` | Required initial scalar/string value; octet strings use an array of bytes |
| `size` | Required only for strings: fixed capacity `1..4096` bytes |
| `access` | SDO `ro`, `wo`, or `rw` (default `rw`) |
| `pdo` | `none`, `tx`, `rx`, or `both` (default `none`) |
| `local_write` | Whether batch channels can write it (default false), independent of SDO access |

VISIBLE_STRING defaults use printable ASCII, contain no embedded NUL, and leave
room for NUL in the fixed slot. OCTET_STRING defaults may be shorter than capacity;
the remainder is zero-filled. Strings are not PDO-mappable in this edition.
Unicode strings, DOMAIN, nonstandard integer widths, variable-size channel slots,
and arbitrary vendor-specific communication objects are explicitly unsupported.
An EDS-to-model converter is not included.

### PDOs

Each `rpdos`/`tpdos` array supports up to eight entries in PDO-number order.
Array position defines the PDO number, including inactive slots. Generated
communication/mapping records cover `0x1400..0x1407` / `0x1600..0x1607` for
RPDOs and `0x1800..0x1807` / `0x1A00..0x1A07` for TPDOs, as configured.
Each entry contains:

- `mapping`: 0..8 objects `{ "index": ..., "subindex": ... }`; total payload <=8 bytes.
- `enabled`: optional boolean, default true for nonempty mappings and false for
  empty mappings. An enabled PDO requires a nonempty mapping. Set false to retain
  a nonempty mapping without activating the PDO.
- `transmission_type`: 0..240 or 254/255, default 255.
- `event_ms`: uint16, default 0; TPDO event time or RPDO timeout.
- `inhibit_100us`: uint16, default 0, relevant for TPDOs.
- `cob_id`: explicit standard 11-bit CAN identifier, without control bits.
  Optional for PDO1..4: when omitted, the default connection-set ID is resolved
  for the active node on every stack reset. Required for PDO5..8, including
  inactive slots. PDO5..8 IDs are absolute and are not incremented by the node ID.

Disabled PDOs have bit 31 set in their generated communication COB-ID and neither
transmit nor apply received data. Empty mappings are valid inactive slots, not
mapping errors. `request_tpdo()` rejects inactive slots. Their configured IDs
remain reserved for collision checking, consistent with disabled EMCY IDs.
Enable state and mappings stay fixed until uninitialization/reinitialization.

For example, an extra TPDO slot can use
`{"cob_id": "0x43F", "mapping": [{"index": "0x2000"}]}`; an unused slot can use
`{"cob_id": "0x43E", "mapping": []}`. Place these at the intended array positions
and configure the master to match. Do not extend the PDO1..4 default-ID formula
to PDO5..8: it overlaps SDO/other service IDs and eventually exceeds 11 bits.

Mappings must have the correct `pdo` direction attributes. Local CAN-ID collisions
and restricted explicit IDs are rejected. Fixed PDO communication/mapping
records are SDO-read-only; dynamic PDO remapping remains disabled. Writable
service communication objects are described below. The caller must also avoid
collisions with other devices on the bus. Eight slots per direction do not imply
that all sixteen PDOs can be transmitted every worker cycle; size transmission
periods and event traffic to the physical CAN bus capacity.

### Node guarding, SYNC, and EMCY

These services reuse CANopenNode's implementations, with OD callbacks installed
at initialization and rebuilt on communication/node resets.

| Optional JSON object | Fields and defaults | OD / behavior |
| --- | --- | --- |
| `node_guarding` | `guard_time_ms`: uint16, default 1000; `life_time_factor`: uint8, default 3 | SDO-writable `0x100C`/`0x100D`. Life time is their product in milliseconds. Products exceeding uint32 microseconds are rejected, including runtime SDO writes. |
| `sync` | `cob_id`: 11-bit ID, default `0x80`; `period_us`: uint32, default 0; `window_us`: uint32, default 0; `counter_overflow`: 0 or 2..240, default 0 | SDO-writable `0x1005`/`0x1006`/`0x1007`. The stack consumes SYNC, monitors its period when nonzero, and applies the configured synchronous window. `0x1019` counter format is read-only and selected at initialization. SYNC production is not enabled. |
| `emcy` | Optional `cob_id`: explicit 11-bit ID with optional disable bit `0x80000000`; omitted means `0x80 + active node ID`. `inhibit_100us`: uint16, default 0 | SDO-writable `0x1014`/`0x1015`, using the stack's configurable producer and inhibit timer. |

With `heartbeat_ms > 0`, heartbeat production takes precedence and node guarding
is disabled. Set `heartbeat_ms: 0` (or write zero to `0x1017`) to use guarding.
The slave responds to RTR requests at `0x700 + node ID` with NMT state and an
alternating bit 7. `0x100E` exposes that standard ID read-only; a custom guarding
ID is not supported. The first request arms life guarding; setting guard time
or life-time factor to zero disables timeout monitoring, not RTR replies.
Re-enabling heartbeat or disabling monitoring clears a pending guarding timeout.
The wrapper's existing NMT error policy moves an Operational node to
Pre-operational on a life-guard timeout. Clearing the timeout does not
automatically return it to Operational; the master controls that transition.
Heartbeat consumer and node-guarding master remain disabled.

Example optional fields for a guarding-based simulation (merge into a complete
version-1 model):

```json
{
  "heartbeat_ms": 0,
  "node_guarding": { "guard_time_ms": 1000, "life_time_factor": 3 },
  "sync": { "cob_id": "0x80", "period_us": 0, "window_us": 0 },
  "emcy": { "inhibit_100us": 0 }
}
```

Changing the SYNC ID through SDO immediately updates the receive filter.
Changing an enabled EMCY producer's ID requires disabling it via bit 31 first,
then selecting/enabling the new ID. Restricted IDs, local SYNC/EMCY/PDO ID
collisions, and unsupported COB-ID bits are rejected. Disabled EMCY IDs remain
reserved for collision checking. Explicit EMCY ID `0x80` is rejected because
the stack treats that value as a node-relative default, not an absolute ID.

SDO changes to these settings survive stop/start. Reset Communication and Reset
Node restore the JSON communication defaults; ordinary channel/SDO changes are
not persisted. Existing heartbeat/PDO defaults are unchanged when these optional
objects are omitted. Worker/XNET scheduling still provides no strict SYNC timing
guarantee.

## Batch ABI

Binding uses three numeric arrays: uint16 indices, uint8 subindices, and uint32
sizes. Each has `count` elements. In LabVIEW configure these as numeric arrays
passed by **Array Data Pointer**, not array handles or arrays of clusters.
Validate array lengths before calling: C receives pointers, not array dimensions.
The status descriptor has eight uint32 fields (32 bytes).

`bind_batch()` copies the arrays and validates exact OD lengths, distinct OD
addresses, <=4096 bindings, and a total size <=1 MiB. Values occupy consecutive
byte slots in array order, with no padding: offset[i] is the sum of sizes before
i. Offsets are calculated internally, not supplied by the caller.
Configure groups before starting for lowest runtime latency.

```c
uint16_t indices[] = {0x2000, 0x2001};
uint8_t subindices[] = {0, 0};
uint32_t sizes[] = {2, 4};
uint32_t batch;
int32_t result = xnet_canopen_bind_batch(
    handle, indices, subindices, sizes, 2, &batch);
/* Check result before using batch. */
uint8_t buffer[6] = {0x34, 0x12, 0, 0, 0, 0};
int32_t entry_status[2];
result = xnet_canopen_write_batch(
    handle, batch, buffer, sizeof(buffer), entry_status, 2);
```

All numeric values use little-endian encoding; REAL32/64 use IEEE-754.
Strings use fixed slots and batch VISIBLE_STRING writes must contain a NUL.
Buffers/arrays are caller-owned and never retained. `buffer_size` is the byte
capacity, at least the sum of bound sizes (6 in the example).
`entry_status` is a caller-allocated int32 output array: element i reports the
result for bound variable i. `entry_count` is its element count, not a byte count,
and must equal the number of bound variables (2 in the example).
Pass the byte buffer and status array as numeric Array Data Pointers in LabVIEW.
Every write call attempts all bound entries. LabVIEW/VeriStand decides when to
call it; there is no enable mask. Bind separate groups or single-entry batches
when variables must be written independently, rather than rewriting unrelated
values that may have changed on the network.

Call-wide errors leave entry statuses unchanged and are rejected before any write.
Individual failures are recorded
per entry; later entries continue and earlier successful writes are not rolled
back. Overall result is `XNET_CO_PARTIAL_FAILURE` if any entry fails. Failed read
slots must not be consumed as valid data. Local writes require `local_write`;
reads require SDO-readable metadata. Bindings invoke current OD callbacks, not
raw memory writes. Worker and channel writes are serialized: the last successful
write wins. A batch prevents worker interleaving but is not a rollback transaction.

This replaces the first-edition descriptor-array binding signature and removes
the write-enable argument and `XNET_CO_SKIPPED`. These are source and binary API
changes: rebuild C callers and update LabVIEW Call Library Function Nodes.

## TPDO, faults, and errors

`request_tpdo(handle, one_based_number)` requires a running, Operational node.
Types 254/255 obey inhibit time; type 0 waits for SYNC. Cyclic synchronous types
1..240 reject manual requests. Requests may coalesce and transmit current values.
Acceptance is not a bus-transmission acknowledgment. Channel writes do not
automatically request a TPDO; write and request are separate operations.

`set_fault()` reports/resets simulated manufacturer faults on bits `0x40..0x4F`.
Use a nonzero CANopen error code when asserting a fault. The stack generates EMCY
and updates error state/history; host API failures do not automatically emit EMCY.

Every operation returns a status from the public header. `get_status()` exposes
runtime/NMT state, active/pending LSS values, CAN error flags, and
`node_guarding_timeout` (0 or 1). The timeout clears on a new guarding request,
disabled monitoring, enabled heartbeat, or stack restart/reset. It is a current
condition, not an event history. The external model decides the drive reaction
(for example, according to `0x6007`); no CiA 402 state machine is added here.
The stack reports/clears the guarding communication error and emits EMCY subject
to its NMT/producer rules.

`node_guarding_timeout` replaces the previously reserved final uint32 in the
status structure. Its size remains 32 bytes and the field is at byte offset 28;
update C field references and the meaning of that word in LabVIEW descriptors.
Hardware bus-off/error counters feed the stack's CAN error status. Overflow flags
are latched until stack restart/reset so losses remain visible.

Operation results use `XNET_CO_*` (`XNET_CO_OK`, `XNET_CO_INVALID_STATE`, etc.).
The separate `runtime_state` field uses `XNET_CO_RUNTIME_UNINITIALIZED`,
`XNET_CO_RUNTIME_STOPPED`, `XNET_CO_RUNTIME_RUNNING`, and `XNET_CO_RUNTIME_FAILED`.
These runtime names replace the initial `XNET_CO_UNINITIALIZED/STOPPED/RUNNING/FAILED`
names; their numeric values (0..3) and the binary ABI are unchanged.
Neither runtime state nor API results are CANopen NMT state or CAN error bitmasks.
The public header documents each enum value, structure field, and API contract.

`get_last_error()` copies the latest per-handle failure as an atomic code/message
snapshot. `message_required` includes NUL; insufficient capacity returns
`XNET_CO_BUFFER_TOO_SMALL` without replacing the original diagnostic. Query with
NULL message/capacity 0 to obtain the required size. Messages are capped at 2047
bytes. Successful operations and diagnostic reads do not clear errors.
`clear_error()` clears the diagnostic, not a physical bus fault or latched flag.

Worker errors move the runtime to FAILED and quiesce sessions. Uninitialize and
initialize again to recover; do not assume a successful earlier start means the
worker is still healthy. Cleanup failures are explicit and retryable.
XNET nonzero statuses, including warnings, are conservatively surfaced as errors.

## File logging

Set **`XNET_CANOPEN_LOG_PATH`** in
[src/logging_config.h](src/logging_config.h) to the desired absolute file path
and rebuild. A compiler definition of the same macro overrides the header.
There is deliberately no runtime path parameter or JSON logging option.
Default paths are:

- NI Linux RT/Linux: `/var/local/xnet_canopen/xnet_canopen.log`
- Windows: `C:\ProgramData\xnet_canopen\xnet_canopen.log`

The deployment account must be able to create/write the parent directory.
The spdlog sink creates missing parent directories. Logging first opens during
`initialize()`, before acquiring XNET sessions. Failure to open/initialize the
file returns `XNET_CO_IO_ERROR` and is available through `get_last_error()`.
Handle allocation itself does not open a file; errors before logging starts
remain available through the normal API diagnostic.

All handles in this library instance share one thread-safe rotating sink.
The active file is always the macro-defined path, with a 5 MiB rotation
threshold and up to three backups (for example `xnet_canopen.1.log`).
Existing logs are appended, not truncated. Do not have another process/library
instance rotate or write the same file concurrently. The integration does not
alter the host application's default spdlog logger or registry.

Each device record includes:

```text
[timestamp] [pid=... tid=...] [info] [device=7 interface=CAN1 identity=00000300:00004644:00000000:000003e9 node=10 kbps=500] device started
```

The **primary trace key is process + device-instance number + full identity**
(`vendor:product:revision:serial`, hexadecimal). The instance number is allocated
once per handle and survives stop/start, NMT resets, and LSS changes. Interface,
active node ID and bitrate are context, not the stable key: node ID alone is
unsuitable because it can change or be reused on another bus. Node `255` means
LSS-unconfigured; node `0`/zero identity indicate unavailable startup/cleanup
context. A new initialization on the same handle may load a different identity.

Logged events include lifecycle and NMT changes, NMT resets, accepted
communication-setting writes, LSS pending settings/bitrate activation/storage,
guarding timeout/recovery, model fault assertions/clears, and API/worker errors.
Identical consecutive API/driver diagnostics are deduplicated until the stored
diagnostic changes or is cleared. Normal batch values and raw CAN frames are
not logged, so ordinary cyclic operation does not generate per-frame traffic.
Observed state changes in the log do not replace a lossless application-event API.

Logging is synchronous and flushes user-space buffers after each emitted record;
it does not promise power-loss durability. Disk I/O can extend API/worker
execution time, consistent with the current non-strict-timing scope.
Internal device log calls accept spdlog-style `{}` format arguments; plain
messages are logged literally. Formatting failures use the same diagnostic path
as sink failures, whose fixed-capacity error reporting does not allocate.
If a later write/rotation/flush fails, the integration reports it to stderr and
the handle diagnostic without stopping CAN communication or undoing completed
protocol operations. `XNET_CO_IO_ERROR` is recorded if no earlier diagnostic is
active; otherwise the original code is preserved and logging-failure text is
appended when space permits. Successful API calls do not clear that diagnostic.

Mock-only CMake builds override the path to the build directory, rather than
writing deployment logs. Tests cover node-ID changes without losing the trace
key, concurrent writes, the rotation threshold, startup open failure/retry, and
runtime sink failures without recursive logging or loss of the original error.

## LSS and persistent configuration

The existing LSS slave supplies global/selective selection, fastscan, identity
and node-ID inquiries, node-ID configuration, bitrate configuration/activation,
and Store Configuration. The wrapper implements adapter and storage callbacks.
LSS uses the standard fixed-rate table: `10, 20, 50, 125, 250, 500, 800, 1000`
kbit/s. Startup JSON and saved configuration also accept other whole-number
rates within `1..1000` kbit/s; they are not restricted to the LSS table.
XNET reports unsupported hardware settings when the rate is applied.
Automatic bitrate detection (table index 9, passed to the callback as rate `0`)
is rejected explicitly because no detection implementation is provided.

Activation stops/flushes sessions immediately, waits the requested delay,
changes the interface bitrate, waits the delay again, and resumes. Protocol
processing/transmission is suspended during those intervals. Adapter failure
marks the worker FAILED; activation has no fabricated success response.

Store writes a version-1 JSON file with identity, pending node ID, and pending
bitrate. A missing file or any exception while loading it uses both model startup
settings. Invalid, unreadable, or identity-mismatched files do not fail initialization
or Reset Node; load exceptions are recorded as `XNET_CO_IO_ERROR` in diagnostics.
Saved values override only startup LSS
settings. Load occurs at initialize and Reset Node, not Reset Communication or
stop/start. Ordinary channel writes and temporary LSS changes do not write disk.

On Linux, replacement uses an exclusive temporary file in the destination
directory, complete write + fsync, atomic rename, and directory fsync. LSS storage
success is returned only after completion. A directory-sync failure can occur
after replacement, so a reported store failure does not prove the old file is
still present; inspect/retry. No flash writes occur in the normal 1000 Hz path.
OD store/restore objects `0x1010/0x1011` are not implemented by this LSS mechanism.

## Validation and limits

Automated groups exercise model validation and numeric limits, batch
Enable/partial errors/string buffers, concurrent callers, caller-managed resources and cleanup,
real SDO/PDO/NMT/EMCY traffic through the stack, LSS selection/inquiry/node-ID
changes, bitrate silence sequencing, persistence/reload/failure paths, and
synchronous manual requests. Additional groups cover guarding RTR filtering and
toggle replies, life-time timeout/recovery, heartbeat/guarding switching,
configurable SYNC/EMCY, collision/overflow rejection, SYNC timers/counters, and
communication-setting retention/reset. A C source verifies the public header,
32-byte status layout and guarding-status offset.
Eight-PDO groups cover all eight transmit/receive paths, ninth-slot rejection,
explicit-ID requirements, higher-slot collisions (including live SYNC/EMCY writes),
empty/disabled slots, synchronous PDO8, stop/start, NMT resets and LSS node changes.

Development validation: Windows/MSVC and Linux x86-64/GCC mock builds/tests;
Linux ASan/UBSan. These are not NI Linux RT ARM64 hardware acceptance.

Important limits:

- No CiA 402 state machine, motion model, EDS converter, or VeriStand UI is supplied.
- Workers use normal inherited OS scheduling; no hard real-time guarantee.
- Receive work is bounded to one 8 KiB read per iteration. Output writes one frame
  at a time with timeout zero, avoiding ambiguous multi-frame partial writes.
- Adapter output is bounded to one pending copy per stack transmit buffer.
  Queue overflow is reported, not hidden; XNET queue-full frames are retried.
- Late synchronous frames still in the adapter queue can be removed. Frames
  already handed to XNET cannot be selectively cancelled through `nxFlush`.
  No strict SYNC-window guarantee is made; session-wide flushing is reserved
  for stop/reset/bitrate switching, not cancelling one PDO.
- No automatic recovery policy is implemented for bus-off or XNET failures.

Manual acceptance on the target should cover same-port stream input/output,
boot-up and heartbeat, expedited/segmented SDO, fixed RPDO/TPDO in appropriate
NMT states, SYNC, resets, EMCY, complete LSS commissioning including bitrate
switching, guarding RTR/toggle/timeout behavior when selected, writable SYNC/EMCY
configuration, and file reload after restarting the process. Verify queue behavior,
physical timing, installed XNET ABI compatibility, and your filesystem's
durability semantics.
