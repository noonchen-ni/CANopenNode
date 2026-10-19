#include "internal.hpp"

#include <algorithm>
#include <cctype>
#include <cstring>
#include <limits>
#include <set>

using xco::Error;
using xco::require;

namespace {
std::mutex claims_mutex;
std::map<std::string, xnet_canopen_device*> interfaces;
std::map<std::filesystem::path, xnet_canopen_device*> files;

void claim(xnet_canopen_device& d) {
    std::lock_guard<std::mutex> lock(claims_mutex);
    require(!interfaces.count(d.interface_name), XNET_CO_INVALID_STATE, "XNET interface already owned by another handle");
    require(!files.count(d.persistence_path), XNET_CO_INVALID_STATE, "persistence path already owned by another handle");
    interfaces.emplace(d.interface_name, &d);
    try { files.emplace(d.persistence_path, &d); }
    catch (...) { interfaces.erase(d.interface_name); throw; }
}
void unclaim(xnet_canopen_device& d) {
    std::lock_guard<std::mutex> lock(claims_mutex);
    auto it = interfaces.find(d.interface_name);
    if (it != interfaces.end() && it->second == &d) interfaces.erase(it);
    auto file = files.find(d.persistence_path);
    if (file != files.end() && file->second == &d) files.erase(file);
}
template<class Function> int32_t call(xnet_canopen_handle d, Function function) noexcept {
    if (!d) return XNET_CO_INVALID_ARGUMENT;
    try {
        std::lock_guard<std::mutex> lifecycle(d->lifecycle);
        std::unique_lock<std::recursive_mutex> lock(d->mutex);
        return function(*d, lock);
    } catch (const Error& e) {
        std::lock_guard<std::recursive_mutex> lock(d->mutex);
        d->fail(e.code, e.what());
        return e.code;
    } catch (const xco::Json::exception& e) {
        std::lock_guard<std::recursive_mutex> lock(d->mutex);
        d->fail(XNET_CO_MODEL_ERROR, e.what());
        return XNET_CO_MODEL_ERROR;
    } catch (const std::exception& e) {
        std::lock_guard<std::recursive_mutex> lock(d->mutex);
        d->fail(XNET_CO_INTERNAL_ERROR, e.what());
        return XNET_CO_INTERNAL_ERROR;
    }
}
void stack_ok(CO_ReturnError_t status, uint32_t info, const char* stage) {
    require(status == CO_ERROR_NO || status == CO_ERROR_NODE_ID_UNCONFIGURED_LSS,
        XNET_CO_STACK_ERROR, std::string(stage) + ": stack status " + std::to_string(status) + ", OD index " + std::to_string(info));
}
}

void xnet_canopen_device::fail(int32_t code, std::string_view message) noexcept {
    error_code = code;
    const auto size = std::min(message.size(), error_message.size() - 1);
    std::memcpy(error_message.data(), message.data(), size);
    error_message[size] = 0;
}
void xnet_canopen_device::initialized() const {
    require(model && co && state != XNET_CO_UNINITIALIZED, XNET_CO_INVALID_STATE, "device is not initialized");
}

void xnet_canopen_device::load_persistent() {
    auto node = model->startup_node;
    auto rate = model->startup_bitrate;
    std::error_code ec;
    const bool exists = std::filesystem::exists(persistence_path, ec);
    require(!ec, XNET_CO_IO_ERROR, "cannot inspect persistence file: " + ec.message());
    if (exists) {
        auto saved = xco::read_json(persistence_path);
        require(saved.is_object() && saved.size() == 4 && saved.at("schema_version") == 1,
            XNET_CO_IO_ERROR, "unsupported LSS persistence schema: " + persistence_path.string());
        require(saved.at("identity") == xco::Json(model->identity), XNET_CO_IO_ERROR, "persistence identity mismatch: " + persistence_path.string());
        node = static_cast<uint8_t>(xco::unsigned_number(saved.at("node_id"), 255, "saved node ID"));
        rate = static_cast<uint16_t>(xco::unsigned_number(saved.at("bitrate_kbps"), 1000, "saved bitrate"));
        require((node >= 1 && node <= 127) || node == 255, XNET_CO_IO_ERROR, "invalid saved node ID");
        require(xco::valid_bitrate(rate), XNET_CO_IO_ERROR, "invalid saved bitrate");
    }
    std::lock_guard<std::recursive_mutex> lock(mutex);
    pending_node = node;
    pending_bitrate = rate;
}

