#include "xnet_canopen.h"
#include <stddef.h>

typedef char binding_size_must_be_12[(sizeof(xnet_canopen_binding_t) == 12) ? 1 : -1];
typedef char offset_field_must_be_4[(offsetof(xnet_canopen_binding_t, offset) == 4) ? 1 : -1];
typedef char status_size_must_be_32[(sizeof(xnet_canopen_status_t) == 32) ? 1 : -1];

int xnet_canopen_c_header_test(void) {
    return xnet_canopen_stop(NULL) == XNET_CO_INVALID_ARGUMENT;
}
