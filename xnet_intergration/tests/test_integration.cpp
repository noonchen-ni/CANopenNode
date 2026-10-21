#include "fake_xnet.hpp"

#include <cstring>
#include <fstream>
#include <functional>
#include <iostream>

#define CHECK(condition) do { if (!(condition)) throw std::runtime_error( \
    std::string(__FILE__) + ":" + std::to_string(__LINE__) + ": " #condition); } while (false)

extern "C" int xnet_canopen_c_header_test(void);

namespace {
using namespace std::chrono_literals;

template<class Predicate> void wait_for(Predicate predicate) {
    const auto deadline = xco::Clock::now() + 2s;
    while (!predicate()) {
        if (xco::Clock::now() >= deadline) throw std::runtime_error("timed out waiting for worker");
        std::this_thread::sleep_for(1ms);
    }
}

std::string diagnostic(xnet_canopen_handle d) {
    int32_t code; uint32_t size; char text[2048]{};
    xnet_canopen_get_last_error(d, &code, text, sizeof(text), &size);
    return std::to_string(code) + ": " + text;
}

struct Fixture {
    xnet_canopen_handle device = nullptr;
    std::filesystem::path directory, model_path, saved_path;
    Fixture() {
        fake::reset();
        directory = std::filesystem::temp_directory_path()
            / ("xnet_canopen_test_" + std::to_string(xco::Clock::now().time_since_epoch().count()));
        CHECK(std::filesystem::create_directory(directory));
        model_path = directory / "device.json"; saved_path = directory / "lss.json";
        std::filesystem::copy_file(XNET_CO_EXAMPLE_MODEL, model_path);
        CHECK(xnet_canopen_create(&device) == 0);
    }
    ~Fixture() {
        if (device && xnet_canopen_destroy(device) != 0) std::cerr << "cleanup failed: " << diagnostic(device) << "\n";
        std::error_code ec;
        for (const auto& entry : std::filesystem::directory_iterator(directory))
            if (entry.is_regular_file()) std::filesystem::remove(entry.path(), ec);
        std::filesystem::remove(directory, ec);
    }
    void model(xco::Json j) { std::ofstream file(model_path); file << j.dump(2); }
    int32_t initialize(const char* name = "CAN1") {
        return xnet_canopen_initialize(device, name, model_path.string().c_str(), saved_path.string().c_str());
    }
    void start() {
        if (xnet_canopen_start(device) != 0) throw std::runtime_error(diagnostic(device));
        wait_for([&] { return !fake::transmitted().empty(); });
    }
    struct Binding { uint16_t index; uint8_t subindex; uint32_t size; };
    uint32_t bind(std::initializer_list<Binding> bindings) {
        std::vector<uint16_t> indices;
        std::vector<uint8_t> subindices;
        std::vector<uint32_t> sizes;
        for (const auto& b : bindings) {
            indices.push_back(b.index);
            subindices.push_back(b.subindex);
            sizes.push_back(b.size);
        }
        uint32_t id = 0;
        CHECK(xnet_canopen_bind_batch(device, indices.data(), subindices.data(), sizes.data(),
                                     static_cast<uint32_t>(bindings.size()), &id) == 0);
        return id;
    }
    xnet_canopen_status_t status() {
        xnet_canopen_status_t s{}; CHECK(xnet_canopen_get_status(device, &s) == 0); return s;
    }
};

nxFrameCAN_t exchange(uint16_t id, std::initializer_list<uint8_t> data, uint16_t response, int command = -1) {
    fake::clear_transmitted();
    fake::inject(id, data);
    nxFrameCAN_t answer{};
    wait_for([&] {
        for (const auto& frame : fake::transmitted()) {
            if (frame.Identifier == response && (command < 0 || frame.Payload[0] == command)) {
                answer = frame; return true;
            }
        }
        return false;
    });
    return answer;
}

void operational(Fixture& f) {
    fake::inject(0, {1, static_cast<uint8_t>(f.status().active_node_id)});
    wait_for([&] { return f.status().nmt_state == CO_NMT_OPERATIONAL; });
}

void model_validation() {
    auto j = xco::read_json(XNET_CO_EXAMPLE_MODEL);
    xco::Model m; m.load(j);
    CHECK(m.config.CNT_LSS_SLV == 1 && m.config.CNT_TPDO == 1);
    CHECK(m.value(0x2004, 2).size == 2);
    CHECK(m.value(0x2005, 2).type == "VISIBLE_STRING");
    CHECK(reinterpret_cast<uintptr_t>(m.value(0x2002, 0).data()) % alignof(float) == 0);
    m.restore(true, 42);
    uint32_t identifier = 0;
    CHECK(OD_get_u32(OD_find(&m.od, 0x1800), 1, &identifier, true) == ODR_OK);
    CHECK(identifier == 0x1aa);
    auto rejects = [](xco::Json bad) {
        try { xco::Model model; model.load(bad); } catch (const std::exception&) { return true; }
        return false;
    };
    auto bad = j; bad["schema_version"] = 2; CHECK(rejects(bad));
    bad = j; bad["objects"][0]["type"] = "DOMAIN"; CHECK(rejects(bad));
    bad = j; bad["objects"][0]["value"] = 65536; CHECK(rejects(bad));
    bad = j; bad["objects"][1]["value"] = 2147483648ULL; CHECK(rejects(bad));
    bad = j; bad["objects"][0]["index"] = "0x1018"; CHECK(rejects(bad));
    bad = j; bad["objects"].push_back(bad["objects"][0]); CHECK(rejects(bad));
    bad = j; bad["tpdos"][0]["mapping"].push_back({{"index", "0x2001"}}); CHECK(rejects(bad));
    bad = j; bad["objects"][3]["value"] = std::string(32, 'x'); CHECK(rejects(bad));
    bad = j; bad["node_id"] = 128; CHECK(rejects(bad));
    bad = j; bad["unexpected"] = true; CHECK(rejects(bad));
}

void scalar_limits() {
    auto j = xco::read_json(XNET_CO_EXAMPLE_MODEL);
    j["objects"].push_back({{"index", "0x2010"}, {"type", "UNSIGNED64"}, {"value", "0xFFFFFFFFFFFFFFFF"}});
    j["objects"].push_back({{"index", "0x2011"}, {"type", "INTEGER64"}, {"value", INT64_MIN}});
    j["objects"].push_back({{"index", "0x2012"}, {"type", "OCTET_STRING"}, {"size", 5}, {"value", {0, 255, 1}}});
    j["objects"].push_back({{"index", "0x2013"}, {"type", "BOOLEAN"}, {"value", true}});
    xco::Model m; m.load(j);
    uint64_t u; int64_t s;
    std::memcpy(&u, m.value(0x2010, 0).data(), 8); CHECK(u == UINT64_MAX);
    std::memcpy(&s, m.value(0x2011, 0).data(), 8); CHECK(s == INT64_MIN);
    auto* bytes = static_cast<uint8_t*>(m.value(0x2012, 0).data());
    CHECK(bytes[0] == 0 && bytes[1] == 255 && bytes[3] == 0);
}

void batches_and_lifecycle() {
    Fixture f; CHECK(f.initialize() == 0);
    CHECK(fake::sessions() == 2);
    auto batch = f.bind({{0x2000, 0, 2}, {0x2001, 0, 4}, {0x2002, 0, 4}});
    uint8_t buffer[10] = {0x34, 0x12, 0xfe, 0xff, 0xff, 0xff};
    float real = 3.25f; std::memcpy(buffer + 6, &real, 4);
    int32_t statuses[3]{};
    CHECK(xnet_canopen_write_batch(f.device, batch, buffer, sizeof(buffer), statuses, 3) == 0);
    CHECK(statuses[0] == 0 && statuses[1] == 0 && statuses[2] == 0);
    std::memset(buffer, 0, sizeof(buffer));
    CHECK(xnet_canopen_read_batch(f.device, batch, buffer, sizeof(buffer), statuses, 3) == 0);
    CHECK(buffer[0] == 0x34 && buffer[1] == 0x12 && buffer[2] == 0xfe && buffer[5] == 0xff);
    std::memcpy(&real, buffer + 6, 4); CHECK(real == 3.25f);
    CHECK(xnet_canopen_read_batch(f.device, batch, buffer, 9, statuses, 3) == XNET_CO_BUFFER_TOO_SMALL);
    CHECK(xnet_canopen_read_batch(f.device, batch, buffer, 10, statuses, 2) == XNET_CO_INVALID_ARGUMENT);
    f.start();
    CHECK(xnet_canopen_start(f.device) == 0);
    CHECK(xnet_canopen_stop(f.device) == 0);
    CHECK(xnet_canopen_stop(f.device) == 0);
    CHECK(xnet_canopen_read_batch(f.device, batch, buffer, 10, statuses, 3) == 0 && buffer[0] == 0x34);
    CHECK(xnet_canopen_uninitialize(f.device) == 0 && fake::sessions() == 0);
    CHECK(f.initialize() == 0);
    CHECK(xnet_canopen_read_batch(f.device, batch, buffer, 10, statuses, 3) == XNET_CO_NOT_FOUND);
    CHECK(f.bind({{0x2000, 0, 2}}) != batch);
}

void packed_batches() {
    Fixture f;
    auto j = xco::read_json(f.model_path);
    j["objects"].push_back({{"index", "0x2010"}, {"type", "UNSIGNED64"}, {"value", 0}, {"local_write", true}});
    j["objects"].push_back({{"index", "0x2011"}, {"type", "BOOLEAN"}, {"value", false}, {"local_write", true}});
    f.model(j); CHECK(f.initialize() == 0);
    uint16_t indices[] = {0x2011, 0x2010, 0x2002, 0x2003};
    uint8_t subindices[] = {0, 0, 0, 0};
    uint32_t sizes[] = {1, 8, 4, 32};
    uint32_t batch = 0;
    CHECK(xnet_canopen_bind_batch(f.device, indices, subindices, sizes, 4, &batch) == 0);
    indices[0] = 0; subindices[1] = 255; sizes[2] = 0;
    uint8_t source[47]{};
    source[0] = 0xa5; source[46] = 0x5a;
    auto* bytes = source + 1;
    bytes[0] = 1;
    for (uint8_t i = 0; i < 8; ++i) bytes[1 + i] = i + 1;
    const float real = 3.25f;
    std::memcpy(bytes + 9, &real, sizeof(real));
    std::memcpy(bytes + 13, "packed", 7);
    int32_t statuses[5] = {-99, -99, -99, -99, -99};
    CHECK(xnet_canopen_write_batch(f.device, batch, bytes, 45, statuses, 4) == 0);
    for (int i = 0; i < 4; ++i) CHECK(statuses[i] == XNET_CO_OK);
    CHECK(statuses[4] == -99 && source[0] == 0xa5 && source[46] == 0x5a);
    uint8_t output[47]{};
    output[0] = 0xa5; output[46] = 0x5a;
    CHECK(xnet_canopen_read_batch(f.device, batch, output + 1, 45, statuses, 4) == 0);
    CHECK(std::memcmp(source, output, sizeof(source)) == 0);
    bytes[0] = 2; bytes[1] = 0x55;
    CHECK(xnet_canopen_write_batch(f.device, batch, bytes, 45, statuses, 4) == XNET_CO_PARTIAL_FAILURE);
    CHECK(statuses[0] == XNET_CO_ACCESS_ERROR && statuses[1] == 0 && statuses[2] == 0 && statuses[3] == 0);
    CHECK(xnet_canopen_read_batch(f.device, batch, output + 1, 45, statuses, 4) == 0);
    CHECK(output[1] == 1 && output[2] == 0x55);
}

void batch_validation() {
    Fixture f;
    auto j = xco::read_json(f.model_path);
    for (uint16_t i = 0; i < 257; ++i)
        j["objects"].push_back({{"index", 0x3000 + i}, {"type", "OCTET_STRING"}, {"size", 4096}, {"value", {0}}});
    f.model(j); CHECK(f.initialize() == 0);
    uint16_t indices[] = {0x2000, 0x2001};
    uint8_t subindices[] = {0, 0};
    uint32_t sizes[] = {2, 4};
    uint32_t batch = 0xabcdef;
    CHECK(xnet_canopen_bind_batch(f.device, nullptr, subindices, sizes, 2, &batch) == XNET_CO_INVALID_ARGUMENT);
    CHECK(xnet_canopen_bind_batch(f.device, indices, nullptr, sizes, 2, &batch) == XNET_CO_INVALID_ARGUMENT);
    CHECK(xnet_canopen_bind_batch(f.device, indices, subindices, nullptr, 2, &batch) == XNET_CO_INVALID_ARGUMENT);
    CHECK(xnet_canopen_bind_batch(f.device, indices, subindices, sizes, 2, nullptr) == XNET_CO_INVALID_ARGUMENT);
    CHECK(xnet_canopen_bind_batch(f.device, indices, subindices, sizes, 0, &batch) == XNET_CO_INVALID_ARGUMENT);
    CHECK(xnet_canopen_bind_batch(f.device, indices, subindices, sizes, 4097, &batch) == XNET_CO_INVALID_ARGUMENT);
    sizes[0] = 0;
    CHECK(xnet_canopen_bind_batch(f.device, indices, subindices, sizes, 2, &batch) == XNET_CO_INVALID_ARGUMENT);
    sizes[0] = UINT32_MAX;
    CHECK(xnet_canopen_bind_batch(f.device, indices, subindices, sizes, 2, &batch) == XNET_CO_INVALID_ARGUMENT);
    sizes[0] = 2; indices[1] = indices[0]; sizes[1] = 2;
    CHECK(xnet_canopen_bind_batch(f.device, indices, subindices, sizes, 2, &batch) == XNET_CO_INVALID_ARGUMENT);
    subindices[1] = 255;
    CHECK(xnet_canopen_bind_batch(f.device, indices, subindices, sizes, 2, &batch) == XNET_CO_NOT_FOUND);
    CHECK(batch == 0xabcdef);
    indices[1] = 0x2001; subindices[1] = 0; sizes[1] = 4;
    CHECK(xnet_canopen_bind_batch(f.device, indices, subindices, sizes, 2, &batch) == 0);
    uint8_t bytes[6] = {0x55};
    int32_t statuses[3] = {-99, -99, -99};
    CHECK(xnet_canopen_write_batch(f.device, batch, bytes, 5, statuses, 2) == XNET_CO_BUFFER_TOO_SMALL);
    CHECK(xnet_canopen_write_batch(f.device, batch, bytes, 6, statuses, 1) == XNET_CO_INVALID_ARGUMENT);
    CHECK(xnet_canopen_write_batch(f.device, batch, bytes, 6, statuses, 3) == XNET_CO_INVALID_ARGUMENT);
    CHECK(xnet_canopen_write_batch(f.device, batch, nullptr, 6, statuses, 2) == XNET_CO_INVALID_ARGUMENT);
    CHECK(xnet_canopen_write_batch(f.device, batch, bytes, 6, nullptr, 2) == XNET_CO_INVALID_ARGUMENT);
    CHECK(xnet_canopen_read_batch(f.device, batch, nullptr, 6, statuses, 2) == XNET_CO_INVALID_ARGUMENT);
    CHECK(xnet_canopen_read_batch(f.device, batch, bytes, 6, nullptr, 2) == XNET_CO_INVALID_ARGUMENT);
    CHECK(statuses[0] == -99 && statuses[1] == -99 && statuses[2] == -99);
    CHECK(xnet_canopen_read_batch(f.device, batch, bytes, 6, statuses, 2) == 0 && bytes[0] == 0);
    CHECK(statuses[0] == 0 && statuses[1] == 0 && statuses[2] == -99);

    std::vector<uint16_t> large_indices;
    std::vector<uint8_t> large_subindices(257, 0);
    std::vector<uint32_t> large_sizes(257, 4096);
    for (uint16_t i = 0; i < 257; ++i) large_indices.push_back(0x3000 + i);
    CHECK(xnet_canopen_bind_batch(f.device, large_indices.data(), large_subindices.data(),
                                 large_sizes.data(), 256, &batch) == 0);
    const auto full_batch = batch;
    CHECK(xnet_canopen_bind_batch(f.device, large_indices.data(), large_subindices.data(),
                                 large_sizes.data(), 257, &batch) == XNET_CO_INVALID_ARGUMENT);
    CHECK(batch == full_batch);
    std::vector<uint8_t> full_buffer(1024 * 1024, 0xff);
    std::vector<int32_t> full_status(256, -99);
    CHECK(xnet_canopen_read_batch(f.device, full_batch, full_buffer.data(),
                                 static_cast<uint32_t>(full_buffer.size()), full_status.data(), 256) == 0);
    CHECK(full_buffer.front() == 0 && full_buffer.back() == 0 && full_status.back() == 0);
}

void partial_and_strings() {
    Fixture f; CHECK(f.initialize() == 0);
    auto batch = f.bind({{0x1018, 1, 4}, {0x2000, 0, 2}});
    uint8_t buffer[6]{9, 0, 0, 0, 0x78, 0x56}; int32_t status[2];
    CHECK(xnet_canopen_write_batch(f.device, batch, buffer, 6, status, 2) == XNET_CO_PARTIAL_FAILURE);
    CHECK(status[0] == XNET_CO_ACCESS_ERROR && status[1] == 0);
    CHECK(xnet_canopen_read_batch(f.device, batch, buffer, 6, status, 2) == 0 && buffer[4] == 0x78);
    auto strings = f.bind({{0x2003, 0, 32}});
    uint8_t text[32]; std::memset(text, 'a', 32);
    CHECK(xnet_canopen_write_batch(f.device, strings, text, 32, status, 1) == XNET_CO_PARTIAL_FAILURE);
    std::memset(text, 0, 32); std::memcpy(text, "manual", 6);
    CHECK(xnet_canopen_write_batch(f.device, strings, text, 32, status, 1) == 0);
    std::memset(text, 0, 32);
    CHECK(xnet_canopen_read_batch(f.device, strings, text, 32, status, 1) == 0);
    CHECK(std::strcmp(reinterpret_cast<char*>(text), "manual") == 0);
}

void protocol_basics() {
    Fixture f; CHECK(f.initialize() == 0); f.start();
    auto answer = exchange(0x60a, {0x40, 0x18, 0x10, 1, 0, 0, 0, 0}, 0x58a);
    CHECK(answer.Payload[0] == 0x43 && answer.Payload[4] == 1);
    answer = exchange(0x60a, {0x2b, 0, 0x18, 2, 1, 0, 0, 0}, 0x58a);
    CHECK(answer.Payload[0] == 0x80 && answer.Payload[4] == 2 && answer.Payload[6] == 1 && answer.Payload[7] == 6);
    answer = exchange(0x60a, {0x40, 3, 0x20, 0, 0, 0, 0, 0}, 0x58a);
    CHECK(answer.Payload[0] == 0x41 && answer.Payload[4] <= 32);
    answer = exchange(0x60a, {0x60, 0, 0, 0, 0, 0, 0, 0}, 0x58a);
    CHECK(answer.Payload[1] == 'X' && answer.Payload[2] == 'N');
    operational(f);
    CHECK(xnet_canopen_request_tpdo(f.device, 1) == 0);
    wait_for([] { for (const auto& frame : fake::transmitted()) if (frame.Identifier == 0x18a) return true; return false; });
    fake::inject(0x20a, {0x78, 0x56, 0x34, 0x12});
    auto batch = f.bind({{0x2001, 0, 4}});
    uint8_t bytes[4]{}; int32_t status;
    wait_for([&] { CHECK(xnet_canopen_read_batch(f.device, batch, bytes, 4, &status, 1) == 0); return bytes[0] == 0x78; });
    CHECK(bytes[3] == 0x12);
    CHECK(xnet_canopen_set_fault(f.device, 0x40, 0x5000, 123, 1) == 0);
    wait_for([] { for (const auto& frame : fake::transmitted()) if (frame.Identifier == 0x8a && frame.Payload[1] == 0x50) return true; return false; });
    CHECK(xnet_canopen_set_fault(f.device, 0x40, 0, 0, 0) == 0);
}

void lss_assignment_and_resets() {
    Fixture f;
    auto j = xco::read_json(f.model_path); j["node_id"] = 255; f.model(j);
    CHECK(f.initialize() == 0);
    CHECK(xnet_canopen_start(f.device) == 0);
    wait_for([&] { return f.status().runtime_state == XNET_CO_RUNTIME_RUNNING; });
    auto answer = exchange(0x7e5, {0x51, 0, 0, 0, 0, 0x80, 0, 0}, 0x7e4, 0x4f);
    CHECK(answer.Payload[0] == 0x4f);
    fake::inject(0x7e5, {0x40, 1, 0, 0, 0, 0, 0, 0});
    fake::inject(0x7e5, {0x41, 1, 0, 0, 0, 0, 0, 0});
    fake::inject(0x7e5, {0x42, 1, 0, 0, 0, 0, 0, 0});
    answer = exchange(0x7e5, {0x43, 0xe9, 3, 0, 0, 0, 0, 0}, 0x7e4, 0x44);
    CHECK(answer.Payload[0] == 0x44);
    answer = exchange(0x7e5, {0x11, 42, 0, 0, 0, 0, 0, 0}, 0x7e4, 0x11);
    CHECK(answer.Payload[1] == 0);
    fake::inject(0x7e5, {4, 0, 0, 0, 0, 0, 0, 0});
    wait_for([&] { return f.status().active_node_id == 42; });
    answer = exchange(0x62a, {0x40, 0, 0x18, 1, 0, 0, 0, 0}, 0x5aa);
    CHECK(answer.Payload[4] == 0xaa && answer.Payload[5] == 1);
    auto batch = f.bind({{0x2000, 0, 2}});
    uint8_t data[2]{77, 0}; int32_t status;
    CHECK(xnet_canopen_write_batch(f.device, batch, data, 2, &status, 1) == 0);
    exchange(0, {0x82, 42}, 0x72a, 0);
    CHECK(xnet_canopen_read_batch(f.device, batch, data, 2, &status, 1) == 0 && data[0] == 77);
    fake::inject(0, {0x81, 42});
    wait_for([&] { return f.status().active_node_id == 255; });
    CHECK(xnet_canopen_read_batch(f.device, batch, data, 2, &status, 1) == 0 && data[0] == 0);
}

void lss_bitrate_and_storage() {
    Fixture f; CHECK(f.initialize() == 0); f.start();
    fake::inject(0x7e5, {4, 1, 0, 0, 0, 0, 0, 0});
    auto answer = exchange(0x7e5, {0x11, 19, 0, 0, 0, 0, 0, 0}, 0x7e4, 0x11);
    CHECK(answer.Payload[1] == 0);
    for (uint8_t command = 0x5a; command <= 0x5e; ++command) {
        answer = exchange(0x7e5, {command, 0, 0, 0, 0, 0, 0, 0}, 0x7e4, command);
        CHECK(answer.Payload[1] == (command == 0x5d ? 0xe9 : command == 0x5e ? 10 : 1));
    }
    answer = exchange(0x7e5, {0x13, 0, 9, 0, 0, 0, 0, 0}, 0x7e4, 0x13);
    CHECK(answer.Payload[1] != 0);
    answer = exchange(0x7e5, {0x13, 0, 2, 0, 0, 0, 0, 0}, 0x7e4, 0x13);
    CHECK(answer.Payload[1] == 0 && f.status().pending_bitrate_kbps == 500);
    const auto requested = xco::Clock::now();
    fake::inject(0x7e5, {0x15, 10, 0, 0, 0, 0, 0, 0});
    wait_for([&] { return f.status().active_bitrate_kbps == 500; });
    std::this_thread::sleep_for(25ms);
    auto events = fake::events();
    xco::Clock::time_point changed{};
    bool resumed = false;
    for (const auto& e : events) {
        if (e.operation == "bitrate" && e.value == 500000) {
            changed = e.when; CHECK(e.when - requested >= 10ms);
        }
        if (changed != xco::Clock::time_point{} && e.operation == "start") {
            CHECK(e.when - changed >= 10ms); resumed = true;
        }
        if (changed != xco::Clock::time_point{} && e.operation == "write") CHECK(e.when - changed >= 10ms);
    }
    CHECK(resumed);
    answer = exchange(0x7e5, {0x17, 0, 0, 0, 0, 0, 0, 0}, 0x7e4, 0x17);
    CHECK(answer.Payload[1] == 0);
    auto stored = xco::read_json(f.saved_path);
    CHECK(stored["node_id"] == 19 && stored["bitrate_kbps"] == 500);
    CHECK(xnet_canopen_uninitialize(f.device) == 0);
    CHECK(f.initialize() == 0);
    CHECK(f.status().active_node_id == 19 && f.status().active_bitrate_kbps == 500);
}

void storage_failures() {
    Fixture f; f.saved_path = f.directory / "absent" / "lss.json";
    CHECK(f.initialize() == 0); f.start();
    fake::inject(0x7e5, {4, 1, 0, 0, 0, 0, 0, 0});
    auto answer = exchange(0x7e5, {0x17, 0, 0, 0, 0, 0, 0, 0}, 0x7e4, 0x17);
    CHECK(answer.Payload[1] != 0);
    CHECK(diagnostic(f.device).find("persistence") != std::string::npos || diagnostic(f.device).find("create") != std::string::npos);
    CHECK(xnet_canopen_uninitialize(f.device) == 0);
    f.saved_path = f.directory / "lss.json";
    { std::ofstream file(f.saved_path); file << "{broken"; }
    CHECK(f.initialize() != 0 && fake::sessions() == 0);
    std::filesystem::remove(f.saved_path);
    xco::write_json_atomic(f.saved_path, {{"schema_version", 1}, {"identity", {9, 9, 9, 9}}, {"node_id", 20}, {"bitrate_kbps", 125}});
    CHECK(f.initialize() == XNET_CO_IO_ERROR);
}

void errors_and_cleanup() {
    Fixture f;
    fake::fail_next("create_output", nxErrInternalError);
    CHECK(f.initialize() == XNET_CO_XNET_ERROR && fake::sessions() == 0);
    CHECK(f.initialize() == 0);
    int32_t code; uint32_t required;
    CHECK(xnet_canopen_get_last_error(f.device, &code, nullptr, 0, &required) == XNET_CO_BUFFER_TOO_SMALL);
    CHECK(code == XNET_CO_XNET_ERROR && required > 1);
    CHECK(xnet_canopen_clear_error(f.device) == 0);
    f.start();
    fake::fail_next("read", nxErrInternalError);
    wait_for([&] { return f.status().runtime_state == XNET_CO_RUNTIME_FAILED; });
    CHECK(diagnostic(f.device).find("read CAN") != std::string::npos);
    CHECK(xnet_canopen_start(f.device) == XNET_CO_INVALID_STATE);
    CHECK(xnet_canopen_uninitialize(f.device) == 0 && fake::sessions() == 0);
    CHECK(f.initialize() == 0);
    fake::fail_next("clear", nxErrInternalError);
    CHECK(xnet_canopen_uninitialize(f.device) == XNET_CO_XNET_ERROR);
    CHECK(xnet_canopen_uninitialize(f.device) == 0 && fake::sessions() == 0);
}

void interface_exclusivity() {
    Fixture f; CHECK(f.initialize() == 0);
    xnet_canopen_handle other; CHECK(xnet_canopen_create(&other) == 0);
    const auto path = (f.directory / "other.json").string();
    CHECK(xnet_canopen_initialize(other, "can1", f.model_path.string().c_str(), path.c_str()) == XNET_CO_INVALID_STATE);
    CHECK(fake::sessions() == 2);
    CHECK(xnet_canopen_initialize(other, "CAN2", f.model_path.string().c_str(), f.saved_path.string().c_str()) == XNET_CO_INVALID_STATE);
    CHECK(xnet_canopen_initialize(other, "CAN2", f.model_path.string().c_str(), path.c_str()) == 0);
    CHECK(fake::sessions() == 4);
    CHECK(xnet_canopen_destroy(other) == 0);
}

void queues_and_filtering() {
    Fixture f; CHECK(f.initialize() == 0); f.start(); operational(f);
    auto batch = f.bind({{0x2001, 0, 4}});
    uint8_t data[4]{}; int32_t status;
    fake::inject(0x20a, {9, 0, 0, 0}, nxFrameType_CAN_Data, nxFrameFlags_TransmitEcho);
    fake::inject(0x20a, {8, 0, 0, 0}, nxFrameType_CANFD_Data);
    std::this_thread::sleep_for(15ms);
    CHECK(xnet_canopen_read_batch(f.device, batch, data, 4, &status, 1) == 0 && data[0] == 0);
    fake::output_full(true);
    CHECK(xnet_canopen_request_tpdo(f.device, 1) == 0);
    wait_for([&] { return (f.status().can_error_status & CO_CAN_ERRTX_OVERFLOW) != 0; });
    fake::output_full(false);
    wait_for([] { for (const auto& frame : fake::transmitted()) if (frame.Identifier == 0x18a) return true; return false; });
    CHECK(xnet_canopen_stop(f.device) == 0);
    fake::inject(0x20a, {7, 0, 0, 0});
    CHECK(xnet_canopen_stop(f.device) == 0);
    f.start(); operational(f);
    CHECK(xnet_canopen_read_batch(f.device, batch, data, 4, &status, 1) == 0 && data[0] == 0);
    fake::bus_state(nxCANCommState_BusOff);
    wait_for([&] { return (f.status().can_error_status & CO_CAN_ERRTX_BUS_OFF) != 0; });
}

void concurrent_channels() {
    Fixture f; CHECK(f.initialize() == 0); f.start();
    auto batch = f.bind({{0x2000, 0, 2}});
    std::atomic<int> errors{0};
    auto operation = [&] {
        for (int i = 0; i < 500; ++i) {
            uint8_t bytes[2]{static_cast<uint8_t>(i), static_cast<uint8_t>(i)};
            int32_t status;
            if (xnet_canopen_write_batch(f.device, batch, bytes, 2, &status, 1) != 0) ++errors;
            if (xnet_canopen_read_batch(f.device, batch, bytes, 2, &status, 1) != 0 || bytes[0] != bytes[1]) ++errors;
        }
    };
    std::thread first(operation), second(operation);
    first.join(); second.join();
    CHECK(errors == 0);
}

void synchronous_requests() {
    Fixture f;
    auto j = xco::read_json(f.model_path);
    j["tpdos"][0]["transmission_type"] = 0;
    f.model(j); CHECK(f.initialize() == 0); f.start(); operational(f);
    fake::clear_transmitted();
    CHECK(xnet_canopen_request_tpdo(f.device, 1) == 0);
    std::this_thread::sleep_for(10ms);
    for (const auto& frame : fake::transmitted()) CHECK(frame.Identifier != 0x18a);
    fake::inject(0x80, {});
    wait_for([] { for (const auto& frame : fake::transmitted()) if (frame.Identifier == 0x18a) return true; return false; });
    CHECK(xnet_canopen_uninitialize(f.device) == 0);
    j["tpdos"][0]["transmission_type"] = 1;
    f.model(j); CHECK(f.initialize() == 0); f.start(); operational(f);
    CHECK(xnet_canopen_request_tpdo(f.device, 1) == XNET_CO_INVALID_STATE);
}

void persistence_atomicity() {
    Fixture f;
    const auto contents = xco::Json{{"original", true}};
    xco::write_json_atomic(f.saved_path, contents);
    xco::write_json_atomic(f.saved_path, {{"replacement", true}});
    CHECK(xco::read_json(f.saved_path) == xco::Json({{"replacement", true}}));
    const auto target = f.directory / "directory.json";
    CHECK(std::filesystem::create_directory(target));
    bool rejected = false;
    try { xco::write_json_atomic(target, contents); } catch (const xco::Error&) { rejected = true; }
    CHECK(rejected && std::filesystem::is_directory(target));
    for (const auto& entry : std::filesystem::directory_iterator(f.directory))
        CHECK(entry.path().filename().string().find(".tmp.") == std::string::npos);
    std::filesystem::remove(target);
}
}

