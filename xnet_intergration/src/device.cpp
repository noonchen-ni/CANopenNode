#include "internal.hpp"

#include <algorithm>
#include <cstring>
#include <limits>
#include <set>

using xco::Error;
using xco::require;

namespace {
template<class Function> int32_t call(xnet_canopen_handle d, Function function) noexcept {
    if (!d) return XNET_CO_INVALID_ARGUMENT;
    try {
        // Keep lifecycle ownership even when a call releases the OD lock to join the worker.
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
    require(model && co && state != XNET_CO_RUNTIME_UNINITIALIZED, XNET_CO_INVALID_STATE, "device is not initialized");
}

void xnet_canopen_device::load_persistent() {
    auto node = model->startup_node;
    auto rate = model->startup_bitrate;
    try {
        if (std::filesystem::exists(persistence_path)) {
            auto saved = xco::read_json(persistence_path);
            require(saved.is_object() && saved.size() == 4 && saved.at("schema_version") == 1,
                XNET_CO_IO_ERROR, "unsupported LSS persistence schema: " + persistence_path.string());
            require(saved.at("identity") == xco::Json(model->identity), XNET_CO_IO_ERROR, "persistence identity mismatch: " + persistence_path.string());
            const auto saved_node = static_cast<uint8_t>(xco::unsigned_number(saved.at("node_id"), 255, "saved node ID"));
            const auto saved_rate = static_cast<uint16_t>(xco::unsigned_number(saved.at("bitrate_kbps"), 1000, "saved bitrate"));
            require((saved_node >= 1 && saved_node <= 127) || saved_node == 255, XNET_CO_IO_ERROR, "invalid saved node ID");
            require(xco::valid_bitrate(saved_rate), XNET_CO_IO_ERROR, "invalid saved bitrate");
            node = saved_node;
            rate = saved_rate;
        }
    } catch (const std::exception& e) {
        std::lock_guard<std::recursive_mutex> lock(mutex);
        fail(XNET_CO_IO_ERROR, e.what());
    } catch (...) {
        std::lock_guard<std::recursive_mutex> lock(mutex);
        fail(XNET_CO_IO_ERROR, "cannot load LSS persistence; using model startup settings");
    }
    std::lock_guard<std::recursive_mutex> lock(mutex);
    pending_node = node;
    pending_bitrate = rate;
}

void xnet_canopen_device::setup_stack() {
    outgoing.clear();
    // Extensions point into the old stack allocation; detach them before rebuilding services.
    for (auto& entry : model->entries) entry.extension = nullptr;
    if (co) { CO_delete(co); co = nullptr; }
    // Also reached by ordinary start/restart: update node-dependent identifiers,
    // but do not restore OD defaults here, since stop/start preserves device data.
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

void xnet_canopen_device::reset_stack(bool reset_all_objects) {
    adapter.stop_flush();
    // Reset Node reloads saved/startup LSS settings and restores all OD defaults.
    // Reset Communication preserves device data and the pending LSS settings.
    if (reset_all_objects) {
        // The worker is the only reset owner; filesystem reads need no OD lock.
        worker_lock->unlock();
        try { load_persistent(); } catch (...) { worker_lock->lock(); throw; }
        worker_lock->lock();
        active_bitrate = pending_bitrate;
        adapter.bitrate(active_bitrate);
    }
    model->restore(reset_all_objects, pending_node);
    setup_stack();
    adapter.start();
    CO_CANsetNormalMode(co->CANmodule);
}

/**
 * LSS validation callback: context is this device and rate is in kbit/s.
 * CANopenNode validates the standard table selector/index before calling this
 * function with its numerical rate. Fixed entries are accepted within 1..1000;
 * hardware support is checked when XNET applies the rate, not by this callback.
 * Index 9 arrives as rate 0, which is rejected because automatic detection is
 * not implemented. Startup JSON is not restricted to the LSS table's fixed rates.
 */
bool_t xnet_canopen_device::check_bitrate(void* context, uint16_t rate) {
    auto& d = *static_cast<xnet_canopen_device*>(context);
    if (xco::valid_bitrate(rate)) return true;
    d.fail(XNET_CO_INVALID_ARGUMENT, "unsupported LSS bitrate (automatic detection is not available)");
    return false;
}

/**
 * LSS activation callback, invoked by the worker while holding the OD mutex.
 * delay is the silence interval in milliseconds on each side of the bitrate change.
 * Stop/flush immediately; run() later changes to pending_bitrate and restarts
 * sessions after the second interval, without blocking this callback for the delay.
 */
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
        d.fail(e.code, e.what()); d.state = XNET_CO_RUNTIME_FAILED; d.stop_requested = true;
    }
}

/**
 * LSS Store Configuration callback: save the supplied node ID and kbit/s rate
 * with this device's identity. Runs on the worker and temporarily releases its
 * OD lock during disk I/O; lifecycle teardown joins the worker before freeing it.
 * Return true only when atomic storage completes, so LSS can acknowledge success;
 * storage exceptions record a diagnostic and return false for an LSS error reply.
 */
bool_t xnet_canopen_device::store(void* context, uint8_t node, uint16_t bitrate) {
    auto& d = *static_cast<xnet_canopen_device*>(context);
    const auto saved = xco::Json{{"schema_version", 1}, {"identity", d.model->identity},
        {"node_id", node}, {"bitrate_kbps", bitrate}};
    const auto path = d.persistence_path;
    // Snapshot before unlocking. Teardown joins this worker, so its context stays alive.
    // Returning true only after durable storage lets the LSS stack acknowledge honestly.
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
        while (!stop_requested) {
            auto now = xco::Clock::now();
            auto elapsed = std::chrono::duration_cast<std::chrono::microseconds>(now - previous).count();
            previous = now;
            uint32_t dt = static_cast<uint32_t>(std::min<int64_t>(elapsed, UINT32_MAX));
            if (switching != Switch::none) {
                // Neither protocol processing nor queued transmission may break LSS silence.
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
                // Receive dispatch can request a reset, stop, or bitrate switch in a callback.
                if (switching == Switch::none && !stop_requested) {
                    auto reset = CO_process(co, false, dt, nullptr);
                    if (switching == Switch::none && !stop_requested) {
                        if (reset != CO_RESET_NOT) reset_stack(reset == CO_RESET_APP);
                        else if (!co->nodeIdUnconfigured) {
                            // Apply RPDO commands before packing values for this cycle's TPDOs.
                            bool_t sync = CO_process_SYNC(co, dt, nullptr);
                            CO_process_RPDO(co, sync, dt, nullptr);
                            CO_process_TPDO(co, sync, dt, nullptr);
                        }
                        if (!stop_requested) transmit();
                    }
                }
            }
            // wait_for releases the OD lock, allowing channel calls between worker cycles.
            wake.wait_for(lock, std::chrono::microseconds(model->worker_period_us), [&] { return stop_requested.load(); });
        }
    } catch (const Error& e) {
        fail(e.code, e.what()); state = XNET_CO_RUNTIME_FAILED; stop_requested = true;
    } catch (const std::exception& e) {
        fail(XNET_CO_INTERNAL_ERROR, e.what()); state = XNET_CO_RUNTIME_FAILED; stop_requested = true;
    }
    try { adapter.stop_flush(); }
    catch (const Error& e) { fail(e.code, e.what()); state = XNET_CO_RUNTIME_FAILED; }
    for (auto& pending : outgoing) pending.buffer->bufferFull = false;
    outgoing.clear();
    if (co) co->CANmodule->CANnormal = false;
    worker_lock = nullptr;
}

