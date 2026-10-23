#ifndef XNET_CO_INTERNAL_HPP
#define XNET_CO_INTERNAL_HPP

#define OD_DEFINITION
#include "CANopen.h"
#include "xnet_canopen.h"
#include "nlohmann/json.hpp"
#include "xnet/nixnet.h"

#include <array>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <deque>
#include <filesystem>
#include <map>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

namespace xco {
using Json = nlohmann::json;
using Clock = std::chrono::steady_clock;

/** Internal exception carrying a public API result code and a diagnostic message. */
struct Error : std::runtime_error {
    int32_t code; /**< XNET_CO_* failure code; what() supplies the associated message. */
    Error(int32_t c, const std::string& text) : std::runtime_error(text), code(c) {}
};
void require(bool condition, int32_t code, const std::string& message);
/** Numeric range for configured Classical CAN rates in kbit/s; XNET checks hardware support. */
bool valid_bitrate(uint16_t kbps);
uint64_t unsigned_number(const Json& value, uint64_t maximum, const std::string& where);
Json read_json(const std::filesystem::path& path);
void write_json_atomic(const std::filesystem::path& path, const Json& value);

/**
 * One Object Dictionary (OD) subentry: a scalar or a fixed-size string/byte buffer.
 * A VAR object has one Value at subindex 0; compound objects have multiple Values.
 * Owns the live bytes exposed to CANopenNode and an immutable default-byte snapshot.
 */
struct Value {
    /** Live storage rounded up to uint64_t words for typed OD pointer alignment. */
    std::vector<uint64_t> storage;
    std::vector<uint8_t> initial; /**< Exactly size default bytes, used for resets and COB-ID rebasing. */
    std::string type; /**< Model type name, e.g. UNSIGNED16, REAL32, or VISIBLE_STRING. */
    uint32_t size = 0; /**< Logical byte length, excluding storage padding; includes string NUL capacity. */
    uint8_t sub = 0; /**< Subindex within the containing OD object. */
    OD_attr_t attr = 0; /**< CANopenNode access, PDO mapping, string, and multibyte attribute flags. */
    bool allows_local_write = false; /**< Allows host batch writes; independent of network SDO write permission. */
    bool is_node_relative = false; /**< Live uint32_t COB-ID is its default base plus node ID (unless unconfigured). */
    /** Address published to OD descriptors; stable while storage is not resized. */
    void* data() { return storage.data(); }
};

/**
 * Owns all subentries and CANopenNode descriptors for one 16-bit OD index.
 * JSON ARRAY and RECORD objects both use record descriptors with per-subentry
 * attributes and storage; a scalar VAR instead uses the single var descriptor.
 * A descriptor is non-owning metadata: a pointer to live bytes plus their access
 * flags and length (and subindex for a record). It does not duplicate the value.
 * CANopenNode reaches values[0].storage through var.dataOrig for a VAR, or each
 * Value's storage through the corresponding record descriptor for a REC.
 */
struct Object {
    uint16_t index = 0; /**< OD index shared by all values in this object. */
    bool is_var_object = false; /**< True for one subentry at index:0 (ODT_VAR), false for ODT_REC; not mutability. */
    std::vector<Value> values; /**< Owning subentry storage, finalized before publishing descriptors. */
    OD_obj_var_t var{}; /**< Non-owning view of values[0]: data pointer, access flags, and byte length; VAR only. */
    std::vector<OD_obj_record_t> record; /**< Compound descriptors pointing into values; used only for REC. */
};

/**
 * Runtime OD built from a validated JSON model and generated communication objects.
 * Owns the storage referenced by OD descriptors and the stack configuration.
 * These objects must outlive the stack; published descriptor/storage addresses
 * must remain stable until the stack is deleted.
 */
struct Model {
    std::vector<std::unique_ptr<Object>> objects; /**< Owning objects, sorted by index during load. */
    std::vector<OD_entry_t> entries; /**< Sorted stack-facing entries plus a trailing empty sentinel. */
    OD_t od{}; /**< Non-owning dictionary view of entries, excluding the sentinel from its count. */
    CO_config_t config{}; /**< Service counts and OD entry pointers used to allocate/initialize the stack. */
    std::array<uint32_t, 4> identity{}; /**< Vendor ID, product code, revision number, serial number (LSS/0x1018). */
    uint8_t startup_node = 10; /**< JSON startup node ID: 1..127, or 255 for LSS-unconfigured. */
    uint16_t startup_bitrate = 125; /**< JSON startup bitrate in kbit/s, before persistence overrides. */
    uint16_t rpdo_count = 0, tpdo_count = 0; /**< Configured receive/transmit PDO counts, respectively. */
    uint32_t worker_period_us = 1000; /**< Worker wait interval in microseconds; not a hard real-time guarantee. */
    void load(const Json& root);
    void restore(bool reset_all_objects, uint8_t node);
    void resolve_node(uint8_t node);
    Value& value(uint16_t index, uint8_t sub);
    void configure();
};

/**
 * One OD subentry's slot in a host batch buffer.
 * Stores an address by index/subindex, not an OD stream pointer: stack resets
 * replace extensions, so each batch operation resolves a fresh OD stream.
 */
struct Binding {
    uint16_t index; /**< Target OD index. */
    uint8_t subindex; /**< Target OD subindex. */
    uint32_t offset; /**< Byte offset from the start of the caller's packed batch buffer. */
    uint32_t size; /**< Slot length in bytes; must equal the target Value's logical size. */
};

/** Ordered host read/write layout; slots are byte-packed without alignment gaps. */
struct Batch {
    std::vector<Binding> bindings; /**< Slot order also determines per-entry status-array order. */
    uint32_t size = 0; /**< Total required buffer length in bytes (sum of binding sizes). */
};

/**
 * Owns a device's two raw NI-XNET frame-stream sessions on one physical interface.
 * No CAN database is used. Session references are released explicitly by close();
 * failed releases retain their references so device cleanup can be retried.
 */
struct Adapter {
    nxSessionRef_t input = 0, output = 0; /**< Input/output stream references, respectively; 0 means none owned. */
    bool sessions_started = false; /**< Both starts completed; cleared by stop/close, not a live hardware-state query. */
    void open(const std::string& name, uint16_t bitrate);
    void bitrate(uint16_t kbps);
    void start();
    void stop_flush();
    void close();
    void check(nxStatus_t status, const char* operation);
};
} // namespace xco

