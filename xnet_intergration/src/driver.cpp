#include "internal.hpp"

#include <algorithm>
#include <cstring>

namespace xco {
void Adapter::check(nxStatus_t status, const char* operation) {
    if (status == nxSuccess) return;
    char message[2048]{};
    nxStatusToString(status, sizeof(message), message);
    throw Error(XNET_CO_XNET_ERROR, std::string(operation) + " (" + std::to_string(status) + "): " + message);
}

void Adapter::open(const std::string& name, uint16_t kbps) {
    check(nxCreateSession(":memory:", "", "", name.c_str(), nxMode_FrameInStream, &input), "create input");
    check(nxCreateSession(":memory:", "", "", name.c_str(), nxMode_FrameOutStream, &output), "create output");
    // XNET boolean properties use a one-byte u8, not the stack's uint_fast8_t.
    u8 off = 0;
    check(nxSetProperty(input, nxPropSession_AutoStart, sizeof(off), &off), "disable input auto-start");
    check(nxSetProperty(output, nxPropSession_AutoStart, sizeof(off), &off), "disable output auto-start");
    check(nxSetProperty(input, nxPropSession_IntfEchoTx, sizeof(off), &off), "disable transmit echo");
    u32 timing = nxOutStrmTimng_Immediate;
    check(nxSetProperty(output, nxPropSession_IntfOutStrmTimng, sizeof(timing), &timing), "immediate output timing");
    u32 queue_size = 1024;
    check(nxSetProperty(input, nxPropSession_QueueSize, sizeof(queue_size), &queue_size), "input queue size");
    check(nxSetProperty(output, nxPropSession_QueueSize, sizeof(queue_size), &queue_size), "output queue size");
    bitrate(kbps);
}

void Adapter::bitrate(uint16_t kbps) {
    u64 rate = uint64_t(kbps) * 1000;
    check(nxSetProperty(input, nxPropSession_IntfBaudRate64, sizeof(rate), &rate), "set bitrate");
}

void Adapter::start() {
    check(nxStart(input, nxStartStop_Normal), "start input");
    check(nxStart(output, nxStartStop_Normal), "start output");
    started = true;
}

void Adapter::stop_flush() {
    nxStatus_t first = 0;
    auto retain = [&](nxStatus_t status) { if (first == 0 && status != 0) first = status; };
    // Stop preserves XNET queues. Attempt every cleanup step, then report the first failure.
    if (input) retain(nxStop(input, nxStartStop_Normal));
    if (output) retain(nxStop(output, nxStartStop_Normal));
    if (input) retain(nxFlush(input));
    if (output) retain(nxFlush(output));
    started = false;
    check(first, "stop/flush sessions");
}

void Adapter::close() {
    nxStatus_t first = 0;
    if (output) {
        auto status = nxClear(output);
        // Keep failed references so a later uninitialize can retry their release.
        if (status == 0) output = 0;
        else first = status;
    }
    if (input) {
        auto status = nxClear(input);
        if (status == 0) input = 0;
        else if (first == 0) first = status;
    }
    started = false;
    check(first, "clear sessions");
}
} // namespace xco

