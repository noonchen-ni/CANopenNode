#include "fake_xnet.hpp"
#include <cstring>

namespace {
struct Session { u32 mode; bool started = false; u64 bitrate = 0; };
std::mutex mutex;
std::map<nxSessionRef_t, Session> refs;
std::deque<nxFrameCAN_t> incoming;
std::vector<nxFrameCAN_t> sent;
std::vector<fake::Event> trace;
std::map<std::string, nxStatus_t> failures;
nxSessionRef_t next_ref = 1;
bool full = false;
u32 comm_state = 0;
nxStatus_t comm_fault = 0;
nxStatus_t result(const std::string& operation) {
    auto found = failures.find(operation);
    if (found == failures.end()) return 0;
    auto status = found->second; failures.erase(found); return status;
}
void event(const std::string& operation, uint64_t value = 0) {
    trace.push_back({operation, value, xco::Clock::now()});
}
}

namespace fake {
void reset() {
    std::lock_guard<std::mutex> lock(mutex);
    if (!refs.empty()) throw std::runtime_error("fake reset with leaked sessions");
    incoming.clear(); sent.clear(); trace.clear(); failures.clear();
    full = false; comm_state = 0; comm_fault = 0;
}
void inject(uint16_t id, std::initializer_list<uint8_t> bytes, uint8_t type, uint8_t flags) {
    std::lock_guard<std::mutex> lock(mutex);
    nxFrameCAN_t frame{}; frame.Identifier = id; frame.Type = type; frame.Flags = flags;
    frame.PayloadLength = static_cast<u8>(bytes.size());
    std::copy(bytes.begin(), bytes.end(), frame.Payload);
    incoming.push_back(frame);
}
std::vector<nxFrameCAN_t> transmitted() { std::lock_guard<std::mutex> lock(mutex); return sent; }
std::vector<Event> events() { std::lock_guard<std::mutex> lock(mutex); return trace; }
void clear_transmitted() { std::lock_guard<std::mutex> lock(mutex); sent.clear(); }
size_t sessions() { std::lock_guard<std::mutex> lock(mutex); return refs.size(); }
void fail_next(const std::string& operation, nxStatus_t status) {
    std::lock_guard<std::mutex> lock(mutex); failures[operation] = status;
}
void output_full(bool value) { std::lock_guard<std::mutex> lock(mutex); full = value; }
void bus_state(u32 value, nxStatus_t fault) { std::lock_guard<std::mutex> lock(mutex); comm_state = value; comm_fault = fault; }
}

extern "C" {
nxStatus_t _NXFUNC nxCreateSession(const char* database, const char*, const char*, const char*, u32 mode, nxSessionRef_t* ref) {
    std::lock_guard<std::mutex> lock(mutex);
    if (auto status = result(mode == nxMode_FrameInStream ? "create_input" : "create_output")) return status;
    if (std::strcmp(database, ":memory:") != 0) return nxErrParameterNullOrEmpty;
    *ref = next_ref++; refs.emplace(*ref, Session{mode}); event("create", mode); return 0;
}
nxStatus_t _NXFUNC nxSetProperty(nxSessionRef_t ref, u32 property, u32 size, void* value) {
    std::lock_guard<std::mutex> lock(mutex);
    if (auto status = result("property")) return status;
    if (!refs.count(ref)) return nxErrParameterNullOrEmpty;
    if (property == nxPropSession_IntfBaudRate64) {
        if (size != sizeof(u64)) return nxErrParameterNullOrEmpty;
        refs.at(ref).bitrate = *static_cast<u64*>(value); event("bitrate", refs.at(ref).bitrate);
    }
    return 0;
}
nxStatus_t _NXFUNC nxStart(nxSessionRef_t ref, u32) {
    std::lock_guard<std::mutex> lock(mutex);
    if (auto status = result("start")) return status;
    if (!refs.count(ref)) return nxErrParameterNullOrEmpty;
    refs.at(ref).started = true; event("start", ref); return 0;
}
nxStatus_t _NXFUNC nxStop(nxSessionRef_t ref, u32) {
    std::lock_guard<std::mutex> lock(mutex);
    if (auto status = result("stop")) return status;
    if (!refs.count(ref)) return nxErrParameterNullOrEmpty;
    refs.at(ref).started = false; event("stop", ref); return 0;
}
nxStatus_t _NXFUNC nxFlush(nxSessionRef_t ref) {
    std::lock_guard<std::mutex> lock(mutex);
    if (auto status = result("flush")) return status;
    if (!refs.count(ref)) return nxErrParameterNullOrEmpty;
    if (refs.at(ref).mode == nxMode_FrameInStream) incoming.clear();
    event("flush", ref); return 0;
}
nxStatus_t _NXFUNC nxClear(nxSessionRef_t ref) {
    std::lock_guard<std::mutex> lock(mutex);
    if (auto status = result("clear")) return status;
    refs.erase(ref); event("clear", ref); return 0;
}
nxStatus_t _NXFUNC nxReadFrame(nxSessionRef_t ref, void* buffer, u32 size, f64 timeout, u32* bytes) {
    std::lock_guard<std::mutex> lock(mutex);
    *bytes = 0;
    if (auto status = result("read")) return status;
    if (!refs.count(ref) || !refs.at(ref).started || timeout != 0) return nxErrParameterNullOrEmpty;
    while (!incoming.empty() && size - *bytes >= sizeof(nxFrameCAN_t)) {
        std::memcpy(static_cast<uint8_t*>(buffer) + *bytes, &incoming.front(), sizeof(nxFrameCAN_t));
        incoming.pop_front(); *bytes += sizeof(nxFrameCAN_t);
    }
    return 0;
}
nxStatus_t _NXFUNC nxWriteFrame(nxSessionRef_t ref, void* buffer, u32 bytes, f64 timeout) {
    std::lock_guard<std::mutex> lock(mutex);
    if (auto status = result("write")) return status;
    if (full) return nxErrOutputQueueOverflow;
    if (!refs.count(ref) || !refs.at(ref).started || timeout != 0 || bytes != sizeof(nxFrameCAN_t))
        return nxErrParameterNullOrEmpty;
    nxFrameCAN_t frame{}; std::memcpy(&frame, buffer, sizeof(frame));
    sent.push_back(frame); event("write", frame.Identifier); return 0;
}
nxStatus_t _NXFUNC nxReadState(nxSessionRef_t ref, u32 state, u32 size, void* value, nxStatus_t* fault) {
    std::lock_guard<std::mutex> lock(mutex);
    if (auto status = result("state")) return status;
    if (!refs.count(ref) || state != nxState_CANComm || size != sizeof(u32)) return nxErrParameterNullOrEmpty;
    *static_cast<u32*>(value) = comm_state; *fault = comm_fault; return 0;
}
void _NXFUNC nxStatusToString(nxStatus_t, u32 size, char* text) {
    const char* message = "fake XNET error";
    if (size) {
        std::strncpy(text, message, size - 1);
        text[size - 1] = 0;
    }
}
}