int main() {
    if (!xnet_canopen_c_header_test()) return 1;
    const std::vector<std::pair<const char*, std::function<void()>>> tests = {
        {"model validation", model_validation}, {"scalar limits", scalar_limits},
        {"batches and lifecycle", batches_and_lifecycle}, {"partial writes and strings", partial_and_strings},
        {"packed batch buffers", packed_batches}, {"batch argument validation and limits", batch_validation},
        {"SDO PDO EMCY NMT", protocol_basics}, {"LSS assignment and resets", lss_assignment_and_resets},
        {"LSS bitrate and persistence", lss_bitrate_and_storage}, {"storage failures", storage_failures},
        {"errors and cleanup", errors_and_cleanup}, {"interface exclusivity", interface_exclusivity},
        {"queues and filtering", queues_and_filtering}, {"concurrent channels", concurrent_channels},
        {"synchronous TPDO requests", synchronous_requests}, {"atomic persistence", persistence_atomicity}};
    int failed = 0;
    for (const auto& test : tests) {
        try { test.second(); std::cout << "PASS " << test.first << "\n"; }
        catch (const std::exception& e) { ++failed; std::cerr << "FAIL " << test.first << ": " << e.what() << "\n"; }
    }
    std::cout << tests.size() - failed << "/" << tests.size() << " tests passed\n";
    return failed ? 1 : 0;
}
