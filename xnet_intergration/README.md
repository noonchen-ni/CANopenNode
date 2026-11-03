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
- [tools/eds_to_json.py](tools/eds_to_json.py): EDS-to-JSON template converter (Python 3.10+, no third-party packages).
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
Successful protocol resets increment separate counters in `get_status()`, so
the external model need not infer reset identity from a transient NMT state.

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
| `value_offset` | Optional `"NODE_ID"` for integer defaults: add the active node ID to `value` when restoring the default |
| `size` | Required only for strings: fixed capacity `1..4096` bytes |
| `access` | SDO `ro`, `wo`, or `rw` (default `rw`) |
| `pdo` | `none`, `tx`, `rx`, or `both` (default `none`) |
| `local_write` | Whether batch channels can write it (default false), independent of SDO access |

VISIBLE_STRING defaults use printable ASCII, contain no embedded NUL, and leave
room for NUL in the fixed slot. OCTET_STRING defaults may be shorter than capacity;
the remainder is zero-filled. Strings are not PDO-mappable in this edition.
Unicode strings, DOMAIN, nonstandard integer widths, variable-size channel slots,
and arbitrary vendor-specific communication objects are explicitly unsupported.

`value_offset: "NODE_ID"` supports signed/unsigned integer VARs, record members
other than subindex 0, and a common offset on every ARRAY element. The stored
`value` is the base, not an already-resolved number. Initialization and Reset
Node evaluate the default against the active node, including persisted startup
settings. Reset Communication, stop/start and LSS node-ID changes do not overwrite
live application values. Node ID 255 (unconfigured) contributes zero. Addition
must fit the declared type; overflow is an error, not wrapping arithmetic.

### EDS conversion

From this directory, generate a template and capture its warnings:

```powershell
python .\tools\eds_to_json.py .\eds\Kinco_FD_20250417_V1.2.eds .\examples\kinco.template.json 2> conversion-warnings.txt
```

The output argument is optional. With only an EDS path, the converter replaces
its extension with `.json` and writes beside the source EDS; the `.inputs.md`
guide is placed in the same folder. An explicit output path still takes precedence:

```powershell
python .\tools\eds_to_json.py .\eds\Kinco_FD_20250417_V1.2.eds
```

The script uses only the Python standard library. `--encoding cp1252` selects
an explicit alternative to the default `utf-8-sig`; encoding errors are not
silently replaced. `--warnings-as-errors` reports warnings but does not write
output. Conversion errors also leave any existing output unchanged.

The output is a **template**, not necessarily an initializable model. Unknown
values are the literal JSON string `"{USER_INPUT}"`, including node ID, selected
bitrate, worker period, missing required nonzero identity fields, string
capacities, unknown types, and unresolved expressions or mappings. Replace each
placeholder with a correctly typed JSON value before initialization: for
example, `10`, `125`, or `true`, not
quoted numeric/boolean text. The runtime rejects unresolved placeholders and
reports their JSON path. String capacity must include space for a visible
string's terminating NUL. For `ro`/`const` strings with known initial values,
capacity is inferred automatically: ASCII byte length plus one for
VISIBLE_STRING, or at least one byte for OCTET_STRING. Writable strings and
strings with unresolved defaults still need an explicit capacity.

Read-only SDO access does not remove the runtime's storage requirement, nor does
it disable host writes when `local_write: true`. Inferred capacity is fixed:
longer host strings require manually increasing `size`. For Kinco `0x6504`, the
known manufacturer string produces `size: 31` without a placeholder.

To reduce manual editing, generated application values have `local_write: true`,
independently of their network SDO permissions. Structural subindex 0 stays
read-only and is not locally writable. Missing numeric OD defaults become `0`;
missing VISIBLE_STRING/OCTET_STRING defaults become `""`/`[]`. Existing EDS
defaults are preserved. Missing supported numeric communication values also
become zero, but required nonzero identity values, COB-IDs and mapping structure
are not guessed. In particular, a missing PDO transmission type becomes `0`
(synchronous acyclic, waiting for SYNC), **not** asynchronous type 254/255.
Automatic defaults are reported as warnings: they are simulator starting values,
not evidence of the real device's factory settings, and may differ from EDS
limits. Unsupported expressions still require manual resolution, not zeroing.

JSON comments are not needed: the converter also writes
`kinco.template.inputs.md` next to `kinco.template.json`. This Markdown companion guide
is **not** loaded by the simulator. It identifies application fields by searchable
OD index/subindex and field name, rather than by position in the JSON array.
JSON Pointer paths are retained only as a secondary reference for tools.
Expected types/ranges, descriptions and illustrative examples are included.
Supported bitrates from the EDS and object names/addresses/default expressions
are included where applicable. It also retains all conversion warnings and
explains the automatic-default policies.
Open it in VS Code's Markdown preview for a readable input table and detailed
field explanations; only the model file remains JSON.

