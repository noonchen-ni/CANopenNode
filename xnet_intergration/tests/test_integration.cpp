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

nxFrameCAN_t sdo_download(uint16_t index, uint8_t subindex, uint32_t value, uint8_t size) {
    return exchange(0x60a, {static_cast<uint8_t>(0x23 | ((4 - size) << 2)),
        static_cast<uint8_t>(index), static_cast<uint8_t>(index >> 8), subindex,
        static_cast<uint8_t>(value), static_cast<uint8_t>(value >> 8),
        static_cast<uint8_t>(value >> 16), static_cast<uint8_t>(value >> 24)}, 0x58a);
}

uint32_t sdo_upload(uint16_t index) {
    const auto response = exchange(0x60a, {0x40, static_cast<uint8_t>(index),
        static_cast<uint8_t>(index >> 8), 0, 0, 0, 0, 0}, 0x58a);
    CHECK(response.Payload[0] == 0x43 || response.Payload[0] == 0x4b || response.Payload[0] == 0x4f);
    const auto size = 4U - ((response.Payload[0] >> 2) & 3U);
    uint32_t value = 0;
    for (uint32_t i = 0; i < size; ++i) value |= uint32_t(response.Payload[4 + i]) << (8 * i);
    return value;
}

nxFrameCAN_t guarding_request() {
    fake::clear_transmitted();
    fake::inject(0x70a, {}, nxFrameType_CAN_Remote);
    nxFrameCAN_t response{};
    wait_for([&] {
        for (const auto& frame : fake::transmitted()) if (frame.Identifier == 0x70a) {
            response = frame;
            return true;
        }
        return false;
    });
    CHECK(response.PayloadLength == 1 && response.Type == nxFrameType_CAN_Data);
    return response;
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
    for (const auto& rate : {xco::Json(0), xco::Json(-1), xco::Json(1001), xco::Json(125.5), xco::Json("invalid")}) {
        bad = j; bad["bitrate_kbps"] = rate; CHECK(rejects(bad));
    }
    bad = j; bad["unexpected"] = true; CHECK(rejects(bad));
    auto maximum_life_time = j;
    maximum_life_time["node_guarding"] = {{"guard_time_ms", 65535}, {"life_time_factor", 65}};
    xco::Model maximum_life_time_model; maximum_life_time_model.load(maximum_life_time);
    bad = maximum_life_time; bad["node_guarding"]["life_time_factor"] = 66; CHECK(rejects(bad));
    bad = j; bad["node_guarding"] = {{"guard_time_ms", 65535}, {"life_time_factor", 255}}; CHECK(rejects(bad));
    bad = j; bad["node_guarding"] = {{"guard_time_ms", -1}}; CHECK(rejects(bad));
    bad = j; bad["node_guarding"] = {{"life_time_factor", 256}}; CHECK(rejects(bad));
    bad = j; bad["node_guarding"] = {{"unknown", 1}}; CHECK(rejects(bad));
    bad = j; bad["sync"] = {{"cob_id", 0x701}}; CHECK(rejects(bad));
    bad = j; bad["sync"] = {{"cob_id", 0x40000080}}; CHECK(rejects(bad));
    bad = j; bad["sync"] = {{"period_us", -1}}; CHECK(rejects(bad));
    bad = j; bad["sync"] = {{"window_us", 0x100000000ULL}}; CHECK(rejects(bad));
    bad = j; bad["sync"] = {{"counter_overflow", 1}}; CHECK(rejects(bad));
    bad = j; bad["sync"] = {{"counter_overflow", 241}}; CHECK(rejects(bad));
    bad = j; bad["emcy"] = {{"cob_id", 0x40000090}}; CHECK(rejects(bad));
    bad = j; bad["emcy"] = {{"cob_id", 0x80}}; CHECK(rejects(bad));
    bad = j; bad["emcy"] = {{"inhibit_100us", 65536}}; CHECK(rejects(bad));
}