void xnet_canopen_device::setup_stack() {
    outgoing.clear();
    for (auto& entry : model->entries) entry.extension = nullptr;
    if (co) { CO_delete(co); co = nullptr; }
    model->resolve_node(pending_node);
    co = CO_new(&model->config, nullptr);
    require(co != nullptr, XNET_CO_STACK_ERROR, "CANopen stack allocation failed");
    stack_ok(CO_CANinit(co, this, active_bitrate), 0, "CAN initialization");
    CO_LSS_address_t address{};
    std::copy(model->identity.begin(), model->identity.end(), address.addr);
    stack_ok(CO_LSSinit(co, &address, &pending_node, &pending_bitrate), 0, "LSS initialization");
    CO_LSSslave_initCkBitRateCall(co->LSSslave, this, check_bitrate);
    CO_LSSslave_initActBitRateCall(co->LSSslave, this, activate_bitrate);
    CO_LSSslave_initCfgStoreCall(co->LSSslave, this, store);
    active_node = pending_node;
    uint32_t info = 0;
    auto init_result = CO_CANopenInit(co, nullptr, nullptr, &model->od, nullptr,
        CO_NMT_ERR_ON_BUSOFF_HB, 500, 1000, 1000, false, active_node, &info);
    stack_ok(init_result, info, "CANopen initialization");
    info = 0;
    auto result = CO_CANopenInitPDO(co, co->em, &model->od, active_node, &info);
    stack_ok(result, info, "PDO initialization");
}

void xnet_canopen_device::reset_stack(bool application) {
    adapter.stop_flush();
    if (application) {
        // The worker is the only reset owner; filesystem reads need no OD lock.
        worker_lock->unlock();
        try { load_persistent(); } catch (...) { worker_lock->lock(); throw; }
        worker_lock->lock();
        active_bitrate = pending_bitrate;
        adapter.bitrate(active_bitrate);
    }
    model->restore(application, pending_node);
    setup_stack();
    adapter.start();
    CO_CANsetNormalMode(co->CANmodule);
}

bool_t xnet_canopen_device::check_bitrate(void* context, uint16_t rate) {
    auto& d = *static_cast<xnet_canopen_device*>(context);
    if (xco::valid_bitrate(rate)) return true;
    d.fail(XNET_CO_INVALID_ARGUMENT, "unsupported LSS bitrate (automatic detection is not available)");
    return false;
}

void xnet_canopen_device::activate_bitrate(void* context, uint16_t delay) {
    auto& d = *static_cast<xnet_canopen_device*>(context);
    d.switch_delay_ms = delay;
    d.switching = Switch::before;
    try {
        // Stop immediately so queued frames cannot violate either LSS silence interval.
        d.adapter.stop_flush();
        for (auto& pending : d.outgoing) pending.buffer->bufferFull = false;
        d.outgoing.clear();
        d.switch_deadline = xco::Clock::now() + std::chrono::milliseconds(delay);
    } catch (const Error& e) {
        d.fail(e.code, e.what()); d.state = XNET_CO_FAILED; d.stopping = true;
    }
}

