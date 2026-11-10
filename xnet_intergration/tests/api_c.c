#include "xnet_canopen.h"
#include <stddef.h>

typedef char status_size_must_be_48[(sizeof(xnet_canopen_status_t) == 48) ? 1 : -1];
typedef char guarding_status_offset_must_be_28[(offsetof(xnet_canopen_status_t, node_guarding_timeout) == 28) ? 1 : -1];
typedef char node_reset_offset_must_be_32[(offsetof(xnet_canopen_status_t, reset_node_count) == 32) ? 1 : -1];
typedef char communication_reset_offset_must_be_36[
    (offsetof(xnet_canopen_status_t, reset_communication_count) == 36) ? 1 : -1];
typedef char can_state_raw_offset_must_be_40[(offsetof(xnet_canopen_status_t, can_state_raw) == 40) ? 1 : -1];
typedef char can_state_valid_offset_must_be_44[(offsetof(xnet_canopen_status_t, can_state_valid) == 44) ? 1 : -1];
typedef char runtime_values_must_be_stable[
    (XNET_CO_RUNTIME_UNINITIALIZED == 0 && XNET_CO_RUNTIME_STOPPED == 1
     && XNET_CO_RUNTIME_RUNNING == 2 && XNET_CO_RUNTIME_FAILED == 3) ? 1 : -1];

int xnet_canopen_c_header_test(void) {
    const uint16_t indices[] = {0x2000};
    const uint8_t subindices[] = {0};
    uint32_t sizes[1], types[1], buffer_size;
    uint32_t batch;
    uint8_t buffer[2] = {0};
    int32_t status;
    uint32_t value_size;
    return xnet_canopen_bind_batch(NULL, indices, subindices, 1, types, sizes, &buffer_size, &batch) == XNET_CO_INVALID_ARGUMENT
        && xnet_canopen_read_batch(NULL, 0, buffer, sizeof(buffer), &status, 1) == XNET_CO_INVALID_ARGUMENT
        && xnet_canopen_write_batch(NULL, 0, buffer, sizeof(buffer), &status, 1) == XNET_CO_INVALID_ARGUMENT
        && xnet_canopen_read_od(NULL, 0x2000, 0, buffer, sizeof(buffer), &value_size) == XNET_CO_INVALID_ARGUMENT
        && xnet_canopen_write_od(NULL, 0x2000, 0, buffer, sizeof(buffer)) == XNET_CO_INVALID_ARGUMENT
        && xnet_canopen_stop(NULL) == XNET_CO_INVALID_ARGUMENT;
}