void startup_bitrates() {
    Fixture f;
    auto j = xco::read_json(f.model_path);
    for (uint16_t rate : {1, 100, 123, 1000}) {
        j["bitrate_kbps"] = rate; f.model(j);
        CHECK(f.initialize() == 0);
        const auto status = f.status();
        CHECK(status.active_bitrate_kbps == rate && status.pending_bitrate_kbps == rate);
        const auto events = fake::events();
        CHECK(!events.empty() && events.back().operation == "bitrate" && events.back().value == uint64_t(rate) * 1000);
        CHECK(xnet_canopen_uninitialize(f.device) == 0 && fake::sessions() == 0);
    }
    j["bitrate_kbps"] = 123; f.model(j);
    CHECK(f.initialize() == 0); f.start();
    fake::inject(0x7e5, {4, 1, 0, 0, 0, 0, 0, 0});
    auto answer = exchange(0x7e5, {0x13, 0, 9, 0, 0, 0, 0, 0}, 0x7e4, 0x13);
    CHECK(answer.Payload[1] != 0 && f.status().pending_bitrate_kbps == 123);
    answer = exchange(0x7e5, {0x17, 0, 0, 0, 0, 0, 0, 0}, 0x7e4, 0x17);
    CHECK(answer.Payload[1] == 0);
    CHECK(xco::read_json(f.saved_path).at("bitrate_kbps") == 123);
    CHECK(xnet_canopen_uninitialize(f.device) == 0);
    j["bitrate_kbps"] = 250; f.model(j);
    CHECK(f.initialize() == 0);
    CHECK(f.status().active_bitrate_kbps == 123 && f.status().pending_bitrate_kbps == 123);
    f.start();
    exchange(0, {0x81, 10}, 0x70a, 0);
    CHECK(f.status().runtime_state == XNET_CO_RUNTIME_RUNNING);
    CHECK(f.status().active_bitrate_kbps == 123 && f.status().pending_bitrate_kbps == 123);
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
    auto check_fallback = [&] {
        CHECK(f.initialize() == 0 && fake::sessions() == 2);
        const auto status = f.status();
        CHECK(status.active_node_id == 10 && status.pending_node_id == 10);
        CHECK(status.active_bitrate_kbps == 125 && status.pending_bitrate_kbps == 125);
        CHECK(diagnostic(f.device).find(std::to_string(XNET_CO_IO_ERROR) + ": ") == 0);
        CHECK(xnet_canopen_uninitialize(f.device) == 0 && fake::sessions() == 0);
    };
    check_fallback();
    xco::Model model; model.load(xco::read_json(f.model_path));
    const auto valid = xco::Json{{"schema_version", 1}, {"identity", model.identity},
        {"node_id", 20}, {"bitrate_kbps", 500}};
    auto bad = valid; bad["identity"] = {9, 9, 9, 9};
    xco::write_json_atomic(f.saved_path, bad); check_fallback();
    bad = valid; bad["schema_version"] = 2;
    xco::write_json_atomic(f.saved_path, bad); check_fallback();
    bad = valid; bad.erase("bitrate_kbps"); bad["unexpected"] = 0;
    xco::write_json_atomic(f.saved_path, bad); check_fallback();
    for (const auto& node : {xco::Json(0), xco::Json(128), xco::Json(256), xco::Json("invalid")}) {
        bad = valid; bad["node_id"] = node;
        xco::write_json_atomic(f.saved_path, bad); check_fallback();
    }
    for (const auto& rate : {xco::Json(0), xco::Json(-1), xco::Json(1001), xco::Json(125.5), xco::Json("invalid")}) {
        bad = valid; bad["bitrate_kbps"] = rate;
        xco::write_json_atomic(f.saved_path, bad); check_fallback();
    }
    f.saved_path = f.directory;
    check_fallback();
}