/**
 * Per-device context behind the opaque public xnet_canopen_handle.
 * Owns the model, stack, XNET sessions, worker, batches, and diagnostics.
 * Public wrapped calls acquire lifecycle before mutex; the worker only acquires
 * mutex. Stop/join releases mutex so the worker can finish without deadlocking.
 * The caller must prevent concurrent calls during destruction and enforce
 * interface/persistence-path exclusivity outside this library.
 */
struct xnet_canopen_device {
    std::mutex lifecycle; /**< Serializes wrapped API calls, including while mutex is released for joins. */
    std::recursive_mutex mutex; /**< Protects OD/runtime state; recursive for stack callbacks into the driver. */
    std::condition_variable_any wake; /**< Interrupts the worker's timed wait when shutdown is requested. */
    std::thread worker; /**< Protocol/frame-processing thread; joined before releasing its resources. */
    std::atomic<bool> stop_requested{false}; /**< Shutdown request visible even when the worker releases mutex. */
    uint32_t state = XNET_CO_RUNTIME_UNINITIALIZED; /**< XNET_CO_RUNTIME_* lifecycle state, not NMT state. */
    int32_t error_code = 0; /**< Latest per-handle XNET_CO_* diagnostic code; 0 when cleared. */
    std::array<char, 2048> error_message{}; /**< NUL-terminated diagnostic text, truncated to fit. */
    std::string interface_name; /**< Caller-supplied XNET interface name, copied without normalization. */
    std::filesystem::path persistence_path; /**< Caller-supplied LSS JSON file path, not canonicalized. */
    std::unique_ptr<xco::Model> model; /**< Owning runtime OD/model; null when uninitialized. */
    xco::Adapter adapter; /**< Sessions used exclusively by this context's frame I/O. */
    CO_t* co = nullptr; /**< Owning CANopenNode allocation, released with CO_delete; may be replaced on reset. */
    uint8_t pending_node = 10, active_node = 10; /**< LSS-selected and stack-applied node IDs; 255 means unconfigured. */
    uint16_t pending_bitrate = 125, active_bitrate = 125; /**< Requested and applied physical bitrates in kbit/s. */
    uint32_t next_batch = 1; /**< Next batch ID; never reused within this handle, with 0 marking exhaustion. */
    std::map<uint32_t, xco::Batch> batches; /**< Batch ID to layout; cleared on uninitialize, retained across resets. */
    /** Immutable queued frame snapshot paired with its originating stack transmit buffer. */
    struct Pending {
        CO_CANtx_t* buffer; /**< Non-owning stack pointer; bufferFull tracks this frame until sent/cancelled. */
        CO_CANrxMsg_t frame; /**< Copied identifier, DLC, and payload, immune to later transmit-buffer edits. */
    };
    std::deque<Pending> outgoing; /**< FIFO with at most one pending copy per stack transmit buffer. */
    /**
     * Non-owning pointer to run()'s outer OD lock; null outside worker execution.
     * Worker callbacks use it to release/reacquire mutex around persistence I/O.
     */
    std::unique_lock<std::recursive_mutex>* worker_lock = nullptr;
    /** LSS bitrate activation phases; protocol/frame I/O is suspended in both silence intervals. */
    enum class Switch {
        none,   /**< Normal protocol processing and frame I/O. */
        before, /**< Sessions quiesced; wait before changing the physical bitrate. */
        after   /**< Bitrate changed; wait again before restarting sessions. */
    };
    Switch switching = Switch::none; /**< Current LSS bitrate activation phase. */
    xco::Clock::time_point switch_deadline{}; /**< Monotonic deadline for the current silence interval. */
    uint16_t switch_delay_ms = 0; /**< LSS-requested delay in milliseconds, applied before and after the change. */

    void fail(int32_t code, std::string_view message) noexcept;
    void initialized() const;
    void load_persistent();
    void setup_stack();
    void reset_stack(bool reset_all_objects);
    void run();
    void receive();
    void transmit();
    void stop();
    void release();
    static bool_t check_bitrate(void*, uint16_t rate);
    static void activate_bitrate(void*, uint16_t delay);
    static bool_t store(void*, uint8_t node, uint16_t bitrate);
};

#endif