bool_t xnet_canopen_device::store(void* context, uint8_t node, uint16_t bitrate) {
    auto& d = *static_cast<xnet_canopen_device*>(context);
    const auto saved = xco::Json{{"schema_version", 1}, {"identity", d.model->identity},
        {"node_id", node}, {"bitrate_kbps", bitrate}};
    const auto path = d.persistence_path;
    d.worker_lock->unlock();
    try {
        xco::write_json_atomic(path, saved);
        d.worker_lock->lock();
        return true;
    } catch (const std::exception& e) {
        d.worker_lock->lock();
        d.fail(XNET_CO_IO_ERROR, e.what());
        return false;
    }
}

void xnet_canopen_device::run() {
    std::unique_lock<std::recursive_mutex> lock(mutex);
    worker_lock = &lock;
    auto previous = xco::Clock::now();
    try {
        while (!stopping) {
            auto now = xco::Clock::now();
            auto elapsed = std::chrono::duration_cast<std::chrono::microseconds>(now - previous).count();
            previous = now;
            uint32_t dt = static_cast<uint32_t>(std::min<int64_t>(elapsed, UINT32_MAX));
            if (switching != Switch::none) {
                if (now >= switch_deadline) {
                    if (switching == Switch::before) {
                        adapter.bitrate(pending_bitrate);
                        active_bitrate = pending_bitrate;
                        switching = Switch::after;
                        switch_deadline = xco::Clock::now() + std::chrono::milliseconds(switch_delay_ms);
                    } else {
                        adapter.start();
                        switching = Switch::none;
                    }
                }
            } else {
                receive();
                if (switching == Switch::none && !stopping) {
                    auto reset = CO_process(co, false, dt, nullptr);
                    if (switching == Switch::none && !stopping) {
                        if (reset != CO_RESET_NOT) reset_stack(reset == CO_RESET_APP);
                        else if (!co->nodeIdUnconfigured) {
                            bool_t sync = CO_process_SYNC(co, dt, nullptr);
                            CO_process_RPDO(co, sync, dt, nullptr);
                            CO_process_TPDO(co, sync, dt, nullptr);
                        }
                        if (!stopping) transmit();
                    }
                }
            }
            wake.wait_for(lock, std::chrono::microseconds(model->worker_period_us), [&] { return stopping.load(); });
        }
    } catch (const Error& e) {
        fail(e.code, e.what()); state = XNET_CO_FAILED; stopping = true;
    } catch (const std::exception& e) {
        fail(XNET_CO_INTERNAL_ERROR, e.what()); state = XNET_CO_FAILED; stopping = true;
    }
    try { adapter.stop_flush(); }
    catch (const Error& e) { fail(e.code, e.what()); state = XNET_CO_FAILED; }
    for (auto& pending : outgoing) pending.buffer->bufferFull = false;
    outgoing.clear();
    if (co) co->CANmodule->CANnormal = false;
    worker_lock = nullptr;
}

void xnet_canopen_device::stop() {
    stopping = true;
    wake.notify_all();
    if (worker.joinable()) worker.join();
    std::lock_guard<std::recursive_mutex> lock(mutex);
    initialized();
    adapter.stop_flush();
    outgoing.clear();
    switching = Switch::none;
    co->CANmodule->CANnormal = false;
    if (state != XNET_CO_FAILED) state = XNET_CO_STOPPED;
}

void xnet_canopen_device::release() {
    if (worker.joinable()) stop();
    std::lock_guard<std::recursive_mutex> lock(mutex);
    adapter.close();
    outgoing.clear();
    if (co) { CO_delete(co); co = nullptr; }
    model.reset();
    batches.clear();
    unclaim(*this);
    interface_name.clear(); persistence_path.clear();
    state = XNET_CO_UNINITIALIZED;
}

