#ifndef XNET_CO_INTERNAL_HPP
#define XNET_CO_INTERNAL_HPP

#define OD_DEFINITION
#include "CANopen.h"
#include "xnet_canopen.h"
#include "nlohmann/json.hpp"
#include "xnet/nixnet.h"

#include <array>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <deque>
#include <filesystem>
#include <map>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

namespace xco {
using Json = nlohmann::json;
using Clock = std::chrono::steady_clock;

struct Error : std::runtime_error {
    int32_t code;
    Error(int32_t c, const std::string& text) : std::runtime_error(text), code(c) {}
};
void require(bool condition, int32_t code, const std::string& message);
bool valid_bitrate(uint16_t kbps);
uint64_t unsigned_number(const Json& value, uint64_t maximum, const std::string& where);
Json read_json(const std::filesystem::path& path);
void write_json_atomic(const std::filesystem::path& path, const Json& value);

struct Value {
    std::vector<uint64_t> storage;
    std::vector<uint8_t> initial;
    std::string type;
    uint32_t size = 0;
    uint8_t sub = 0;
    OD_attr_t attr = 0;
    bool local_write = false;
    bool node_relative = false;
    void* data() { return storage.data(); }
};

struct Object {
    uint16_t index = 0;
    bool variable = false;
    std::vector<Value> values;
    OD_obj_var_t var{};
    std::vector<OD_obj_record_t> record;
};

struct Model {
    std::vector<std::unique_ptr<Object>> objects;
    std::vector<OD_entry_t> entries;
    OD_t od{};
    CO_config_t config{};
    std::array<uint32_t, 4> identity{};
    uint8_t startup_node = 10;
    uint16_t startup_bitrate = 125;
    uint16_t rpdo_count = 0, tpdo_count = 0;
    uint32_t worker_period_us = 1000;
    void load(const Json& root);
    void restore(bool application, uint8_t node);
    void resolve_node(uint8_t node);
    Value& value(uint16_t index, uint8_t sub);
    void configure();
};

struct Batch {
    std::vector<xnet_canopen_binding_t> bindings;
    uint32_t size = 0;
};

struct Adapter {
    nxSessionRef_t input = 0, output = 0;
    bool started = false;
    void open(const std::string& name, uint16_t bitrate);
    void bitrate(uint16_t kbps);
    void start();
    void stop_flush();
    void close();
    void check(nxStatus_t status, const char* operation);
};
} // namespace xco

struct xnet_canopen_device {
    std::mutex lifecycle;
    std::recursive_mutex mutex;
    std::condition_variable_any wake;
    std::thread worker;
    std::atomic<bool> stopping{false};
    uint32_t state = XNET_CO_UNINITIALIZED;
    int32_t error_code = 0;
    std::array<char, 2048> error_message{};
    std::string interface_name;
    std::filesystem::path persistence_path;
    std::unique_ptr<xco::Model> model;
    xco::Adapter adapter;
    CO_t* co = nullptr;
    uint8_t pending_node = 10, active_node = 10;
    uint16_t pending_bitrate = 125, active_bitrate = 125;
    uint32_t next_batch = 1;
    std::map<uint32_t, xco::Batch> batches;
    struct Pending {
        CO_CANtx_t* buffer;
        CO_CANrxMsg_t frame;
    };
    std::deque<Pending> outgoing;
    std::unique_lock<std::recursive_mutex>* worker_lock = nullptr;
    enum class Switch { none, before, after };
    Switch switching = Switch::none;
    xco::Clock::time_point switch_deadline{};
    uint16_t switch_delay_ms = 0;

    void fail(int32_t code, std::string_view message) noexcept;
    void initialized() const;
    void load_persistent();
    void setup_stack();
    void reset_stack(bool application);
    void run();
    void receive();
    void transmit();
    void stop();
    void release();
    static bool_t check_bitrate(void*, uint16_t rate);
    static void activate_bitrate(void*, uint16_t delay);
    static bool_t store(void*, uint8_t node, uint16_t bitrate);
};

#endif