For example, `/node_id` is described as an integer `1..127`, or `255` for LSS
commissioning, with example `10`. A string `size` field explains its byte
capacity and NUL requirement and gives the minimum needed by its initial value.
An application field is labeled, for example,
`index=0x2001, subindex=0x00, size`: search for `0x2001` in the editor, then edit
that object's `size` (or the indicated record member's field).
Examples are guidance, not confirmed commissioning settings; edit the model
file, not the guide. `--warnings-as-errors` leaves both files unchanged.

The converter:

- Imports application VARs and explicit ARRAY/RECORD subentries in
  `0x2000..0xBFFF`. Arrays are emitted as records to preserve per-subentry types,
  permissions and sparse indices. Subindex 0 is normalized, with a warning, to
  the runtime's read-only UNSIGNED8 highest-subindex convention.
- Uses `0x1018` defaults for identity, with DeviceInfo fallback for vendor,
  product and revision. A missing or zero serial number needs user input.
  Translates device type, heartbeat, guarding, SYNC and EMCY settings into their
  dedicated JSON fields, rather than redefining communication objects.
- Reads actual PDO mappings from `0x1600..0x1607` / `0x1A00..0x1A07`, respecting
  the active subindex-0 count, and communication settings from
  `0x1400..0x1407` / `0x1800..0x1807`. Preserves slot numbers, disable bits and
  timing units. Validates references, directions, full target widths and the
  64-bit payload limit. Missing active mapping words produce a placeholder for
  the complete mapping list; known invalid mappings are errors.
- Preserves PDO eligibility independently of actual mappings. `ro`/`const`
  permit TX, `wo` permits RX, `rw` permits both, and `rwr`/`rww` select TX/RX.
  BOOLEAN occupies one byte in this runtime; bit-level/partial-field mapping
  is not supported.
- Recognizes integer constants, `$NODEID`, `$NODEID + constant`, and
  `constant + $NODEID`, without evaluating arbitrary Python or EDS expressions.
  Integer application defaults use `value_offset`; PDO/SYNC/EMCY IDs use
  `cob_id_offset`. Other expressions, or expressions in fields without offset
  support, produce explicit placeholders and warnings with the source text.

For example, `DefaultValue=$NODEID+0x240` in a PDO communication COB-ID becomes:

```json
{
  "cob_id": 576,
  "cob_id_offset": "NODE_ID"
}
```

The runtime resolves this to `0x24A` at node 10 and `0x26A` at node 42. A scalar
default of `$NODEID+0x80` becomes `"value": 128, "value_offset": "NODE_ID"`;
application defaults follow the reset rules above, not live COB-ID rebasing.
These optional schema-version-1 extensions require the updated runtime; older
runtime binaries reject the new keys.

Every warning is printed to stderr and saved in the companion guide. Warnings
cover missing inputs, automatic defaults, normalized
counts, unsupported/unrepresented fields, unused metadata and definitions,
omitted communication objects, fixed/read-only PDO records, RTR limitations,
and absent optional services that will use runtime defaults. Names, limits,
flags and supported-feature lists are not imported as runtime validation rules.
Unsupported data types remain explicit type/value placeholders; compact
subobject layouts and unsupported object kinds require manual expansion or
conversion and are errors. No device behavior is synthesized.

Standalone converter tests:

```powershell
python -m unittest discover -s .\tests -p test_eds_to_json.py
```

When Python 3.10+ is found, the CMake test build also registers converter tests
and validates filled test templates through the real C++ loader. Customer EDS
checks run only when the corresponding supplied file is present. End-to-end
tests supply test settings for remaining placeholders; production commissioning
settings are left for the user.

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
  inactive slots. Unless `cob_id_offset` is specified, PDO5..8 IDs are absolute
  and are not incremented by the node ID.
- `cob_id_offset`: optional `"NODE_ID"` with an explicit `cob_id`. Treats that
  ID as a base and adds the active node ID on initialization and communication
  resets, including PDO5..8. Node 255 contributes zero. Omission retains the
  existing absolute-ID behavior for explicit IDs. Resolved IDs must be valid,
  nonrestricted 11-bit IDs and must not collide with other local services.

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

#### Deferred communication-interruption policy

The current scope does not require configurable interruption reactions.
`get_status().node_guarding_timeout` reports the current condition to the
external model, which owns drive alarms and motion reactions. However, the
communication stack independently reports the guarding error through its EMCY
rules and moves an Operational node to Pre-operational, preventing Operational
PDO exchange. The model cannot undo these effects merely by ignoring the flag.

A configured application object such as Kinco `0x6007:00` is currently ordinary
OD storage; its value does not alter this library policy. If tests later require
an "ignore interruption" setting, first confirm the expected NMT, PDO, EMCY and
recovery behavior. Then provide a generic communication-error policy mechanism,
with vendor-object interpretation in the device adapter/model. This is an
important deferred compatibility item, not an implemented option or a current
requirement. Existing timeout detection and reporting remain unchanged.

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

SYNC and EMCY also accept `cob_id_offset: "NODE_ID"` alongside an explicit
`cob_id` base. They follow the same node-relative rebasing and resolved-ID
validation as PDOs. Accepted SDO changes override that relative startup default
across stop/start; a communication/node reset restores and re-evaluates it.
EMCY's omitted `cob_id` still selects its existing `0x80 + node ID` default.