extern "C" {
void xnet_co_lock(void* context) {
    if (context) static_cast<xnet_canopen_device*>(context)->mutex.lock();
}
void xnet_co_unlock(void* context) {
    if (context) static_cast<xnet_canopen_device*>(context)->mutex.unlock();
}
void CO_CANsetConfigurationMode(void*) {}
void CO_CANsetNormalMode(CO_CANmodule_t* module) { if (module) module->CANnormal = true; }

CO_ReturnError_t CO_CANmodule_init(CO_CANmodule_t* m, void* context, CO_CANrx_t rx[], uint16_t nr,
                                  CO_CANtx_t tx[], uint16_t nt, uint16_t bitrate) {
    if (!m || !context || !rx || !tx || !xco::valid_bitrate(bitrate)) return CO_ERROR_ILLEGAL_ARGUMENT;
    m->CANptr = context; m->rxArray = rx; m->rxSize = nr; m->txArray = tx; m->txSize = nt;
    m->CANerrorStatus = 0; m->CANnormal = false;
    std::memset(rx, 0, nr * sizeof(*rx)); std::memset(tx, 0, nt * sizeof(*tx));
    return CO_ERROR_NO;
}
void CO_CANmodule_disable(CO_CANmodule_t* m) { if (m) m->CANnormal = false; }

CO_ReturnError_t CO_CANrxBufferInit(CO_CANmodule_t* m, uint16_t index, uint16_t ident, uint16_t mask,
                                   bool_t rtr, void* object, void (*callback)(void*, void*)) {
    if (!m || index >= m->rxSize || !object || !callback) return CO_ERROR_ILLEGAL_ARGUMENT;
    m->rxArray[index] = {static_cast<uint16_t>((ident & 0x7ff) | (rtr ? 0x800 : 0)),
        static_cast<uint16_t>((mask & 0x7ff) | 0x800), object, callback};
    return CO_ERROR_NO;
}
CO_CANtx_t* CO_CANtxBufferInit(CO_CANmodule_t* m, uint16_t index, uint16_t ident, bool_t rtr, uint8_t length, bool_t sync) {
    if (!m || index >= m->txSize || ident > 0x7ff || length > 8) return nullptr;
    auto& b = m->txArray[index];
    b.ident = ident | (rtr ? 0x800U : 0U); b.DLC = length; b.syncFlag = sync; b.bufferFull = false;
    return &b;
}
CO_ReturnError_t CO_CANsend(CO_CANmodule_t* m, CO_CANtx_t* b) {
    if (!m || !m->CANptr || !b) return CO_ERROR_ILLEGAL_ARGUMENT;
    auto& d = *static_cast<xnet_canopen_device*>(m->CANptr);
    std::lock_guard<std::recursive_mutex> lock(d.mutex);
    if (b->bufferFull) {
        m->CANerrorStatus |= CO_CAN_ERRTX_OVERFLOW;
        d.fail(XNET_CO_STACK_ERROR, "CANopen transmit buffer overflow");
        return CO_ERROR_TX_OVERFLOW;
    }
    try {
        xnet_canopen_device::Pending pending{};
        pending.buffer = b;
        pending.frame.ident = static_cast<uint16_t>(b->ident);
        pending.frame.DLC = b->DLC;
        std::memcpy(pending.frame.data, b->data, 8);
        // One immutable pending copy per stack buffer bounds the queue and avoids later mutation.
        d.outgoing.push_back(pending);
        b->bufferFull = true;
        return CO_ERROR_NO;
    } catch (const std::exception& e) {
        d.fail(XNET_CO_INTERNAL_ERROR, e.what());
        return CO_ERROR_OUT_OF_MEMORY;
    }
}
void CO_CANclearPendingSyncPDOs(CO_CANmodule_t* m) {
    auto& d = *static_cast<xnet_canopen_device*>(m->CANptr);
    std::lock_guard<std::recursive_mutex> lock(d.mutex);
    // Only unsent adapter copies are cancellable; flushing XNET would discard unrelated traffic.
    for (auto it = d.outgoing.begin(); it != d.outgoing.end();) {
        if (it->buffer->syncFlag) {
            it->buffer->bufferFull = false;
            it = d.outgoing.erase(it);
            m->CANerrorStatus |= CO_CAN_ERRTX_PDO_LATE;
            d.fail(XNET_CO_STACK_ERROR, "late synchronous PDO removed from adapter queue");
        } else ++it;
    }
}
void CO_CANmodule_process(CO_CANmodule_t* m) {
    auto& d = *static_cast<xnet_canopen_device*>(m->CANptr);
    u32 state = 0; nxStatus_t fault = 0;
    try {
        d.adapter.check(nxReadState(d.adapter.input, nxState_CANComm, sizeof(state), &state, &fault), "read CAN state");
        d.adapter.check(fault, "CAN state fault");
        // Retain loss indicators until reset, but recompute physical bus state from current counters.
        uint16_t flags = m->CANerrorStatus & (CO_CAN_ERRTX_OVERFLOW | CO_CAN_ERRRX_OVERFLOW | CO_CAN_ERRTX_PDO_LATE);
        auto tx = nxCANComm_Get_TxErrCount(state), rx = nxCANComm_Get_RxErrCount(state);
        if (tx >= 96) flags |= CO_CAN_ERRTX_WARNING;
        if (rx >= 96) flags |= CO_CAN_ERRRX_WARNING;
        if (tx >= 128) flags |= CO_CAN_ERRTX_PASSIVE;
        if (rx >= 128) flags |= CO_CAN_ERRRX_PASSIVE;
        if (nxCANComm_Get_CommState(state) == nxCANCommState_BusOff) flags |= CO_CAN_ERRTX_BUS_OFF;
        if (flags != m->CANerrorStatus && flags != 0)
            d.fail(XNET_CO_XNET_ERROR, "CAN communication error flags: " + std::to_string(flags));
        m->CANerrorStatus = flags;
    } catch (const xco::Error& e) {
        d.fail(e.code, e.what());
        d.state = XNET_CO_RUNTIME_FAILED;
        d.stopping = true;
    }
}
} // extern "C"

