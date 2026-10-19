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
- [examples/device.json](examples/device.json): complete working model.
- [tests/](tests/): fake XNET API, real stack protocol tests, and C header/ABI checks.

The supplied NI header and JSON single header are used unchanged.

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

All paths/name strings are copied during initialization. Supply an explicit
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

Uninitialize releases sessions, OD, stack, and interface ownership and invalidates
all batch IDs. IDs are not reused within a handle. Diagnostics survive until
explicitly cleared or the handle is destroyed.

Operations are serialized per handle; the worker uses the same recursive OD
mutex. Persistence I/O releases the OD mutex while retaining worker ownership
of protocol processing. Callers may update channels during an LSS store, but
teardown joins the worker before freeing its state.

The caller must coordinate destroy with every other call and must never pass a
stale, fabricated, or already destroyed handle. A failed destroy retains the
handle so cleanup can be retried. Null handles return an argument error.

Interface and persistence-path exclusivity is enforced within this library
instance/process. Interface names are normalized to uppercase; use canonical
physical names such as `CAN1`, not different aliases for the same port. NI-XNET
remains responsible for detecting conflicting external usage.

## JSON model version 1

See [examples/device.json](examples/device.json). This is an explicit simulator
model, not a direct serialization of an arbitrary EDS.

Required root fields:

| Field | Meaning |
| --- | --- |
| `schema_version` | Integer `1` |
| `node_id` | `1..127`, or `255` for LSS commissioning |
| `bitrate_kbps` | `10, 20, 50, 125, 250, 500, 800, 1000` |
| `identity` | `vendor_id`, `product_code`, `revision_number`, `serial_number`, each uint32 |
| `objects` | Array of application VAR/ARRAY/RECORD descriptions |

Optional fields: `device_type` (default 0), `heartbeat_ms` (1000),
`worker_period_us` (1000; range 100..100000), `rpdos`, and `tpdos` (empty).
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
`0x1000, 0x1001, 0x1003, 0x1005..0x1007, 0x1014, 0x1015, 0x1017..0x1019,
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

Each `rpdos`/`tpdos` array supports up to four entries in PDO-number order.
Each entry contains:

- `mapping`: 1..8 objects `{ "index": ..., "subindex": ... }`; total payload <=8 bytes.
- `transmission_type`: 0..240 or 254/255, default 255.
- `event_ms`: uint16, default 0; TPDO event time or RPDO timeout.
- `inhibit_100us`: uint16, default 0, relevant for TPDOs.
- Optional explicit `cob_id` (standard 11-bit). When omitted, the default
  connection-set ID is resolved for the active node on every stack reset.

Mappings must have the correct `pdo` direction attributes. Local CAN-ID collisions
and restricted explicit IDs are rejected. Fixed communication/mapping records
are SDO-read-only. Only producer heartbeat time is writable among the generated
communication configuration; the installed extension updates its active timer.

The master still controls NMT and can provide SYNC. SYNC consumer ID is fixed
at `0x80`, without a SYNC counter/window timeout configured in this edition.
Heartbeat consumer and node guarding are not enabled.

## Batch ABI

The binding descriptor has size 12 bytes, with fields at offsets:
`index:0`, `subindex:2`, `reserved:3`, `offset:4`, `size:8`.
Use the platform's normal C ABI, not packed clusters with a different stride.
The status descriptor has eight uint32 fields (32 bytes).

`bind_batch()` validates exact OD lengths, distinct OD addresses, non-overlapping
buffer ranges, reserved=0, <=4096 bindings, and a buffer span <=1 MiB.
Configure groups before starting for lowest runtime latency.

```c
xnet_canopen_binding_t bindings[] = {
    {0x2000, 0, 0, 0, 2},
    {0x2001, 0, 0, 2, 4}
};
uint32_t batch;
int32_t result = xnet_canopen_bind_batch(handle, bindings, 2, &batch);
/* Check result before using batch. */
uint8_t buffer[6] = {0x34, 0x12, 0, 0, 0, 0};
uint8_t enable[2] = {1, 0};
int32_t entry_status[2];
result = xnet_canopen_write_batch(
    handle, batch, buffer, sizeof(buffer), enable, entry_status, 2);
```

All numeric values use little-endian encoding; REAL32/64 use IEEE-754.
Strings use fixed slots and batch VISIBLE_STRING writes must contain a NUL.
Buffers/arrays are caller-owned and never retained. The final count argument
must equal the bound count; status and Enable arrays must have that many elements.
Disabled writes return `XNET_CO_SKIPPED`. Enable=1 permits a write on every call,
not only on a rising edge.

Call-wide errors are rejected before any write. Individual failures are recorded
per entry; later entries continue and earlier successful writes are not rolled
back. Overall result is `XNET_CO_PARTIAL_FAILURE` if any entry fails. Failed read
slots must not be consumed as valid data. Local writes require `local_write`;
reads require SDO-readable metadata. Bindings invoke current OD callbacks, not
raw memory writes. Worker and channel writes are serialized: the last successful
write wins. A batch prevents worker interleaving but is not a rollback transaction.

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
runtime/NMT state, active/pending LSS values, and CAN error flags.
Hardware bus-off/error counters feed the stack's CAN error status. Overflow flags
are latched until stack restart/reset so losses remain visible.

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

## LSS and persistent configuration

The existing LSS slave supplies global/selective selection, fastscan, identity
and node-ID inquiries, node-ID configuration, bitrate configuration/activation,
and Store Configuration. The wrapper implements adapter and storage callbacks.
All standard fixed rates listed above are offered; hardware-specific support
must be validated on the target. Automatic bitrate detection (table index 9)
is rejected explicitly because no detection implementation is provided.

Activation stops/flushes sessions immediately, waits the requested delay,
changes the interface bitrate, waits the delay again, and resumes. Protocol
processing/transmission is suspended during those intervals. Adapter failure
marks the worker FAILED; activation has no fabricated success response.

Store writes a version-1 JSON file with identity, pending node ID, and pending
bitrate. Missing file uses startup settings; an existing invalid/unreadable or
identity-mismatched file is an error. Saved values override only startup LSS
settings. Load occurs at initialize and Reset Node, not Reset Communication or
stop/start. Ordinary channel writes and temporary LSS changes do not write disk.

On Linux, replacement uses an exclusive temporary file in the destination
directory, complete write + fsync, atomic rename, and directory fsync. LSS storage
success is returned only after completion. A directory-sync failure can occur
after replacement, so a reported store failure does not prove the old file is
still present; inspect/retry. No flash writes occur in the normal 1000 Hz path.
OD store/restore objects `0x1010/0x1011` are not implemented by this LSS mechanism.

## Validation and limits

Fourteen automated groups exercise model validation and numeric limits, batch
Enable/partial errors/string buffers, concurrent callers, ownership and cleanup,
real SDO/PDO/NMT/EMCY traffic through the stack, LSS selection/inquiry/node-ID
changes, bitrate silence sequencing, persistence/reload/failure paths, and
synchronous manual requests. A C source verifies the public header and layouts.

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
switching, and file reload after restarting the process. Verify queue behavior,
physical timing, installed XNET ABI compatibility, and your filesystem's
durability semantics.
