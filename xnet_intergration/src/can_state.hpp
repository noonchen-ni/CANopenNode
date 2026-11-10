#ifndef XNET_CO_CAN_STATE_HPP
#define XNET_CO_CAN_STATE_HPP

#include "xnet/nixnet.h"

namespace xco {
inline const char* can_communication_name(u32 state) {
    switch (nxCANComm_Get_CommState(state)) {
        case nxCANCommState_ErrorActive: return "error-active";
        case nxCANCommState_ErrorPassive: return "error-passive";
        case nxCANCommState_BusOff: return "bus-off";
        case nxCANCommState_Init: return "initializing";
        default: return "unknown";
    }
}

inline const char* can_last_error_name(u32 state) {
    switch (nxCANComm_Get_LastErr(state)) {
        case nxCANLastErr_None: return "none";
        case nxCANLastErr_Stuff: return "stuff";
        case nxCANLastErr_Form: return "form";
        case nxCANLastErr_Ack: return "ACK";
        case nxCANLastErr_Bit1: return "bit-1";
        case nxCANLastErr_Bit0: return "bit-0";
        case nxCANLastErr_CRC: return "CRC";
        default: return "unknown";
    }
}
} // namespace xco

#endif
