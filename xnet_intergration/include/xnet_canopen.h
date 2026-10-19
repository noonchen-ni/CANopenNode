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

typedef struct xnet_canopen_device* xnet_canopen_handle;

enum {
    XNET_CO_OK = 0,
    XNET_CO_SKIPPED = 1,
    XNET_CO_INVALID_ARGUMENT = -1,
    XNET_CO_INVALID_STATE = -2,
    XNET_CO_MODEL_ERROR = -3,
    XNET_CO_IO_ERROR = -4,
    XNET_CO_XNET_ERROR = -5,
    XNET_CO_STACK_ERROR = -6,
    XNET_CO_NOT_FOUND = -7,
    XNET_CO_ACCESS_ERROR = -8,
    XNET_CO_BUFFER_TOO_SMALL = -9,
    XNET_CO_PARTIAL_FAILURE = -10,
    XNET_CO_INTERNAL_ERROR = -11
};

enum {
    XNET_CO_UNINITIALIZED = 0,
    XNET_CO_STOPPED = 1,
    XNET_CO_RUNNING = 2,
    XNET_CO_FAILED = 3
};

typedef struct {
    uint16_t index;
    uint8_t subindex;
    uint8_t reserved;
    uint32_t offset;
    uint32_t size;
} xnet_canopen_binding_t;

typedef struct {
    uint32_t runtime_state;
    uint32_t nmt_state;
    uint32_t active_node_id;
    uint32_t pending_node_id;
    uint32_t active_bitrate_kbps;
    uint32_t pending_bitrate_kbps;
    uint32_t can_error_status;
    uint32_t reserved;
} xnet_canopen_status_t;

XNET_CANOPEN_API int32_t xnet_canopen_create(xnet_canopen_handle* handle);
XNET_CANOPEN_API int32_t xnet_canopen_destroy(xnet_canopen_handle handle);
XNET_CANOPEN_API int32_t xnet_canopen_initialize(
    xnet_canopen_handle handle, const char* interface_name,
    const char* model_path, const char* persistence_path);
XNET_CANOPEN_API int32_t xnet_canopen_uninitialize(xnet_canopen_handle handle);
XNET_CANOPEN_API int32_t xnet_canopen_start(xnet_canopen_handle handle);
XNET_CANOPEN_API int32_t xnet_canopen_stop(xnet_canopen_handle handle);
XNET_CANOPEN_API int32_t xnet_canopen_bind_batch(
    xnet_canopen_handle handle, const xnet_canopen_binding_t* bindings,
    uint32_t count, uint32_t* batch_id);
XNET_CANOPEN_API int32_t xnet_canopen_read_batch(
    xnet_canopen_handle handle, uint32_t batch_id, uint8_t* buffer,
    uint32_t buffer_size, int32_t* entry_status, uint32_t entry_count);
XNET_CANOPEN_API int32_t xnet_canopen_write_batch(
    xnet_canopen_handle handle, uint32_t batch_id, const uint8_t* buffer,
    uint32_t buffer_size, const uint8_t* enable,
    int32_t* entry_status, uint32_t entry_count);
XNET_CANOPEN_API int32_t xnet_canopen_request_tpdo(
    xnet_canopen_handle handle, uint16_t tpdo_number);
/* Bits 0x40..0x4F are reserved here for simulated manufacturer faults. */
XNET_CANOPEN_API int32_t xnet_canopen_set_fault(
    xnet_canopen_handle handle, uint8_t error_bit, uint16_t error_code,
    uint32_t info_code, uint8_t active);
XNET_CANOPEN_API int32_t xnet_canopen_get_status(
    xnet_canopen_handle handle, xnet_canopen_status_t* status);
/* message_required includes NUL; a NULL message with capacity 0 queries size. */
XNET_CANOPEN_API int32_t xnet_canopen_get_last_error(
    xnet_canopen_handle handle, int32_t* error_code, char* message,
    uint32_t message_capacity, uint32_t* message_required);
XNET_CANOPEN_API int32_t xnet_canopen_clear_error(xnet_canopen_handle handle);

#ifdef __cplusplus
}
#endif
#endif
