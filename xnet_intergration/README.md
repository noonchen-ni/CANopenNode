# NI-XNET CANopen device simulator

This is a C++17 shared-library wrapper around the existing CANopenNode C stack,
intended for NI Linux RT ARM64 and a VeriStand Custom Device. All integration
sources are in this directory. No existing CANopenNode source files are changed.

Each handle owns one simulated device, one physical XNET interface, two raw
frame-stream sessions, one runtime Object Dictionary, and one processing worker.
The library owns all frame I/O. A CAN database is not needed by XNET.

This README is the maintenance and design reference for the integration. The
[public C header](include/xnet_canopen.h) defines the exact API contracts;
the implementation and tests define supported behavior. Historical API sketches,
implementation progress reports, and superseded design discussions are not part
of this guide.

## Contents

- [Design background and responsibility boundaries](#design-background-and-responsibility-boundaries)
- [Files](#files)
- [Build](#build)
- [Lifecycle and ownership](#lifecycle-and-ownership)
- [JSON model version 1](#json-model-version-1)
- [Batch ABI](#batch-abi)
- [TPDO, faults, and errors](#tpdo-faults-and-errors)
- [File logging](#file-logging)
- [LSS and persistent configuration](#lss-and-persistent-configuration)
- [External device-model integration](#external-device-model-integration)
- [Kinco configuration and compatibility notes](#kinco-configuration-and-compatibility-notes)
- [Maintaining and extending the integration](#maintaining-and-extending-the-integration)
- [Validation and limits](#validation-and-limits)

## Design background and responsibility boundaries

The purpose is to simulate a CANopen controlled node for an external master,
with VeriStand supplying application behavior and channels. CANopenNode provides
communication, not a motor/plant model or a CiA 402 state machine. Adding drive
objects or importing an EDS does not implement drive behavior. Host runtime state,
CANopen NMT state, and CiA 402 drive state are three separate concepts:
NMT Operational does not mean a drive is Operation Enabled.

```text
External CANopen master
    <-> classic CAN bus <-> NI hardware / NI-XNET
    <-> library-owned adapter / CANopenNode / runtime OD
    <-> C batch, status, TPDO-request and fault APIs
    <-> VeriStand adapter / channels
    <-> external device / CiA 402 / plant model
```

| Layer | Responsibility |
| --- | --- |
| NI-XNET adapter | Own two raw frame-stream sessions on one physical interface; receive/filter frames, queue output, apply bitrate changes, and expose bus/queue failures. |
| CANopenNode integration | NMT, SDO server, fixed RPDO/TPDO, heartbeat or node/life guarding, SYNC consumer, EMCY producer, LSS slave, OD storage, lifecycle and status. |
| Provisioning / channel manifest | Generate versioned JSON before initialization; describe OD addresses, types, permissions, raw defaults, fixed PDO layout and channel encoding from one consistent model. |
| VeriStand adapter / external model | Schedule model execution, interpret commands, implement selected device modes, motion, I/O priority, scaling, feedback, alarms and fault-reset behavior. |
| Optional device-specific handlers | Interpret vendor commands only when selected by the test contract; use generic integration hooks where needed rather than embedding Kinco meanings in the communication core. |

Important design choices:

- The deployment target is **NI Linux RT ARM64**. Windows and Linux x86-64
  mock builds are development tools, not deployment/hardware certification.
- Each handle owns one device and one interface. LabVIEW holds the handle, not
  XNET sessions, and does not pass frames or call a protocol-processing step.
  Interface/path exclusivity is caller-managed, not enforced by a library registry.
- Construct the OD once at initialization, rather than generating C for each
  customer or resizing it while running. Values can change, but descriptor and
  storage addresses must stay stable until uninitialization.
- PDO mappings, enable state and communication records are fixed per
  initialization. Configure the master not to depend on startup SDO writes to
  those records. Live SYNC/EMCY/guarding settings are separate supported features.
- VeriStand access is normally 1000 Hz, with synchronized latest-value exchange.
  The contract does not promise every command edge, next-cycle application, or
  same-SYNC-cycle feedback. CAN traffic rate and channel polling rate are independent.
- Full LSS slave integration includes adapter bitrate activation and file-backed
  storage, not merely node-ID assignment. Unsupported operations and failures
  must not be acknowledged as successful.
- No hard real-time deadline is promised. A previously observed XNET
  write-to-send latency of about 200 us was a measurement under particular
  conditions, not a worst-case bound or total model-to-wire latency.

### CANopen concepts needed for maintenance

This port uses classic CAN: standard 11-bit identifiers and at most eight payload
bytes, not CANopen FD or 29-bit frames. An OD address is a 16-bit index plus an
8-bit subindex, for example `0x1018:01` (vendor ID). OD values remain local unless
accessed through a protocol service or the host API.

| Service | Local simulated-device role |
| --- | --- |
| SDO | Server answering remote OD reads/writes; no SDO client is needed to answer a master. |
| PDO | TPDO producer and RPDO consumer, named from the simulated node's perspective. |
| NMT | Controlled node; Pre-operational allows SDO but not Operational PDO exchange. |
| Heartbeat / guarding | Heartbeat producer or guarding slave; no heartbeat consumer/guarding master. |
| SYNC / EMCY / LSS | SYNC consumer, EMCY producer and LSS slave. |

One XNET stream session carries many protocol CAN IDs; a PDO is not a session.
TPDO/RPDO numbers are local slot numbers, not bus-wide identifiers. A producer
and consumer must agree on CAN ID and payload layout; their local PDO numbers
and OD addresses need not match. Normally only one producer uses a given PDO ID.

An SDO transfer can span frames: expedited transfers carry up to four value
bytes, segmented transfers use successive segments of up to seven bytes. This
port enables segmented, not block, SDO. For node 10, the default SDO request/reply
IDs are `0x60A`/`0x58A`; boot-up and heartbeat use `0x70A`. The server and adapter
handle these exchanges without application calls to SDO-client functions.

The runtime OD is the live storage/metadata. EDS/XDD describes capabilities for
tools; DCF/XDC describes configured devices. A DBC is not required for raw XNET
frame I/O. JSON provisioning and EDS conversion do not synthesize application
behavior or engineering-unit conversions.

## Files

- [include/xnet_canopen.h](include/xnet_canopen.h): public C ABI.
- [src/model.cpp](src/model.cpp): version-1 JSON validation and OD construction.
- [src/driver.cpp](src/driver.cpp): XNET sessions and CANopen driver boundary.
- [src/device.cpp](src/device.cpp): lifecycle, worker, LSS, channels, diagnostics.
- [src/internal.hpp](src/internal.hpp): private storage, ownership, bindings and synchronization.
- [src/CO_driver_target.h](src/CO_driver_target.h): shared stack feature masks, frame layout and lock macros.
- [src/persistence.cpp](src/persistence.cpp): local JSON storage with atomic replacement.
- [src/logging.cpp](src/logging.cpp): private spdlog file backend and per-device tags.
- [src/logging_config.h](src/logging_config.h): compile-time fixed log path.
- [examples/device.json](examples/device.json): complete working model.
- [tools/eds_to_json.py](tools/eds_to_json.py): EDS-to-JSON template converter (Python 3.10+, no third-party packages).
- [examples/kinco.template.json](examples/kinco.template.json): generated Kinco provisioning template, not a commissioned model.
- [examples/kinco.template.inputs.md](examples/kinco.template.inputs.md): generated field-specific inputs and conversion warnings; retained beside the template.
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

Successful initialization does not start communication. `start()` is a no-op
when already running; repeated `stop()` calls retain the stopped state but still
attempt session stop/flush and can report cleanup errors. A FAILED runtime cannot
be restarted without uninitialization/reinitialization.

`start()` after `stop()` rebuilds protocol state. A configured node emits boot-up
and waits in NMT Pre-operational for the master; node ID 255 remains
LSS-unconfigured instead. Application values, communication settings and batch
IDs survive stop/start, but stack-owned state does not: for example, the error
register, EMCY history/fault bits and guarding state are reinitialized. Reassert
still-active model faults after restarting. Stack setup applies the pending LSS
node ID and rebases relative IDs; it does not activate a pending bitrate.
Successful stop flushes network queues, so stale frames do not survive.
An NMT Stop command is different: the worker remains running.

Reset Communication restores communication defaults and retains application
values and pending LSS settings. Reset Node restores all OD defaults and reloads
saved/startup LSS settings. Both rebuild services without changing the host
handle or OD structure; batch access resolves current extensions each call.
Successful protocol resets increment separate counters in `get_status()`, so
the external model need not infer reset identity from a transient NMT state.

Successful uninitialize releases sessions, OD, and stack and invalidates
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
| `schema_version` | Value `1`, using the unsigned-number syntax described below |
| `node_id` | `1..127`, or `255` for LSS commissioning |
| `bitrate_kbps` | Whole-number startup bitrate from `1` to `1000` kbit/s; must be supported by the target XNET interface |
| `identity` | `vendor_id`, `product_code`, `revision_number`, `serial_number`, each uint32 |
| `objects` | Array of application VAR/ARRAY/RECORD descriptions |

Optional root fields:

| Field | Default | Range / meaning |
| --- | --- | --- |
| `device_type` | 0 | uint32, generated `0x1000` |
| `heartbeat_ms` | 1000 | uint16 milliseconds; 0 disables heartbeat production |
| `worker_period_us` | 1000 | `100..100000` microseconds of worker wait time after processing; not a fixed cycle deadline |
| `rpdos`, `tpdos` | Empty arrays | Up to eight configured slots per direction |
| `node_guarding`, `sync`, `emcy` | Empty configuration objects | Services use the defaults in [service configuration](#node-guarding-sync-and-emcy), rather than being disabled by omission |

Vendor ID and serial number must be nonzero; use an appropriate unique identity
for each device on the physical network. The example identity is illustrative.

Unknown fields, invalid types, and out-of-range defaults are errors. Input files
are limited to 4 MiB; custom objects to 4096. Index/subindex and unsigned numeric
values, including unsigned configuration fields, may use nonnegative JSON
integers or decimal/lowercase-prefix `0x` strings. Signed application values use
JSON integers; BOOLEAN defaults accept JSON booleans or unsigned values 0/1.
REAL32/64 defaults must be finite JSON numbers and fit the selected type.
Keep 64-bit integers exact when generating JSON; do not round-trip them
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
  explicit `subindex` in `0..254` and value fields. Subindex 0 must be read-only UNSIGNED8
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
output. EDS parsing/conversion validation errors occur before either output is
written. Output file writes themselves are sequential, not atomic: an I/O failure
can leave a truncated/replaced JSON file or a JSON/guide mismatch. Preserve
commissioned models separately from converter output.

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
- `inhibit_100us`: uint16, default 0, used by TPDOs only; omit it for RPDOs,
  where the builder does not use or validate its value.
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
records are SDO-read-only; dynamic PDO remapping remains disabled. Final
resolved-ID collision checks run for configured nodes; with node ID 255 they
are deferred until a configured node ID is applied. Writable
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

Callers built against the earlier descriptor-array/write-enable API must be
rebuilt and their LabVIEW Call Library Function Nodes updated. The current API
has neither a write-enable argument nor `XNET_CO_SKIPPED`.

## TPDO, faults, and errors

`request_tpdo(handle, one_based_number)` requires a running, Operational node.
Requests during LSS bitrate switching are rejected.
Types 254/255 obey inhibit time; type 0 waits for SYNC. Cyclic synchronous types
1..240 reject manual requests. Requests may coalesce and transmit current values.
Acceptance is not a bus-transmission acknowledgment. Channel writes do not
automatically request a TPDO; write and request are separate operations.

`set_fault()` reports/resets simulated manufacturer faults on bits `0x40..0x4F`.
It requires a running, LSS-configured node, but not NMT Operational.
`active` must be 0 or 1; use a nonzero CANopen error code when asserting a fault.
The stack generates EMCY and updates error state/history subject to its NMT and
producer rules; host API failures do not automatically emit EMCY.

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
external device/plant model consistently with application OD defaults; Reset
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

Fatal worker errors move the runtime to FAILED and quiesce sessions. Uninitialize
and initialize again to recover; do not assume a successful earlier start means
the worker is still healthy. Cleanup failures are explicit and retryable.
Nonzero XNET statuses, including warnings, produce diagnostics rather than being
silently accepted. Not every diagnostic stops communication: output queue
overflow retains the pending frame for retry, and input queue overflow flushes
unread input and records the loss. Unsupported incoming frame types/IDs/payloads
are ignored with a diagnostic. Check runtime state as well as the latest error.

## File logging

Set **`XNET_CANOPEN_LOG_PATH`** in
[src/logging_config.h](src/logging_config.h) to the desired absolute file path
and rebuild. A compiler definition of the same macro overrides the header.
There is deliberately no runtime path parameter or JSON logging option.
Default paths are:

- NI Linux RT/Linux: `/var/local/natinst/log/xnet_canopen.log`
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

With `XNET_CANOPEN_BUILD_TESTS=ON` and `XNET_CANOPEN_BUILD_LIBRARY=OFF`, CMake
overrides the integration log path to `xnet_canopen-test.log` in the build
directory. Enabling tests alongside the shared library does not override its
deployment log path. The separate logging-test executable always uses its own
build-directory path. Tests cover node-ID changes without losing the trace
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

Store writes a JSON object with exactly four fields: `schema_version: 1`,
`identity` as `[vendor_id, product_code, revision_number, serial_number]`,
`node_id` and `bitrate_kbps`, using the pending LSS settings.
The identity representation differs from the device model's named object.
A missing file or any exception while loading it uses both model startup
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
The persistence path must contain a parent-directory component, and that
directory must already exist; initialization does not preflight writability.
OD store/restore objects `0x1010/0x1011` are not implemented by this LSS mechanism.

## External device-model integration

The existing batch, status, TPDO-request and fault APIs are the boundary to an
external CiA 402 or other device model. A dedicated drive-profile API is not
needed for ordinary command/feedback exchange. Generate only the objects and
channels required by the agreed modes/tests, not every object found in an EDS.
For each channel, record index/subindex, type, byte size, signedness, raw
units/scaling, ownership, default and reset behavior.

### Ownership and permissions

| Owner | Typical data | Recommended policy |
| --- | --- | --- |
| Master | Commands and network-configured parameters | Use network permissions required by the device; normally `local_write: false`. Never cyclically echo cached commands back into the OD. |
| External model | Feedback, statusword and application alarm values | Normally SDO `ro` with `local_write: true`; update through feedback-write batches. |
| Stack | NMT state, communication timers, error register and protocol state | Use the supported service/status interfaces, not arbitrary model writes to backing memory. |

The EDS converter intentionally emits `local_write: true` for application values
to simplify preparation. Tighten those generated permissions to the selected
ownership contract before deployment. Binding validates existence and size;
read/write permission failures are reported per entry when the batch is used.
Batch reads currently require SDO-readable metadata: there is no independent
local-read permission for a genuinely network-write-only object.

Keep command-read and feedback-write batches separate. Preserve signedness and
exact 64-bit values when converting to VeriStand channels. Physical sensors,
voltage and plant/load properties can go directly to the external model; route
them through the OD only if the real device exposes them there.

If modeled I/O can override network controlword/mode, define priority and local
write permissions explicitly rather than blindly rewriting cached commands.

### Execution and command observability

An ordinary functional loop is:

```text
Initialize from provisioned JSON and bind command/feedback batches
Record reset-counter baselines
At each agreed VeriStand step:
    Read status, command/parameter batch, then status again
    Check operation and per-entry results; handle reset-counter changes
    Execute the external device/plant model using the host elapsed time
    Write feedback and check all per-entry results
    Request affected event-driven TPDOs after successful required writes
    Report fault assertion/clear transitions through set_fault()
```

Follow the [reset sampling rules](#status-snapshot-and-reset-notification);
separate API calls are not a transaction around model execution. The worker does
not run the external model between RPDO application and TPDO packing, so this
loop does not guarantee same-SYNC-cycle feedback.

Latest-value polling is the current scope, with command pacing/handshakes agreed
with the master. Multiple RPDOs may overwrite pending data before application,
and several SDO writes may occur between model reads. A successful read proves
OD access, not freshness or observation of every controlword edge/repeated write.
NMT/guarding status and log records are snapshots/observations, not lossless
transition histories. A sequence counter alone could detect missing writes but
would not preserve overwritten values. If a future feature requires every
command, capture it before coalescing or define a suitable acknowledged protocol.

Writing alarm/statusword objects does not emit EMCY. `set_fault()` does not write
those model-owned objects. Keep internal alarm words, drive error codes, status
bits and CANopen EMCY representations consistent explicitly. Report fault
transitions, not unconditional repeated assertions. If TPDO/fault requests cross
VeriStand channels, use an explicit acknowledgment/sequence scheme to avoid
accidentally repeating actions.

### Extension boundaries, not current requirements

The following are conditional capability limits, not a mandatory implementation
backlog. Add a generic mechanism only when a selected device/test requires it.

| Required behavior, if selected | Why latest-value batches alone are insufficient | Extension direction |
| --- | --- | --- |
| Every command edge or repeated command write | Values can coalesce in RPDO buffers or between host reads. | Capture ordered events before loss, or agree command pacing/acknowledgments. |
| Application-specific SDO validation/abort | By model polling time, an SDO success response may already have been sent. | Validate synchronously through an in-library OD extension; a later model decision cannot retroactively reject an acknowledged write. |
| Device save/default commands | LSS storage covers node ID/bitrate only; a stored command byte has no persistence/reset side effect. | Device adapter defines parameter groups and algorithms; supply reusable command/persistence hooks if required. |
| Vendor-object-triggered bitrate change | Current physical activation is through LSS, not an arbitrary application OD value. | Device adapter interprets vendor encoding; use a controlled generic reconfiguration path if added. |
| Configurable communication-error reaction | Model alarms cannot undo the stack's NMT/PDO/EMCY effects. | Define generic policy and recovery semantics; keep vendor-object interpretation outside the core. |
| Truly network-write-only model input | Batch reads require SDO-readable metadata. | Add independent local-read semantics only for a confirmed need; do not broaden network permissions as a workaround. |
| Model coordination with individual SYNCs | No model-facing SYNC sequence/event/timestamp is exported. | Define observation/latching semantics if interpolation needs them; motion interpolation stays in the model. |

## Kinco configuration and compatibility notes

These notes preserve device-specific findings useful when provisioning or
extending the simulator. They are not a claim that the wrapper reproduces a
complete physical Kinco drive or that all listed options are required.

### Sources and interpretation

- Primary behavior reference: the supplied
  [Kinco drive manual](customer_input/Kinco%20低压伺服驱动器使用手册20260909.pdf),
  particularly chapters 4, 5, 8 and 11 and the conversion appendix.
- Supplied configuration reference:
  [Kinco EDS](eds/Kinco_FD_20250417_V1.2.eds).
- Provisioning artifacts: [template JSON](examples/kinco.template.json) and its
  [generated input guide](examples/kinco.template.inputs.md).

Manual page numbers below mean physical PDF pages (1-based viewer pages), not
printed footers; in the main body the printed page is one less. Converted text,
GUI labels, shorthand overview tables and isolated examples are not substitutes
for detailed object definitions and actual wire behavior. The manual itself
contains conflicting/editorial material. Use the CANopen chapter's FD1x3 slave
baseline; other product-family and EtherCAT/CoE tables do not automatically
establish CANopen requirements. Resolve device behavior against the manual and
selected firmware; use the supplied EDS for provisioning, checking genuine
conflicts against the CANopen specification and agreed device contract.

The current scope uses fixed PDOs and latest-value command polling with agreed
pacing. Vendor parameter save/default behavior, vendor-object bitrate activation,
configurable interruption policy, custom SDO validation and lossless command
queues are deferred, not implied by storing their OD objects.

### Provisioning and fixed PDO layout

The checked-in Kinco template currently needs four commissioning inputs:
`node_id`, `bitrate_kbps`, `worker_period_us` and `identity.serial_number`.
The generated guide gives ranges and EDS-supported bitrates
`50, 125, 250, 500, 1000` kbit/s. These are configuration-reference rates, not a
restriction of the library's startup range or the standard LSS table. Choose
unique identity/node settings, replace every placeholder, and review automatic
defaults and permissions. Regeneration can change the required input list.

The EDS declares eight RPDOs and eight TPDOs. Its nonempty default mappings are:

| PDO / mapping record | Fields in EDS order | Bytes |
| --- | --- | --- |
| RPDO1 / `0x1600` | `0x607A:00` I32 target position; `0x6060:00` I8 mode; `0x6040:00` U16 controlword | 7 |
| RPDO2 / `0x1601` | `0x6081:00` U32 profile velocity; `0x60FF:00` I32 target velocity | 8 |
| TPDO1 / `0x1A00` | `0x6041:00` U16 statusword; `0x6063:00` I32 actual position; `0x6061:00` I8 active mode | 7 |

Other EDS slots have zero default mapped entries and become inactive. Preserve
slot numbering and the agreed payload order. Some manual examples use a
different order or different TPDO fields; they are configuration examples, not
one universal layout. Configure the master and JSON to match exactly. PDO5..8
need explicit IDs even when inactive; the manual's extra-PDO example `0x43F`
is an ordinary 11-bit ID, not evidence that extended frames are necessary.

### Application channel selection

Directions here are relative to the external model. This is a selection guide,
not a mandatory complete OD. Read commands through batch reads and write
feedback through batches with appropriate `local_write` permissions.

| Model inputs | OD addresses / representation | Maintenance considerations |
| --- | --- | --- |
| Controlword / requested mode | `0x6040:00` U16 / `0x6060:00` I8 | Drive-state commands, fault-reset edges and mode selection belong in the model. |
| Position / velocity targets | `0x607A:00` I32 / `0x60FF:00` I32 | Detailed manual table marks these read/write/mappable; overview `W` shorthand is not a confirmed local-read blocker. |
| Profile speed and ramps | `0x6081:00`, `0x6083:00`, `0x6084:00` U32 | Profile velocity is also read/write/mappable in the detailed table; use raw scaled values. |
| Torque/current command and limit | `0x6071:00` I16, `0x60F6:08` I16, `0x6073:00` U16 | Select the actual command path/scaling for the mode; do not reinterpret in transport code. |
| Homing | `0x6098:00`, `0x6099:01/02`, `0x609A:00` U32, `0x607C:00` I32, `0x6099:05` U8 | Select homing method, speeds, acceleration, offset and offset mode as required (manual page 103). |
| Stop behavior | `0x605A..0x605E:00` I16, `0x6085:00` U32 | Model stop-option codes and quick-stop deceleration (pages 107/108). |
| Interpolation | `0x3011:01/02` plus confirmed target object(s) | For CANopen mode 7, period encodings 0/1/2/3 mean 1/2/4/8 ms; clock enable is 0/1. This does not specify the target-buffer/latch contract. |
| Timeout response, limits and I/O configuration | `0x6007:00` and selected manufacturer objects | Define model reactions and I/O priority; `0x6007` does not control library communication policy. |

| Model outputs | OD addresses / representation | Maintenance considerations |
| --- | --- | --- |
| Statusword / active mode | `0x6041:00` U16 / `0x6061:00` I8 | Model generates state/mode bits and accepted mode, not the library. |
| Actual position | `0x6063:00` I32 | Used by the CANopen examples; do not substitute EtherCAT `0x6064` without confirming semantics. |
| Actual velocity / current / digital inputs | `0x606C:00` I32 / `0x6078:00` I16 / `0x60FD:00` U32 | Manual page 100 confirms these. Input bits 0/1/2/3 indicate negative limit, positive limit, home and hardware lock. |
| Alarm words | `0x2601:00`, `0x2602:00` | Select firmware-appropriate bits; conflicting manual tables must not leak into generic transport logic. |
| Optional additional diagnostics | `0x2605:07`, `0x603F:00`, `0x2680:00`, `0x6077:00`, `0x6064:00` | Confirm representation and selected-device meaning before adding them; position entries/error representations are not interchangeable. |

Known manual conversions include
`DEC = rpm * 512 * encoder_resolution / 1875` for velocity and
`DEC = (rps/s) * 65536 * encoder_resolution / 4000000` for acceleration.
Confirm encoder resolution, peak current, position units and command-specific
torque/current conventions in the model/manifest. Do not send unscaled
engineering values merely because a GUI displays rpm or Arms.

### Compatibility hazards and conditional behavior

| Topic | Retained finding / required care | Owning layer |
| --- | --- | --- |
| Operating modes | Page 100 lists `1, 3, 4, -3, -4, 6, 7`; pages 131/132 describe CANopen interpolation mode 7. Select only required modes for the actual variant; EtherCAT CSP/CSV/CST `8/9/10` are not substitutes. | External model / test contract |
| Communication-range application settings | The manual mentions `0x103F` immediate target updating and `0x100B` node settings. Custom JSON objects are restricted to `0x2000..0xBFFF`, so these cannot simply be added as application objects. Confirm CAN-facing requirements and add supported integration/schema handling if selected. Serial save/restart instructions and CAN DIP switches do not establish an LSS-equivalent setting path. | Integration / provisioning |
| PDO inhibit units | Manual pages 123/129 say ms; standard `0x1800:03` and JSON `inhibit_100us` use 100 us. Verify raw values or traces/GUI conversion; do not change stack units based on wording alone. | Provisioning / on-wire contract |
| Transmission/frame variants | Broad manual claims of 29-bit IDs or all transmission types do not change current 11-bit/type 0..240/254/255 support. RTR-triggered TPDO types 252/253 are unsupported; guarding RTR is a different service. | Integration, only if actually required |
| Vendor baud object `0x2F81:00` | Page 127 encoding is `100=1000`, `50=500`, `25=250`, `12=125`, `5=50`, `1=10` kbit/s; default encoding 50 means 500 kbit/s. RS232's 38,400 bit/s setting is unrelated. Current writes only change OD storage, not adapter bitrate. Activation/save timing requires confirmation if implemented. | Optional vendor adapter + generic reconfiguration |
| Save/default `0x2FF0` | Page 106: U8 `:01=1` stores configuration/control parameters, `:01=10` initializes configuration, `:03=1` stores motor parameters. Defaults must subsequently be saved (page 59). A stored byte does none of this; partitions, completion and readback semantics need definition if selected. This is not LSS persistence or generic CiA 402 behavior. | External device adapter/model, with generic hooks if needed |
| Interruption `0x6007` | Pages 128/132 describe 0=no action and 1=error; the current stack still reports life-guard failure and leaves NMT Operational regardless. Ignoring the model alarm cannot restore PDO exchange or undo EMCY. See [policy limitation](#deferred-communication-interruption-policy). | Generic integration policy + vendor interpretation |
| Torque/current scaling | Page 101 mixes percentage-of-rated-torque and current/DEC descriptions for `0x6071`; `0x60F6:08` is a separate current command. Confirm the selected raw convention. | Model / manifest |
| Alarm/status bits | Pages 67/107 disagree on `0x2601` encoder-error bits and bit 14; page 99's statusword bit-4 wording/polarity also needs care. Confirm the actual firmware map and mode-specific high bits. | External model |
| Error representations | `0x2602` bit 12 / internal error `0x8100` is associated with interruption in the manual; it does not establish an EMCY wire code or exact manufacturer-byte layout. Current fault API uses CANopenNode conventions. | Model / device adapter |
| Interpolation observation | Relaxed timing does not define mode-7 buffering, latching or which commands must be observed. Confirm those semantics before adding a SYNC event API. | Model contract / generic event boundary |
| Editorial examples | Page 118's introductory mode-address wording conflicts with the object table; page 134 labels `0x03EB` as 1000 ms, but 1000 is `0x03E8`. Prefer consistent object definitions and standard SDO framing over isolated byte examples. | Provisioning / test expectations |

## Maintaining and extending the integration

### Worker and driver flow

The worker in [src/device.cpp](src/device.cpp) is the sole running owner of frame
dispatch and protocol time advancement. It uses actual monotonic elapsed
microseconds, not an assumed nominal period:

```text
Receive one bounded nonblocking XNET read
    -> validate each frame and match ID/mask/RTR subscriptions
    -> dispatch registered stack callback
    -> service CO_process(..., dt=0) between frames for general protocols
    -> handle reset/bitrate-switch requests before continuing
CO_process(..., elapsed_us)
    -> handle reset, otherwise:
       CO_process_SYNC() -> CO_process_RPDO() -> CO_process_TPDO()
Drain pending adapter output
Wait worker_period_us, releasing the shared OD mutex
```

LSS activation suspends receive/protocol/transmit processing during its required
silence intervals. General-protocol draining between frames does not make RPDO
delivery lossless. The host model executes independently, not inside this flow.
Do not introduce a second unsynchronized stack-processing path from LabVIEW.
Frame reads/writes use zero-timeout XNET calls; persistence is a separate slow
path that releases the OD mutex while retaining lifecycle/worker ownership.
Synchronous logging can still perform disk I/O while the OD mutex is held.

[src/driver.cpp](src/driver.cpp) implements the
[CANopenNode driver interface](../301/CO_driver.h), not SDO/PDO decoding itself:

- `CO_CANrxBufferInit()` records ID/mask/RTR subscriptions; receive dispatch calls
  the matching protocol callback. Internal frame bit `0x800` represents RTR, not
  a twelfth CAN-ID bit. Translate it to/from XNET's separate frame type.
- Validate stream record lengths and types before payload access. Keep eight
  stable, zero-initialized data bytes even for short frames because some stack
  callbacks copy all eight bytes. Ignore transmit echoes.
- `CO_CANsend()` queues a copy, bounded by the stack transmit buffers.
  `transmit()` submits one frame per zero-timeout write; queue-full retains it
  for retry and records overflow. Queue acceptance is not bus delivery.
- `CO_CANmodule_process()` translates hardware CAN state/errors.
  `CO_CANclearPendingSyncPDOs()` can remove pending adapter copies, not frames
  already accepted by XNET. Do not flush unrelated traffic to cancel one PDO.

### Stack allocation and OD lifetimes

[src/model.cpp](src/model.cpp) owns aligned values, VAR/RECORD descriptors, a
sorted `OD_entry_t` list and `CO_config_t`. ARRAY values use independent record
subentries. Finalize storage before publishing pointers to the stack; never
resize backing vectors after initialization. The model must outlive the stack.
See [OD documentation](../doc/objectDictionary.md) and
[generated example OD](../example/OD.c) for stack layout references, not runtime
configuration inputs. The upstream [blank driver](../example/CO_driver_blank.c)
and [main template](../example/main_blank.c) are not working XNET/scheduler code.

Stack setup uses `CO_new()` -> `CO_CANinit()` -> `CO_LSSinit()` and callbacks ->
`CO_CANopenInit()` -> `CO_CANopenInitPDO()`. Normal mode/session startup and
cleanup are coordinated by the wrapper. Check every return and initialization
`errInfo`. `CANptr` consistently points to the device context, never a CAN frame.
`CO_delete()` invalidates stack-owned extensions; reset code detaches/rebuilds
them while retaining model storage. Preserve wrapper-decorated communication
callbacks and resolve fresh OD streams on batch calls, rather than caching stale
extension pointers or bypassing them with direct memory writes.

### PDO mapping internals

Mapping records contain descriptors
`[index:16][subindex:8][bit-length:8]`; descriptors are configuration, not payload.
For example, mapping `0x2000:00` U32 then `0x2001:00` U16 gives descriptor words
`0x20000020`, `0x20010010`. Values `0x12345678` and `0xABCD` produce six bytes:
`78 56 34 12 CD AB`. Mapping order, not numeric OD order, determines offsets.
This port requires whole-value byte widths and does not enable bitwise mapping.

In [CO_PDO.c](../301/CO_PDO.c):

- `CO_TPDO_init()`/`CO_RPDO_init()` use `PDO_initMapping()` and `PDOconfigMap()`
  to find OD entries, validate permissions/lengths and prepare OD I/O.
- `CO_TPDO_process()` decides when to send; internal `CO_TPDOsend()` reads
  mapped values through OD callbacks and submits a packed frame.
- `CO_PDO_receive()` buffers input; `CO_RPDO_process()` later applies it through
  OD writes. That buffer is not an event queue.

The stack's `OD_write_PDO_mapping()`, `OD_write_18xx()` and `OD_write_14xx()`
support dynamic configuration in other ports. This integration deliberately
keeps those records read-only. Adding live PDO configuration is a new behavioral
contract, not just changing backing bytes or enabling a flag; validation,
extension updates, reset defaults, master expectations and tests must agree.

### Compile-time configuration

All C stack sources and C++ wrapper sources must use identical feature settings:
macros change structure layouts and API availability. The integration CMake
object target supplies the shared include paths. Put port configuration in
[src/CO_driver_target.h](src/CO_driver_target.h), not scattered per-source defines.

| Layer | Current choice / rule |
| --- | --- |
| Allocation | `CO_MULTIPLE_OD` supplies per-handle `CO_config_t`/OD. Leave `CO_USE_GLOBALS` undefined; defining it as zero still selects `#ifdef` branches. |
| Capability masks | Combine feature bits with `\|`; compiled features do not by themselves create service instances or OD entries. |
| Counts and OD pointers | `Model::configure()` connects service counts/OD entries to allocation. Keep counts, compiled sources and descriptors consistent; do not edit internal `CO_RX_CNT_*`/`CO_TX_CNT_*` index calculations. |
| Runtime values | JSON/SDO communication values configure enabled services; they do not enable a compiled-out service. |
| Supported byte order | Little-endian storage and targets only; do not remove the target check without implementing scalar/wire conversion. |

Current masks enable the NMT/heartbeat base, node-guarding slave, segmented SDO
server, EMCY producer/history/inhibit/configuration, SYNC consumer with live OD
handling, both PDO directions with timers/SYNC/OD I/O, and LSS slave.
SDO client, heartbeat consumer, guarding master, SYNC producer, TIME, LEDs,
gateway, GFC, SRDO, trace and general OD storage are disabled.

Important macro traps:

- `CO_CONFIG_NMT = 0` retains base NMT/heartbeat; `CO_CONFIG_SDO_SRV = 0`
  would retain expedited SDO, not remove the server.
- `CO_CONFIG_FLAG_OD_DYNAMIC` enables service-specific live OD handling. Global
  dynamic handling is off here; SYNC explicitly opts in while PDO does not.
  Do not enable it globally to add a single service callback.
- Callback-pre flags enable wake-up callbacks, not a worker by themselves.
  Timer-next flags calculate deadlines, not whether protocol timers exist.
- [CO_config.h](../301/CO_config.h) documents flags, but many illustrated masks
  are under `CO_DOXYGEN`; actual fallback masks live in protocol headers.
  Do not define `CO_DOXYGEN` in an application build.
- LSS persistence uses its store callback and does not require
  `CO_CONFIG_STORAGE` or implement general OD store/restore.

The numbered upstream folders identify CiA specification areas, not OD address
ranges: `301` is core communication, `303` includes LED recommendations, `304`
is CANopen Safety, `305` is LSS, and `309` includes the ASCII gateway. Adding an
upstream module still requires integration configuration, OD entries and processing.

### Change checklist

1. Decide whether the feature belongs in provisioning, the external device
   model, a generic model-facing hook, or the communication/driver layer.
   Reuse CANopenNode services and OD extensions rather than reimplementing protocols.
2. For a schema/type/service change, update validation, aligned storage/defaults,
   descriptors, service counts, feature masks and CMake sources as applicable.
   Wire EDS conversion and generated input guidance when the field is importable.
3. Define behavior for startup, host stop/start, both protocol reset types, LSS
   node changes, uninitialize/reinitialize and failed cleanup. Rebase relative
   COB-IDs from model defaults; do not preserve IDs for an old node by accident.
4. Preserve shared locking, extension callbacks and worker ownership. Never free
   state while a worker/store operation can still use it. Expose errors through
   operation/per-entry results and diagnostics rather than success-shaped defaults.
5. For ABI changes, update the public header, C ABI assertions and LabVIEW
   descriptors/call nodes together. Schema-version-1 optional fields can still
   require a newer runtime; older binaries reject unknown keys.
6. Add focused fake-XNET tests using the real stack, plus converter tests for
   provisioning changes. Exercise bounds, permissions, callback failures, reset
   retention, relative IDs/collisions and failure/retry paths relevant to the change.
7. Update this guide and perform target acceptance for hardware-dependent
   behavior. Mock success cannot establish bus timing, physical bitrate changes,
   installed XNET compatibility or filesystem durability.

## Validation and limits

Automated groups exercise model validation and numeric limits, batch
permissions/partial errors/string buffers, concurrent callers, caller-managed resources and cleanup,
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

Use Windows/MSVC or Linux x86-64/GCC mock builds/tests for development, with
ASan/UBSan available on Linux. These do not establish NI Linux RT ARM64
hardware acceptance.

Important limits:

- No CiA 402 state machine, motion model, in-library EDS loader, or VeriStand UI is supplied.
  The standalone EDS-to-JSON template converter is supplied.
- Workers use normal inherited OS scheduling; no hard real-time guarantee.
- Receive work is bounded to one 8 KiB read per iteration. Output writes one frame
  at a time with timeout zero, avoiding ambiguous multi-frame partial writes.
- Adapter output is bounded to one pending copy per stack transmit buffer.
  Queue overflow is reported, not hidden; XNET queue-full frames are retried.
- Late synchronous frames still in the adapter queue can be removed. Frames
  already handed to XNET cannot be selectively cancelled through `nxFlush`.
  No strict SYNC-window guarantee is made; session-wide flushing is reserved
  for stop/reset/bitrate switching, not cancelling one PDO.
- No wrapper-controlled bus-off restart/recovery or FAILED-runtime recovery is
  implemented. Physical warning/passive/bus-off flags are recomputed from XNET
  state and can clear when hardware recovers, but this does not automatically
  return NMT to Operational or restart a FAILED runtime.

Manual acceptance on the target should cover same-port stream input/output,
boot-up and heartbeat, expedited/segmented SDO, fixed RPDO/TPDO in appropriate
NMT states, SYNC, resets, EMCY, complete LSS commissioning including bitrate
switching, guarding RTR/toggle/timeout behavior when selected, writable SYNC/EMCY
configuration, and file reload after restarting the process. Verify queue behavior,
physical timing, installed XNET ABI compatibility, and your filesystem's
durability semantics.

For a minimal first hardware check, use correct termination/wiring, matching
bitrate and distinct node IDs. With simulator node 10, start the worker, verify
boot-up/heartbeat on `0x70A`, then read vendor ID `0x1018:01` through
`0x60A`/`0x58A`. Put the node Operational before checking a known fixed PDO
payload. Expand to RPDO commands and external-model feedback, command pacing,
fault/clear behavior and both reset paths. Do not treat successful SDO or NMT
Operational as proof of implemented CiA 402 behavior.