## Batch ABI

Binding uses three numeric arrays: uint16 indices, uint8 subindices, and uint32
sizes. Each has `count` elements. In LabVIEW configure these as numeric arrays
passed by **Array Data Pointer**, not array handles or arrays of clusters.
Validate array lengths before calling: C receives pointers, not array dimensions.
The `get_status()` descriptor has ten uint32 fields (40 bytes); it is separate
from the per-entry int32 status array returned by batch operations.

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
runtime/NMT state, active/pending LSS values, separate reset-completion counters, and
CAN error flags, plus
`node_guarding_timeout` (0 or 1). The timeout clears on a new guarding request,
disabled monitoring, enabled heartbeat, or stack restart/reset. It is a current
condition, not an event history. The external model decides the drive reaction
(for example, according to `0x6007`); no CiA 402 state machine is added here.
The stack reports/clears the guarding communication error and emits EMCY subject
to its NMT/producer rules.

### Status snapshot and reset notification

`get_status()` remains the single read-only runtime/protocol information API.
It needs no batch binding, JSON configuration, or extra network-visible OD
entries. All fields are copied under the same runtime lock.

| Byte offset | uint32 field | Meaning |
| --- | --- | --- |
| 0 | `runtime_state` | Host library lifecycle state. |
| 4 | `nmt_state` | Current CANopen NMT state, not the CiA 402 drive state. |
| 8 | `active_node_id` | Active node ID. |
| 12 | `pending_node_id` | LSS-selected node ID. |
| 16 | `active_bitrate_kbps` | Applied interface bitrate. |
| 20 | `pending_bitrate_kbps` | LSS-selected bitrate. |
| 24 | `can_error_status` | Stack CAN error bitmask. |
| 28 | `node_guarding_timeout` | Current life-guard timeout condition, 0 or 1. |
| 32 | `reset_node_count` | Successfully completed Reset Node operations. |
| 36 | `reset_communication_count` | Successfully completed Reset Communication operations. |

**ABI change:** the structure is now 40 bytes, previously 32. The original eight
fields retain their offsets; `node_guarding_timeout` remains at offset 28.
Rebuild C callers and enlarge LabVIEW Call Library Function Node output
descriptors to ten uint32 fields before using the updated library. The function
has no output-size parameter and cannot safely accept an old 32-byte buffer.

Reset counters are stored outside the OD and stack. Each increments only after
the reset restores the required OD values, rebuilds services, and successfully
restarts sessions. This reports local reset completion, not confirmation that a
boot-up frame has reached the bus. A Reset Node increments only its own counter;
its communication reinitialization does not also increment the communication
counter. Stack-requested protocol resets use the same path, including LSS
commissioning when it requests a communication reset.

Both counters start at zero. Successful `initialize()` clears them; failed
initialize attempts do not. They survive protocol resets, host stop/start,
uninitialize, and runtime failure. Without a stack, the retained counters and
runtime state remain readable while the other fields are zero. Neither polling
nor `clear_error()` clears them, and ordinary initialization/start/stop does not
count as a protocol reset. Counters are not persisted across destroyed handles.

At model startup, record a baseline. Each model step compares both counters
against the previous snapshot: Reset Node normally requires reinitializing the
external controller consistently with application OD defaults; Reset
Communication preserves application state. The library does not reset the
external model or implement CiA 402 behavior. Each counter wraps modulo 2^32;
uint32 subtraction gives the number of completions modulo 2^32. Separate counters
retain both reset types between reads but do not preserve their chronological
order. A full wrap between reads cannot be distinguished from no change.

Status and OD batch calls are separately synchronized. For a consistent
read-only model input sample, read status, read the input batch, then read status
again. If either counter changed, discard/retry the input sample and handle the
reset. This is not a transaction around model execution/feedback writes: a reset
can still occur between the final check and a feedback write. Check runtime and
batch results as well, especially after reset failures. Current NMT state and
guarding timeout are snapshots, not lossless transition histories.

Hardware bus-off/error counters feed the stack's CAN error status. Overflow flags
are latched until stack restart/reset so losses remain visible.

Operation results use `XNET_CO_*` (`XNET_CO_OK`, `XNET_CO_INVALID_STATE`, etc.).
The separate `runtime_state` field uses `XNET_CO_RUNTIME_UNINITIALIZED`,
`XNET_CO_RUNTIME_STOPPED`, `XNET_CO_RUNTIME_RUNNING`, and `XNET_CO_RUNTIME_FAILED`.
These runtime names replace the initial `XNET_CO_UNINITIALIZED/STOPPED/RUNNING/FAILED`
names; their numeric values (0..3) are unchanged.
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
40-byte status layout, unchanged guarding-status offset, and appended reset
counter offsets. Reset-status groups cover completion counts, preserved/reset
application values, repeated/broadcast commands, ordinary NMT state changes,
stop/start, teardown/reinitialization, failed resets/initialization and uint32
wraparound.
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