void reset_with_invalid_persistence() {
    Fixture f;
    xco::Model model; model.load(xco::read_json(f.model_path));
    xco::write_json_atomic(f.saved_path, {{"schema_version", 1}, {"identity", model.identity},
        {"node_id", 20}, {"bitrate_kbps", 500}});
    CHECK(f.initialize() == 0);
    CHECK(f.status().active_node_id == 20 && f.status().active_bitrate_kbps == 500);
    f.start();
    { std::ofstream file(f.saved_path); file << "{broken"; }
    fake::inject(0, {0x81, 20});
    wait_for([&] { return f.status().active_node_id == 10; });
    const auto status = f.status();
    CHECK(status.runtime_state == XNET_CO_RUNTIME_RUNNING);
    CHECK(status.pending_node_id == 10);
    CHECK(status.active_bitrate_kbps == 125 && status.pending_bitrate_kbps == 125);
    CHECK(diagnostic(f.device).find(std::to_string(XNET_CO_IO_ERROR) + ": ") == 0);
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

void caller_managed_resources() {
    Fixture f;
    f.saved_path = std::filesystem::path(".") / std::filesystem::relative(f.saved_path);
    CHECK(f.initialize("can1") == 0);
    CHECK(f.device->interface_name == "can1");
    CHECK(f.device->persistence_path == f.saved_path);
    CHECK(f.device->persistence_path.is_relative());
    xnet_canopen_handle other; CHECK(xnet_canopen_create(&other) == 0);
    const auto path = (f.directory / "other.json").string();
    CHECK(xnet_canopen_initialize(other, "can1", f.model_path.string().c_str(), path.c_str()) == 0);
    CHECK(fake::sessions() == 4);
    CHECK(xnet_canopen_uninitialize(other) == 0);
    CHECK(xnet_canopen_initialize(other, "CAN2", f.model_path.string().c_str(), f.saved_path.string().c_str()) == 0);
    CHECK(fake::sessions() == 4);
    CHECK(xnet_canopen_uninitialize(other) == 0);
    CHECK(xnet_canopen_initialize(other, "can1", f.model_path.string().c_str(), f.saved_path.string().c_str()) == 0);
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

void node_guarding() {
    Fixture f;
    auto j = xco::read_json(f.model_path);
    j["heartbeat_ms"] = 0;
    j["node_guarding"] = {{"guard_time_ms", 100}, {"life_time_factor", 3}};
    f.model(j); CHECK(f.initialize() == 0); f.start();
    CHECK(sdo_upload(0x100c) == 100 && sdo_upload(0x100d) == 3 && sdo_upload(0x100e) == 0x70a);
    fake::clear_transmitted();
    fake::inject(0x70a, {0});
    std::this_thread::sleep_for(15ms);
    for (const auto& frame : fake::transmitted()) CHECK(frame.Identifier != 0x70a);
    CHECK(guarding_request().Payload[0] == CO_NMT_PRE_OPERATIONAL);
    CHECK(guarding_request().Payload[0] == (CO_NMT_PRE_OPERATIONAL | 0x80));
    operational(f);
    CHECK(guarding_request().Payload[0] == CO_NMT_OPERATIONAL);
    wait_for([&] { return f.status().node_guarding_timeout == 1; });
    wait_for([] {
        for (const auto& frame : fake::transmitted())
            if (frame.Identifier == 0x8a && frame.Payload[0] == 0x30 && frame.Payload[1] == 0x81) return true;
        return false;
    });
    wait_for([&] { return f.status().nmt_state == CO_NMT_PRE_OPERATIONAL; });
    guarding_request();
    CHECK(f.status().node_guarding_timeout == 0);
    CHECK(sdo_download(0x100c, 0, 65535, 2).Payload[0] == 0x60);
    CHECK(sdo_download(0x100d, 0, 255, 1).Payload[0] == 0x80);
    CHECK(sdo_upload(0x100d) == 3);
    CHECK(sdo_download(0x100c, 0, 60, 2).Payload[0] == 0x60);
    CHECK(sdo_download(0x100d, 0, 4, 1).Payload[0] == 0x60);
    CHECK(xnet_canopen_stop(f.device) == 0);
    f.start();
    CHECK(f.status().node_guarding_timeout == 0);
    CHECK(sdo_upload(0x100c) == 60 && sdo_upload(0x100d) == 4);
    CHECK(guarding_request().Payload[0] == CO_NMT_PRE_OPERATIONAL);
    exchange(0, {0x82, 10}, 0x70a, 0);
    CHECK(sdo_upload(0x100c) == 100 && sdo_upload(0x100d) == 3);
    CHECK(f.status().node_guarding_timeout == 0);
    CHECK(xnet_canopen_uninitialize(f.device) == 0);
    CHECK(f.status().node_guarding_timeout == 0);
}

void supervision_switching() {
    Fixture f;
    auto j = xco::read_json(f.model_path);
    j["node_guarding"] = {{"guard_time_ms", 20}, {"life_time_factor", 1}};
    f.model(j); CHECK(f.initialize() == 0); f.start();
    fake::clear_transmitted();
    fake::inject(0x70a, {}, nxFrameType_CAN_Remote);
    std::this_thread::sleep_for(40ms);
    CHECK(f.status().node_guarding_timeout == 0);
    for (const auto& frame : fake::transmitted()) CHECK(frame.Identifier != 0x70a);
    CHECK(sdo_download(0x1017, 0, 0, 2).Payload[0] == 0x60);
    guarding_request();
    wait_for([&] { return f.status().node_guarding_timeout == 1; });
    CHECK(sdo_download(0x1017, 0, 50, 2).Payload[0] == 0x60);
    CHECK(f.status().node_guarding_timeout == 0);
    wait_for([] {
        for (const auto& frame : fake::transmitted()) if (frame.Identifier == 0x70a) return true;
        return false;
    });
    CHECK(sdo_download(0x1017, 0, 0, 2).Payload[0] == 0x60);
    CHECK(sdo_download(0x100d, 0, 0, 1).Payload[0] == 0x60);
    CHECK((guarding_request().Payload[0] & 0x7f) == CO_NMT_PRE_OPERATIONAL);
    std::this_thread::sleep_for(40ms);
    CHECK(f.status().node_guarding_timeout == 0);
    CHECK(sdo_download(0x100d, 0, 1, 1).Payload[0] == 0x60);
    guarding_request();
    wait_for([&] { return f.status().node_guarding_timeout == 1; });
    CHECK(sdo_download(0x100c, 0, 0, 2).Payload[0] == 0x60);
    CHECK(f.status().node_guarding_timeout == 0);
}

void configurable_services() {
    Fixture f;
    auto j = xco::read_json(f.model_path);
    j["sync"] = {{"cob_id", "0x90"}};
    j["emcy"] = {{"cob_id", "0xa0"}, {"inhibit_100us", 10}};
    j["tpdos"][0]["transmission_type"] = 0;
    f.model(j); CHECK(f.initialize() == 0); f.start(); operational(f);
    CHECK(sdo_upload(0x1005) == 0x90 && sdo_upload(0x1014) == 0xa0 && sdo_upload(0x1015) == 10);
    auto check_sync = [&](uint16_t old_id, uint16_t new_id) {
        fake::clear_transmitted();
        CHECK(xnet_canopen_request_tpdo(f.device, 1) == 0);
        fake::inject(old_id, {});
        std::this_thread::sleep_for(15ms);
        for (const auto& frame : fake::transmitted()) CHECK(frame.Identifier != 0x18a);
        fake::inject(new_id, {});
        wait_for([] {
            for (const auto& frame : fake::transmitted()) if (frame.Identifier == 0x18a) return true;
            return false;
        });
    };
    check_sync(0x80, 0x90);
    CHECK(sdo_download(0x1005, 0, 0x91, 4).Payload[0] == 0x60);
    check_sync(0x90, 0x91);
    for (uint32_t invalid : {0xa0U, 0x18aU, 0x40000080U, 0x70aU})
        CHECK(sdo_download(0x1005, 0, invalid, 4).Payload[0] == 0x80);
    CHECK(sdo_upload(0x1005) == 0x91);
    CHECK(sdo_download(0x1014, 0, 0xa1, 4).Payload[0] == 0x80);
    CHECK(sdo_download(0x1014, 0, 0x800000a0, 4).Payload[0] == 0x60);
    CHECK(sdo_upload(0x1014) == 0x800000a0);
    CHECK(sdo_download(0x1014, 0, 0x800000a1, 4).Payload[0] == 0x60);
    CHECK(sdo_download(0x1014, 0, 0x91, 4).Payload[0] == 0x80);
    CHECK(sdo_download(0x1014, 0, 0x18a, 4).Payload[0] == 0x80);
    CHECK(sdo_download(0x1014, 0, 0xa1, 4).Payload[0] == 0x60);
    CHECK(sdo_download(0x1015, 0, 20, 2).Payload[0] == 0x60);
    fake::clear_transmitted();
    CHECK(xnet_canopen_set_fault(f.device, 0x40, 0x5000, 42, 1) == 0);
    wait_for([] {
        for (const auto& frame : fake::transmitted())
            if (frame.Identifier == 0xa1 && frame.Payload[1] == 0x50) return true;
        return false;
    });
    CHECK(xnet_canopen_stop(f.device) == 0); f.start();
    CHECK(sdo_upload(0x1005) == 0x91 && sdo_upload(0x1014) == 0xa1 && sdo_upload(0x1015) == 20);
    exchange(0, {0x82, 10}, 0x70a, 0);
    CHECK(sdo_upload(0x1005) == 0x90 && sdo_upload(0x1014) == 0xa0 && sdo_upload(0x1015) == 10);
    CHECK(sdo_download(0x1800, 1, 0x8000018a, 4).Payload[0] == 0x80);
    CHECK(sdo_download(0x1a00, 0, 0, 1).Payload[0] == 0x80);
    CHECK(xnet_canopen_uninitialize(f.device) == 0);
    j.erase("emcy");
    f.model(j); CHECK(f.initialize() == 0); f.start();
    CHECK(sdo_upload(0x1014) == 0x8a);
    CHECK(sdo_download(0x1014, 0, 0x8000008a, 4).Payload[0] == 0x60);
    CHECK(xnet_canopen_stop(f.device) == 0); f.start();
    CHECK(sdo_upload(0x1014) == 0x8000008a);
    CHECK(sdo_download(0x1014, 0, 0xa2, 4).Payload[0] == 0x60);
    CHECK(xnet_canopen_stop(f.device) == 0); f.start();
    CHECK(sdo_upload(0x1014) == 0xa2);
    exchange(0, {0x82, 10}, 0x70a, 0);
    CHECK(sdo_upload(0x1014) == 0x8a);
}

void sync_configuration_and_validation() {
    Fixture f;
    auto j = xco::read_json(f.model_path);
    j["sync"] = {{"cob_id", 0x90}};
    j["emcy"] = {{"cob_id", 0x90}};
    f.model(j); CHECK(f.initialize() == XNET_CO_MODEL_ERROR);
    CHECK(fake::sessions() == 0);
    CHECK(xnet_canopen_uninitialize(f.device) == 0);
    j["emcy"]["cob_id"] = 0xa0;
    j["tpdos"][0]["cob_id"] = 0x90;
    f.model(j); CHECK(f.initialize() == XNET_CO_MODEL_ERROR);
    CHECK(xnet_canopen_uninitialize(f.device) == 0);
    j["tpdos"][0].erase("cob_id");
    j["sync"]["counter_overflow"] = 2;
    f.model(j); CHECK(f.initialize() == 0); f.start();
    CHECK(sdo_upload(0x1019) == 2);
    CHECK(sdo_download(0x1006, 0, 20000, 4).Payload[0] == 0x60);
    CHECK(sdo_download(0x1007, 0, 1000, 4).Payload[0] == 0x60);
    CHECK(sdo_upload(0x1006) == 20000 && sdo_upload(0x1007) == 1000);
    fake::inject(0x90, {1});
    wait_for([&] {
        std::lock_guard<std::recursive_mutex> lock(f.device->mutex);
        return f.device->co->SYNC->syncIsOutsideWindow && f.device->co->SYNC->timeoutError == 2;
    });
    CHECK(xnet_canopen_stop(f.device) == 0); f.start();
    CHECK(sdo_upload(0x1006) == 20000 && sdo_upload(0x1007) == 1000);
    exchange(0, {0x81, 10}, 0x70a, 0);
    CHECK(sdo_upload(0x1006) == 0 && sdo_upload(0x1007) == 0 && sdo_upload(0x1019) == 2);
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
        {"model validation", model_validation}, {"startup bitrates", startup_bitrates}, {"scalar limits", scalar_limits},
        {"batches and lifecycle", batches_and_lifecycle}, {"partial writes and strings", partial_and_strings},
        {"packed batch buffers", packed_batches}, {"batch argument validation and limits", batch_validation},
        {"SDO PDO EMCY NMT", protocol_basics}, {"LSS assignment and resets", lss_assignment_and_resets},
        {"LSS bitrate and persistence", lss_bitrate_and_storage}, {"storage failures", storage_failures},
        {"Reset Node with invalid persistence", reset_with_invalid_persistence},
        {"errors and cleanup", errors_and_cleanup}, {"caller-managed resources", caller_managed_resources},
        {"queues and filtering", queues_and_filtering}, {"concurrent channels", concurrent_channels},
        {"synchronous TPDO requests", synchronous_requests}, {"node guarding", node_guarding},
        {"heartbeat and guarding switching", supervision_switching}, {"configurable SYNC and EMCY", configurable_services},
        {"SYNC timers and service validation", sync_configuration_and_validation},
        {"atomic persistence", persistence_atomicity}};
    int failed = 0;
    for (const auto& test : tests) {
        try { test.second(); std::cout << "PASS " << test.first << "\n"; }
        catch (const std::exception& e) { ++failed; std::cerr << "FAIL " << test.first << ": " << e.what() << "\n"; }
    }
    std::cout << tests.size() - failed << "/" << tests.size() << " tests passed\n";
    return failed ? 1 : 0;
}
