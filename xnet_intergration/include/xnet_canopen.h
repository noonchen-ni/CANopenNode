#ifndef XNET_CANOPEN_H
#define XNET_CANOPEN_H

#include <stdint.h>

#if defined(_WIN32)
#define XNET_CANOPEN_API __declspec(dllexport)
#else
#define XNET_CANOPEN_API __attribute__((visibility("default")))
#endif

#ifdef __cplusplus
extern "C" {
#endif

/**
 * Opaque device owned by the caller until a successful destroy.
 * Do not use stale handles or overlap destroy with any other call.
 */
typedef struct xnet_canopen_device* xnet_canopen_handle;

/** Operation results and per-entry batch results; these are not runtime states. */
enum {
    XNET_CO_OK = 0,                 /**< Operation or batch entry completed successfully. */
    XNET_CO_INVALID_ARGUMENT = -1,  /**< Invalid pointer, count, value, or buffer layout. */
    XNET_CO_INVALID_STATE = -2,     /**< Lifecycle/NMT state forbids the operation. */
    XNET_CO_MODEL_ERROR = -3,       /**< JSON schema, OD description, or configured value is invalid. */
    XNET_CO_IO_ERROR = -4,          /**< File access, persistence, or cleanup failed. */
    XNET_CO_XNET_ERROR = -5,        /**< XNET call or adapter communication failed; inspect the message. */
    XNET_CO_STACK_ERROR = -6,       /**< CANopen initialization or protocol processing failed. */
    XNET_CO_NOT_FOUND = -7,         /**< OD variable or batch ID does not exist. */
    XNET_CO_ACCESS_ERROR = -8,      /**< OD permission, value validation, or callback transfer failed. */
    XNET_CO_BUFFER_TOO_SMALL = -9,  /**< Batch or diagnostic output buffer lacks capacity. */
    XNET_CO_PARTIAL_FAILURE = -10,  /**< At least one batch entry failed; inspect every entry result. */
    XNET_CO_INTERNAL_ERROR = -11   /**< Allocation or another unexpected internal operation failed. */
};

/** Values of xnet_canopen_status_t.runtime_state; independent of CANopen NMT state. */
enum {
    XNET_CO_RUNTIME_UNINITIALIZED = 0, /**< Context exists without an initialized device. */
    XNET_CO_RUNTIME_STOPPED = 1,       /**< Initialized resources are retained; worker is not running. */
    XNET_CO_RUNTIME_RUNNING = 2,       /**< Worker owns processing; node may still await LSS/NMT. */
    XNET_CO_RUNTIME_FAILED = 3         /**< Runtime/setup failed; uninitialize before reinitializing. */
};

/** Synchronized 32-byte status snapshot; fields other than runtime_state are zero before initialization. */
typedef struct {
    uint32_t runtime_state;        /**< One of XNET_CO_RUNTIME_*, not an API result. */
    uint32_t nmt_state;            /**< CANopen NMT state value; 0 while initializing/unconfigured. */
    uint32_t active_node_id;       /**< Current node ID, or 255 while awaiting LSS assignment. */
    uint32_t pending_node_id;      /**< LSS-configured node ID, applied at communication reset. */
    uint32_t active_bitrate_kbps;  /**< Current physical interface bitrate in kbit/s. */
    uint32_t pending_bitrate_kbps; /**< LSS-configured bitrate, applied by activation. */
    uint32_t can_error_status;     /**< CANopenNode CO_CAN_ERR_* bitmask, not an API result. */
    uint32_t reserved;             /**< Always zero. */
} xnet_canopen_status_t;

/**
 * Allocate an uninitialized context and write its handle to a required output.
 * Sets *handle to NULL on allocation failure. No hardware resources are acquired.
 * All APIs return XNET_CO_* operation results; negative values indicate failure.
 */
XNET_CANOPEN_API int32_t xnet_canopen_create(xnet_canopen_handle* handle);

/**
 * Stop and release all resources, then invalidate the handle on success.
 * Caller must prevent concurrent/new calls. On failure the handle remains valid
 * for diagnostics and a cleanup retry. NULL is an invalid argument.
 */
XNET_CANOPEN_API int32_t xnet_canopen_destroy(xnet_canopen_handle handle);

/**
 * Open an interface and construct the OD/stack from a version-1 JSON model.
 * Requires an uninitialized handle and nonempty interface/model/persistence paths.
 * Strings are copied without normalization; caller ensures correct names/paths
 * and exclusive interface/persistence-file use. Missing or unloadable persistence
 * uses model startup settings; load exceptions are recorded in diagnostics
 * without failing initialization. Sessions are not started.
 * Failure releases partial resources when possible; inspect diagnostics and
 * retry uninitialize if cleanup also fails. See README.md for the JSON schema.
 */
XNET_CANOPEN_API int32_t xnet_canopen_initialize(
    xnet_canopen_handle handle, const char* interface_name,
    const char* model_path, const char* persistence_path);

/**
 * Stop/join the worker and release sessions, stack, OD, and batch IDs.
 * Retains the context and diagnostics for another initialize. Also succeeds for
 * an already uninitialized context; cleanup failure is reported and retryable.
 */
XNET_CANOPEN_API int32_t xnet_canopen_uninitialize(xnet_canopen_handle handle);

/**
 * Start/restart a stopped initialized device; already running is a no-op.
 * Rebuilds protocol state while retaining OD values/bindings. A configured node
 * emits boot-up and starts Pre-operational, not NMT Operational. Inspect status
 * for later asynchronous failures; a FAILED runtime must first be uninitialized.
 */
XNET_CANOPEN_API int32_t xnet_canopen_start(xnet_canopen_handle handle);

/**
 * Stop/join processing and discard pending network traffic, retaining OD values,
 * configuration, bindings, and sessions. Already stopped is a no-op.
 * Requires initialization. This is not CANopen NMT Stop, which keeps processing.
 */
XNET_CANOPEN_API int32_t xnet_canopen_stop(xnet_canopen_handle handle);

/**
 * Bind 1..4096 distinct OD variables using three required numeric arrays.
 * indices, subindices, and sizes each contain count elements; sizes must match
 * exact OD lengths, including full string slots. The library copies the values
 * and returns a batch ID through the required batch_id output.
 * Buffer slots follow array order, tightly packed without padding: offset[i] is
 * the sum of preceding sizes. Total size must be <=1 MiB.
 * IDs survive stop/start and master resets, but not uninitialize. Prefer binding
 * during setup; no caller array pointer is retained. In LabVIEW, pass numeric
 * arrays as Array Data Pointer, not array handles; verify their lengths first.
 */
XNET_CANOPEN_API int32_t xnet_canopen_bind_batch(
    xnet_canopen_handle handle, const uint16_t* indices,
    const uint8_t* subindices, const uint32_t* sizes, uint32_t count,
    uint32_t* batch_id);

/**
 * Read a bound group while initialized and not FAILED.
 * buffer_size is byte capacity (at least the sum of bound sizes). entry_status
 * is a required caller-allocated int32 output array with entry_count elements;
 * entry_count must equal the bound variable count, not the byte-buffer size.
 * entry_status[i] reports the result for variable i in binding order.
 * Call-wide errors leave entry_status unchanged. Numeric values are little-endian;
 * string slots have fixed capacity. Only successful entry results identify valid
 * output. Entry failures do not stop later entries; overall result is
 * XNET_CO_PARTIAL_FAILURE if any entry fails. OD callbacks run under the OD lock.
 */
XNET_CANOPEN_API int32_t xnet_canopen_read_batch(
    xnet_canopen_handle handle, uint32_t batch_id, uint8_t* buffer,
    uint32_t buffer_size, int32_t* entry_status, uint32_t entry_count);

/**
 * Attempt every bound entry while initialized and not FAILED.
 * Buffer/count/status rules match read_batch. The caller decides when to invoke
 * a write; there is no enable mask. Bind separate groups for independent writes.
 * Values need model local_write permission; VISIBLE_STRING slots require NUL.
 * Call-wide validation precedes writes. Entry failures allow later writes and
 * return XNET_CO_PARTIAL_FAILURE, without rollback of earlier successes.
 * Serialized writes are last-successful-write-wins; no TPDO is requested here.
 */
XNET_CANOPEN_API int32_t xnet_canopen_write_batch(
    xnet_canopen_handle handle, uint32_t batch_id, const uint8_t* buffer,
    uint32_t buffer_size, int32_t* entry_status, uint32_t entry_count);

/**
 * Request one enabled TPDO by one-based number on a running Operational node.
 * Types 254/255 obey inhibit time; type 0 waits for SYNC. Other types and requests
 * during bitrate switching are rejected. Requests may coalesce; success means
 * accepted by the stack, not transmitted/acknowledged on the physical bus.
 */
XNET_CANOPEN_API int32_t xnet_canopen_request_tpdo(
    xnet_canopen_handle handle, uint16_t tpdo_number);

/**
 * Assert/reset a simulated manufacturer fault on a running, configured node.
 * error_bit must be 0x40..0x4F and active must be 0 or 1. Asserting requires a
 * nonzero CANopen error_code; reset uses the no-error code. info_code is the
 * manufacturer diagnostic payload. The stack updates EMCY/error history; this
 * is separate from host API diagnostics and does not bypass NMT transmission rules.
 */
XNET_CANOPEN_API int32_t xnet_canopen_set_fault(
    xnet_canopen_handle handle, uint8_t error_bit, uint16_t error_code,
    uint32_t info_code, uint8_t active);

/** Copy a synchronized status snapshot to a required output, even before initialization or after failure. */
XNET_CANOPEN_API int32_t xnet_canopen_get_status(
    xnet_canopen_handle handle, xnet_canopen_status_t* status);

/**
 * Copy the latest per-handle diagnostic without clearing or replacing it.
 * error_code and message_required are required outputs. message_required includes
 * NUL; NULL message with capacity 0 queries size and returns BUFFER_TOO_SMALL.
 * Code/required size are returned even when capacity is insufficient; no partial
 * message is copied. Messages are capped at 2047 bytes plus NUL. Successful
 * unrelated calls do not erase failures; diagnostics survive stop/uninitialize.
 * The returned operation result is distinct from the retrieved error_code.
 */
XNET_CANOPEN_API int32_t xnet_canopen_get_last_error(
    xnet_canopen_handle handle, int32_t* error_code, char* message,
    uint32_t message_capacity, uint32_t* message_required);

/** Clear only the latest host diagnostic; does not reset runtime state, CAN flags, or simulated faults. */
XNET_CANOPEN_API int32_t xnet_canopen_clear_error(xnet_canopen_handle handle);

#ifdef __cplusplus
}
#endif
#endif
