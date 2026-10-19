#ifndef XNET_CO_FAKE_XNET_HPP
#define XNET_CO_FAKE_XNET_HPP
#include "internal.hpp"

namespace fake {
struct Event {
    std::string operation;
    uint64_t value;
    xco::Clock::time_point when;
};
void reset();
void inject(uint16_t id, std::initializer_list<uint8_t> bytes, uint8_t type = nxFrameType_CAN_Data, uint8_t flags = 0);
std::vector<nxFrameCAN_t> transmitted();
std::vector<Event> events();
void clear_transmitted();
size_t sessions();
void fail_next(const std::string& operation, nxStatus_t status);
void output_full(bool value);
void bus_state(u32 value, nxStatus_t fault = 0);
}
#endif