void xnet_canopen_device::stop() {
    stop_requested = true;
    wake.notify_all();
    // The worker reacquires the OD lock during shutdown; never join while holding it.
    if (worker.joinable()) worker.join();
    std::lock_guard<std::recursive_mutex> lock(mutex);
    initialized();
    adapter.stop_flush();
    outgoing.clear();
    switching = Switch::none;
    co->CANmodule->CANnormal = false;
    if (state != XNET_CO_RUNTIME_FAILED) state = XNET_CO_RUNTIME_STOPPED;
}

void xnet_canopen_device::release() {
    if (worker.joinable()) stop();
    std::lock_guard<std::recursive_mutex> lock(mutex);
    adapter.close();
    outgoing.clear();
    if (co) { CO_delete(co); co = nullptr; }
    model.reset();
    batches.clear();
    interface_name.clear(); persistence_path.clear();
    state = XNET_CO_RUNTIME_UNINITIALIZED;
}

extern "C" {
int32_t xnet_canopen_create(xnet_canopen_handle* handle) {
    if (!handle) return XNET_CO_INVALID_ARGUMENT;
    *handle = nullptr;
    try { *handle = new xnet_canopen_device; return XNET_CO_OK; }
    catch (const std::exception&) { return XNET_CO_INTERNAL_ERROR; }
}

int32_t xnet_canopen_destroy(xnet_canopen_handle handle) {
    auto result = call(handle, [](xnet_canopen_device& d, std::unique_lock<std::recursive_mutex>& lock) {
        lock.unlock(); d.release(); return XNET_CO_OK;
    });
    if (result == XNET_CO_OK) delete handle;
    return result;
}

int32_t xnet_canopen_initialize(xnet_canopen_handle handle, const char* interface_name,
                               const char* model_path, const char* persistence_path) {
    return call(handle, [&](xnet_canopen_device& d, std::unique_lock<std::recursive_mutex>& lock) {
        require(d.state == XNET_CO_RUNTIME_UNINITIALIZED, XNET_CO_INVALID_STATE, "initialize requires an uninitialized handle");
        require(interface_name && *interface_name && model_path && *model_path && persistence_path && *persistence_path,
                XNET_CO_INVALID_ARGUMENT, "interface, JSON model and persistence paths are required");
        try {
            d.interface_name = interface_name;
            d.persistence_path = persistence_path;
            d.model = std::make_unique<xco::Model>();
            d.model->load(xco::read_json(model_path));
            d.load_persistent();
            d.active_bitrate = d.pending_bitrate;
            d.model->restore(true, d.pending_node);
            d.adapter.open(d.interface_name, d.active_bitrate);
            d.setup_stack();
            d.state = XNET_CO_RUNTIME_STOPPED;
        } catch (const std::exception& original) {
            const std::string reason = original.what();
            lock.unlock();
            // A failed cleanup retains resources so uninitialize can retry safely.
            try { d.release(); }
            catch (const std::exception& cleanup) {
                d.state = XNET_CO_RUNTIME_FAILED;
                throw Error(XNET_CO_IO_ERROR, reason + "; cleanup failed: " + cleanup.what() + "; retry uninitialize");
            }
            throw;
        }
        return XNET_CO_OK;
    });
}

int32_t xnet_canopen_uninitialize(xnet_canopen_handle handle) {
    return call(handle, [](xnet_canopen_device& d, std::unique_lock<std::recursive_mutex>& lock) {
        lock.unlock(); d.release(); return XNET_CO_OK;
    });
}
int32_t xnet_canopen_start(xnet_canopen_handle handle) {
    return call(handle, [](xnet_canopen_device& d, std::unique_lock<std::recursive_mutex>&) {
        d.initialized();
        if (d.state == XNET_CO_RUNTIME_RUNNING) return XNET_CO_OK;
        require(d.state == XNET_CO_RUNTIME_STOPPED, XNET_CO_INVALID_STATE, "uninitialize a failed device before restarting");
        try {
            d.setup_stack();
            d.adapter.start();
            CO_CANsetNormalMode(d.co->CANmodule);
            d.stop_requested = false;
            d.state = XNET_CO_RUNTIME_RUNNING;
            d.worker = std::thread(&xnet_canopen_device::run, &d);
        } catch (...) { d.state = XNET_CO_RUNTIME_FAILED; throw; }
        return XNET_CO_OK;
    });
}
int32_t xnet_canopen_stop(xnet_canopen_handle handle) {
    return call(handle, [](xnet_canopen_device& d, std::unique_lock<std::recursive_mutex>& lock) {
        d.initialized(); lock.unlock(); d.stop(); return XNET_CO_OK;
    });
}

int32_t xnet_canopen_bind_batch(xnet_canopen_handle handle, const uint16_t* indices,
                               const uint8_t* subindices, const uint32_t* sizes,
                               uint32_t count, uint32_t* batch_id) {
    return call(handle, [&](xnet_canopen_device& d, std::unique_lock<std::recursive_mutex>&) {
        d.initialized();
        require(indices && subindices && sizes && batch_id && count > 0 && count <= 4096,
                XNET_CO_INVALID_ARGUMENT, "invalid batch arguments");
        require(d.next_batch != 0, XNET_CO_INVALID_STATE, "batch ID space exhausted");
        xco::Batch batch;
        for (uint32_t i = 0; i < count; ++i) {
            // Slots are byte-packed; scalar alignment must not add padding.
            const xco::Binding b{indices[i], subindices[i], batch.size, sizes[i]};
            auto& v = d.model->value(b.index, b.subindex);
            require(b.size == v.size && b.size <= 1024 * 1024 - batch.size,
                    XNET_CO_INVALID_ARGUMENT, "batch OD length mismatch or total size exceeds 1 MiB");
            for (const auto& previous : batch.bindings) {
                require(b.index != previous.index || b.subindex != previous.subindex, XNET_CO_INVALID_ARGUMENT, "duplicate batch OD variable");
            }
            batch.size += b.size;
            batch.bindings.push_back(b);
        }
        d.batches.emplace(d.next_batch, std::move(batch));
        *batch_id = d.next_batch++;
        return XNET_CO_OK;
    });
}

static int32_t batch_io(xnet_canopen_handle handle, uint32_t batch_id, uint8_t* read_buffer, const uint8_t* write_buffer,
                        uint32_t buffer_size, int32_t* statuses, uint32_t count, bool is_write) {
    return call(handle, [&](xnet_canopen_device& d, std::unique_lock<std::recursive_mutex>&) {
        d.initialized();
        require(d.state != XNET_CO_RUNTIME_FAILED, XNET_CO_INVALID_STATE, "device failed; inspect diagnostics and uninitialize");
        auto found = d.batches.find(batch_id);
        require(found != d.batches.end(), XNET_CO_NOT_FOUND, "unknown or invalidated batch ID");
        const auto& batch = found->second;
        require(statuses && (is_write ? write_buffer != nullptr : read_buffer != nullptr) && count == batch.bindings.size(),
                XNET_CO_INVALID_ARGUMENT, "buffer/status pointer or entry count mismatch");
        require(buffer_size >= batch.size, XNET_CO_BUFFER_TOO_SMALL, "batch buffer too small");
        bool has_entry_errors = false;
        for (uint32_t i = 0; i < count; ++i) {
            statuses[i] = XNET_CO_OK;
            const auto& b = batch.bindings[i];
            auto& v = d.model->value(b.index, b.subindex);
            if ((is_write && !v.allows_local_write) || (!is_write && !(v.attr & ODA_SDO_R))) {
                statuses[i] = XNET_CO_ACCESS_ERROR;
            } else {
                OD_IO_t io{};
                // Stack resets replace extensions. Resolve a fresh stream rather than caching callbacks.
                auto result = OD_getSub(OD_find(&d.model->od, b.index), b.subindex, &io, false);
                OD_size_t transferred = 0;
                if (result == ODR_OK) {
                    if (is_write && v.type == "BOOLEAN" && write_buffer[b.offset] > 1) result = ODR_INVALID_VALUE;
                    else if (is_write && v.type == "VISIBLE_STRING"
                        && !std::memchr(write_buffer + b.offset, 0, b.size)) result = ODR_INVALID_VALUE;
                    else if (is_write) result = io.write(&io.stream, write_buffer + b.offset, b.size, &transferred);
                    else result = io.read(&io.stream, read_buffer + b.offset, b.size, &transferred);
                }
                if (result != ODR_OK || transferred != b.size) statuses[i] = XNET_CO_ACCESS_ERROR;
            }
            if (statuses[i] < 0) {
                has_entry_errors = true;
                d.fail(statuses[i], "batch entry " + std::to_string(i) + " OD " + std::to_string(b.index)
                    + ":" + std::to_string(b.subindex) + (is_write ? " write failed" : " read failed"));
            }
        }
        return has_entry_errors ? XNET_CO_PARTIAL_FAILURE : XNET_CO_OK;
    });
}

int32_t xnet_canopen_read_batch(xnet_canopen_handle handle, uint32_t id, uint8_t* buffer,
                               uint32_t size, int32_t* statuses, uint32_t count) {
    return batch_io(handle, id, buffer, nullptr, size, statuses, count, false);
}
int32_t xnet_canopen_write_batch(xnet_canopen_handle handle, uint32_t id, const uint8_t* buffer,
                                uint32_t size, int32_t* statuses, uint32_t count) {
    return batch_io(handle, id, nullptr, buffer, size, statuses, count, true);
}
int32_t xnet_canopen_request_tpdo(xnet_canopen_handle handle, uint16_t number) {
    return call(handle, [&](xnet_canopen_device& d, std::unique_lock<std::recursive_mutex>&) {
        d.initialized();
        require(d.state == XNET_CO_RUNTIME_RUNNING && d.switching == xnet_canopen_device::Switch::none && !d.co->nodeIdUnconfigured
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
    return call(handle, [&](xnet_canopen_device& d, std::unique_lock<std::recursive_mutex>&) {
        d.initialized();
        require(d.state == XNET_CO_RUNTIME_RUNNING && !d.co->nodeIdUnconfigured, XNET_CO_INVALID_STATE, "fault requires initialized running node");
        require(bit >= 0x40 && bit <= 0x4f && active <= 1 && (!active || code != 0),
            XNET_CO_INVALID_ARGUMENT, "invalid manufacturer fault bit/code/active");
        CO_error(d.co->em, active != 0, bit, active ? code : CO_EMC_NO_ERROR, info);
        return XNET_CO_OK;
    });
}
int32_t xnet_canopen_get_status(xnet_canopen_handle handle, xnet_canopen_status_t* status) {
    return call(handle, [&](xnet_canopen_device& d, std::unique_lock<std::recursive_mutex>&) {
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
    return call(handle, [](xnet_canopen_device& d, std::unique_lock<std::recursive_mutex>&) {
        d.fail(0, ""); return XNET_CO_OK;
    });
}
} // extern "C"