extern "C" {
int32_t xnet_canopen_create(xnet_canopen_handle* handle) {
    if (!handle) return XNET_CO_INVALID_ARGUMENT;
    *handle = nullptr;
    try { *handle = new xnet_canopen_device; return XNET_CO_OK; }
    catch (const std::exception&) { return XNET_CO_INTERNAL_ERROR; }
}

int32_t xnet_canopen_destroy(xnet_canopen_handle handle) {
    auto result = call(handle, [](auto& d, auto& lock) {
        lock.unlock(); d.release(); return XNET_CO_OK;
    });
    if (result == XNET_CO_OK) delete handle;
    return result;
}

int32_t xnet_canopen_initialize(xnet_canopen_handle handle, const char* interface_name,
                               const char* model_path, const char* persistence_path) {
    return call(handle, [&](auto& d, auto& lock) {
        require(d.state == XNET_CO_UNINITIALIZED, XNET_CO_INVALID_STATE, "initialize requires an uninitialized handle");
        require(interface_name && *interface_name && model_path && *model_path && persistence_path && *persistence_path,
                XNET_CO_INVALID_ARGUMENT, "interface, JSON model and persistence paths are required");
        try {
            d.interface_name = interface_name;
            std::transform(d.interface_name.begin(), d.interface_name.end(), d.interface_name.begin(),
                [](unsigned char c) { return static_cast<char>(std::toupper(c)); });
            d.persistence_path = std::filesystem::weakly_canonical(std::filesystem::absolute(persistence_path));
            claim(d);
            d.model = std::make_unique<xco::Model>();
            d.model->load(xco::read_json(model_path));
            d.load_persistent();
            d.active_bitrate = d.pending_bitrate;
            d.model->restore(true, d.pending_node);
            d.adapter.open(d.interface_name, d.active_bitrate);
            d.setup_stack();
            d.state = XNET_CO_STOPPED;
        } catch (const std::exception& original) {
            const std::string reason = original.what();
            lock.unlock();
            try { d.release(); }
            catch (const std::exception& cleanup) {
                d.state = XNET_CO_FAILED;
                throw Error(XNET_CO_IO_ERROR, reason + "; cleanup failed: " + cleanup.what() + "; retry uninitialize");
            }
            throw;
        }
        return XNET_CO_OK;
    });
}

int32_t xnet_canopen_uninitialize(xnet_canopen_handle handle) {
    return call(handle, [](auto& d, auto& lock) { lock.unlock(); d.release(); return XNET_CO_OK; });
}
int32_t xnet_canopen_start(xnet_canopen_handle handle) {
    return call(handle, [](auto& d, auto&) {
        d.initialized();
        if (d.state == XNET_CO_RUNNING) return XNET_CO_OK;
        require(d.state == XNET_CO_STOPPED, XNET_CO_INVALID_STATE, "uninitialize a failed device before restarting");
        try {
            d.setup_stack();
            d.adapter.start();
            CO_CANsetNormalMode(d.co->CANmodule);
            d.stopping = false;
            d.state = XNET_CO_RUNNING;
            d.worker = std::thread(&xnet_canopen_device::run, &d);
        } catch (...) { d.state = XNET_CO_FAILED; throw; }
        return XNET_CO_OK;
    });
}
int32_t xnet_canopen_stop(xnet_canopen_handle handle) {
    return call(handle, [](auto& d, auto& lock) { d.initialized(); lock.unlock(); d.stop(); return XNET_CO_OK; });
}

int32_t xnet_canopen_bind_batch(xnet_canopen_handle handle, const xnet_canopen_binding_t* bindings,
                               uint32_t count, uint32_t* batch_id) {
    return call(handle, [&](auto& d, auto&) {
        d.initialized();
        require(bindings && batch_id && count > 0 && count <= 4096, XNET_CO_INVALID_ARGUMENT, "invalid batch arguments");
        require(d.next_batch != 0, XNET_CO_INVALID_STATE, "batch ID space exhausted");
        xco::Batch batch;
        for (uint32_t i = 0; i < count; ++i) {
            const auto& b = bindings[i];
            auto& v = d.model->value(b.index, b.subindex);
            require(!b.reserved && b.size == v.size && b.offset <= 1024 * 1024 && b.size <= 1024 * 1024 - b.offset,
                    XNET_CO_INVALID_ARGUMENT, "batch size/offset/reserved field mismatch");
            for (const auto& previous : batch.bindings) {
                require(b.offset >= previous.offset + previous.size || previous.offset >= b.offset + b.size,
                    XNET_CO_INVALID_ARGUMENT, "overlapping batch buffer ranges");
                require(b.index != previous.index || b.subindex != previous.subindex, XNET_CO_INVALID_ARGUMENT, "duplicate batch OD variable");
            }
            batch.size = std::max(batch.size, b.offset + b.size);
            batch.bindings.push_back(b);
        }
        *batch_id = d.next_batch++;
        d.batches.emplace(*batch_id, std::move(batch));
        return XNET_CO_OK;
    });
}

static int32_t batch_io(xnet_canopen_handle handle, uint32_t batch_id, uint8_t* read_buffer, const uint8_t* write_buffer,
                        uint32_t buffer_size, const uint8_t* enable, int32_t* statuses, uint32_t count, bool writing) {
    return call(handle, [&](auto& d, auto&) {
        d.initialized();
        require(d.state != XNET_CO_FAILED, XNET_CO_INVALID_STATE, "device failed; inspect diagnostics and uninitialize");
        auto found = d.batches.find(batch_id);
        require(found != d.batches.end(), XNET_CO_NOT_FOUND, "unknown or invalidated batch ID");
        const auto& batch = found->second;
        require(statuses && (writing ? write_buffer && enable : read_buffer != nullptr) && count == batch.bindings.size(),
                XNET_CO_INVALID_ARGUMENT, "buffer/status/Enable count mismatch");
        require(buffer_size >= batch.size, XNET_CO_BUFFER_TOO_SMALL, "batch buffer too small");
        if (writing) for (uint32_t i = 0; i < count; ++i)
            require(enable[i] <= 1, XNET_CO_INVALID_ARGUMENT, "Enable values must be 0 or 1");
        bool failed = false;
        for (uint32_t i = 0; i < count; ++i) {
            statuses[i] = XNET_CO_OK;
            if (writing && !enable[i]) { statuses[i] = XNET_CO_SKIPPED; continue; }
            const auto& b = batch.bindings[i];
            auto& v = d.model->value(b.index, b.subindex);
            if ((writing && !v.local_write) || (!writing && !(v.attr & ODA_SDO_R))) {
                statuses[i] = XNET_CO_ACCESS_ERROR;
            } else {
                OD_IO_t io{};
                auto result = OD_getSub(OD_find(&d.model->od, b.index), b.subindex, &io, false);
                OD_size_t transferred = 0;
                if (result == ODR_OK) {
                    if (writing && v.type == "BOOLEAN" && write_buffer[b.offset] > 1) result = ODR_INVALID_VALUE;
                    else if (writing && v.type == "VISIBLE_STRING"
                        && !std::memchr(write_buffer + b.offset, 0, b.size)) result = ODR_INVALID_VALUE;
                    else if (writing) result = io.write(&io.stream, write_buffer + b.offset, b.size, &transferred);
                    else result = io.read(&io.stream, read_buffer + b.offset, b.size, &transferred);
                }
                if (result != ODR_OK || transferred != b.size) statuses[i] = XNET_CO_ACCESS_ERROR;
            }
            if (statuses[i] < 0) {
                failed = true;
                d.fail(statuses[i], "batch entry " + std::to_string(i) + " OD " + std::to_string(b.index)
                    + ":" + std::to_string(b.subindex) + (writing ? " write failed" : " read failed"));
            }
        }
        return failed ? XNET_CO_PARTIAL_FAILURE : XNET_CO_OK;
    });
}

int32_t xnet_canopen_read_batch(xnet_canopen_handle handle, uint32_t id, uint8_t* buffer,
                               uint32_t size, int32_t* statuses, uint32_t count) {
    return batch_io(handle, id, buffer, nullptr, size, nullptr, statuses, count, false);
}
int32_t xnet_canopen_write_batch(xnet_canopen_handle handle, uint32_t id, const uint8_t* buffer,
                                uint32_t size, const uint8_t* enable, int32_t* statuses, uint32_t count) {
    return batch_io(handle, id, nullptr, buffer, size, enable, statuses, count, true);
}
int32_t xnet_canopen_request_tpdo(xnet_canopen_handle handle, uint16_t number) {
    return call(handle, [&](auto& d, auto&) {
        d.initialized();
        require(d.state == XNET_CO_RUNNING && d.switching == xnet_canopen_device::Switch::none && !d.co->nodeIdUnconfigured
            && CO_NMT_getInternalState(d.co->NMT) == CO_NMT_OPERATIONAL, XNET_CO_INVALID_STATE, "TPDO request requires Operational running device");
        require(number >= 1 && number <= d.model->tpdo_count, XNET_CO_INVALID_ARGUMENT, "invalid one-based TPDO number");
        auto& tpdo = d.co->TPDO[number - 1];
        require(tpdo.PDO_common.valid && (tpdo.transmissionType == 0 || tpdo.transmissionType >= 254),
            XNET_CO_INVALID_STATE, "TPDO disabled or not application-request driven");
        CO_TPDOsendRequest(&tpdo);
        return XNET_CO_OK;
    });
}
int32_t xnet_canopen_set_fault(xnet_canopen_handle handle, uint8_t bit, uint16_t code, uint32_t info, uint8_t active) {
    return call(handle, [&](auto& d, auto&) {
        d.initialized();
        require(d.state == XNET_CO_RUNNING && !d.co->nodeIdUnconfigured, XNET_CO_INVALID_STATE, "fault requires initialized running node");
        require(bit >= 0x40 && bit <= 0x4f && active <= 1 && (!active || code != 0),
            XNET_CO_INVALID_ARGUMENT, "invalid manufacturer fault bit/code/active");
        CO_error(d.co->em, active != 0, bit, active ? code : CO_EMC_NO_ERROR, info);
        return XNET_CO_OK;
    });
}
int32_t xnet_canopen_get_status(xnet_canopen_handle handle, xnet_canopen_status_t* status) {
    return call(handle, [&](auto& d, auto&) {
        require(status != nullptr, XNET_CO_INVALID_ARGUMENT, "status output is required");
        *status = {};
        status->runtime_state = d.state;
        if (d.co) {
            status->nmt_state = CO_NMT_getInternalState(d.co->NMT);
            status->can_error_status = d.co->CANmodule->CANerrorStatus;
            status->active_node_id = d.active_node; status->pending_node_id = d.pending_node;
            status->active_bitrate_kbps = d.active_bitrate; status->pending_bitrate_kbps = d.pending_bitrate;
        }
        return XNET_CO_OK;
    });
}
int32_t xnet_canopen_get_last_error(xnet_canopen_handle handle, int32_t* code, char* message, uint32_t capacity, uint32_t* required) {
    // Diagnostic queries must not replace the error they are trying to retrieve.
    if (!handle || !code || !required || (!message && capacity)) return XNET_CO_INVALID_ARGUMENT;
    std::lock_guard<std::recursive_mutex> lock(handle->mutex);
    *code = handle->error_code;
    *required = static_cast<uint32_t>(std::strlen(handle->error_message.data()) + 1);
    if (capacity < *required) return XNET_CO_BUFFER_TOO_SMALL;
    std::memcpy(message, handle->error_message.data(), *required);
    return XNET_CO_OK;
}
int32_t xnet_canopen_clear_error(xnet_canopen_handle handle) {
    return call(handle, [](auto& d, auto&) { d.fail(0, ""); return XNET_CO_OK; });
}
} // extern "C"