void xnet_canopen_device::transmit() {
    while (!outgoing.empty()) {
        const auto& pending = outgoing.front();
        nxFrameCAN_t frame{};
        frame.Identifier = pending.frame.ident & 0x7ff;
        frame.Type = (pending.frame.ident & 0x800) ? nxFrameType_CAN_Remote : nxFrameType_CAN_Data;
        frame.PayloadLength = pending.frame.DLC;
        std::memcpy(frame.Payload, pending.frame.data, 8);
        // Submit one frame per call so failure cannot ambiguously accept part of a multi-frame write.
        auto status = nxWriteFrame(adapter.output, &frame, sizeof(frame), nxTimeout_None);
        if (status == nxErrOutputQueueOverflow) {
            co->CANmodule->CANerrorStatus |= CO_CAN_ERRTX_OVERFLOW;
            fail(XNET_CO_XNET_ERROR, "XNET output queue full; frame remains in bounded adapter queue");
            return;
        }
        adapter.check(status, "write CAN frame");
        pending.buffer->bufferFull = false;
        outgoing.pop_front();
    }
}

void xnet_canopen_device::receive() {
    std::array<uint64_t, 1024> storage{};
    u32 bytes = 0;
    auto status = nxReadFrame(adapter.input, storage.data(), static_cast<u32>(sizeof(storage)), nxTimeout_None, &bytes);
    if (status == nxErrInputQueueOverflow) {
        co->CANmodule->CANerrorStatus |= CO_CAN_ERRRX_OVERFLOW;
        fail(XNET_CO_XNET_ERROR, "XNET input queue overflow; unread traffic lost");
        adapter.check(nxFlush(adapter.input), "flush overflowed input");
        return;
    }
    adapter.check(status, "read CAN frames");
    xco::require(bytes <= sizeof(storage), XNET_CO_XNET_ERROR, "XNET returned an invalid byte count");
    size_t offset = 0;
    const auto* raw = reinterpret_cast<const uint8_t*>(storage.data());
    while (offset < bytes) {
        // XNET stream records have variable padded lengths, even when unsupported frame types arrive.
        xco::require(bytes - offset >= nxSizeofFrameHeader, XNET_CO_XNET_ERROR, "truncated XNET frame header");
        nxFrameCAN_t frame{};
        std::memcpy(&frame, raw + offset, nxSizeofFrameHeader);
        const auto size = static_cast<size_t>(nxFrameSize(nxFrameGetPayloadLength(&frame)));
        xco::require(size <= bytes - offset, XNET_CO_XNET_ERROR, "truncated XNET frame payload");
        if ((frame.Type == nxFrameType_CAN_Data || frame.Type == nxFrameType_CAN20_Data || frame.Type == nxFrameType_CAN_Remote)
            && frame.Identifier <= 0x7ff && frame.PayloadLength <= 8 && !(frame.Flags & nxFrameFlags_TransmitEcho)) {
            CO_CANrxMsg_t message{};
            message.ident = static_cast<uint16_t>(frame.Identifier | (frame.Type == nxFrameType_CAN_Remote ? 0x800 : 0));
            message.DLC = frame.PayloadLength;
            // Some stack callbacks copy all eight bytes; keep bytes beyond the actual payload zeroed.
            if (frame.Type != nxFrameType_CAN_Remote) std::memcpy(message.data, raw + offset + nxSizeofFrameHeader, message.DLC);
            for (uint16_t i = 0; i < co->CANmodule->rxSize; ++i) {
                auto& subscription = co->CANmodule->rxArray[i];
                if (subscription.CANrx_callback && ((message.ident ^ subscription.ident) & subscription.mask) == 0) {
                    subscription.CANrx_callback(subscription.object, &message);
                    break;
                }
            }
            // Drain single-message mailboxes before the next frame; the outer loop advances timers once.
            auto reset = CO_process(co, false, 0, nullptr);
            if (reset != CO_RESET_NOT) { reset_stack(reset == CO_RESET_APP); return; }
            if (switching != Switch::none || stopping) return;
        } else if (frame.Type != nxFrameType_CAN_BusError && !(frame.Flags & nxFrameFlags_TransmitEcho)) {
            fail(XNET_CO_XNET_ERROR, "unsupported CAN frame type, identifier, or payload ignored");
        }
        offset += size;
    }
}
