#include "internal.hpp"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <limits>
#include <set>
#include <sstream>

namespace xco {
void require(bool condition, int32_t code, const std::string& message) {
    if (!condition) throw Error(code, message);
}

bool valid_bitrate(uint16_t rate) {
    return rate == 10 || rate == 20 || rate == 50 || rate == 125 || rate == 250
        || rate == 500 || rate == 800 || rate == 1000;
}

uint64_t unsigned_number(const Json& j, uint64_t maximum, const std::string& where) {
    uint64_t value = 0;
    if (j.is_string()) {
        const auto text = j.get<std::string>();
        require(!text.empty() && text[0] != '-' && text[0] != '+', XNET_CO_MODEL_ERROR, where + ": invalid unsigned value");
        size_t used = 0;
        try {
            value = std::stoull(text, &used, text.size() > 2 && text.substr(0, 2) == "0x" ? 16 : 10);
        } catch (const std::exception&) {
            throw Error(XNET_CO_MODEL_ERROR, where + ": invalid unsigned value");
        }
        require(used == text.size(), XNET_CO_MODEL_ERROR, where + ": trailing characters");
    } else {
        require(j.is_number_unsigned() || (j.is_number_integer() && j.get<int64_t>() >= 0),
                XNET_CO_MODEL_ERROR, where + ": expected unsigned integer");
        value = j.get<uint64_t>();
    }
    require(value <= maximum, XNET_CO_MODEL_ERROR, where + ": out of range");
    return value;
}

static void keys(const Json& j, std::initializer_list<const char*> allowed, const std::string& where) {
    require(j.is_object(), XNET_CO_MODEL_ERROR, where + ": expected object");
    for (auto it = j.begin(); it != j.end(); ++it) {
        require(std::any_of(allowed.begin(), allowed.end(), [&](const char* key) { return it.key() == key; }),
                XNET_CO_MODEL_ERROR, where + ": unknown field " + it.key());
    }
}

static uint32_t type_size(const std::string& type, const Json& j) {
    static const std::map<std::string, uint32_t> sizes = {
        {"BOOLEAN", 1}, {"UNSIGNED8", 1}, {"UNSIGNED16", 2}, {"UNSIGNED32", 4}, {"UNSIGNED64", 8},
        {"INTEGER8", 1}, {"INTEGER16", 2}, {"INTEGER32", 4}, {"INTEGER64", 8}, {"REAL32", 4}, {"REAL64", 8}};
    auto found = sizes.find(type);
    if (found != sizes.end()) {
        require(!j.contains("size"), XNET_CO_MODEL_ERROR, type + ": scalar size is implicit");
        return found->second;
    }
    require(type == "VISIBLE_STRING" || type == "OCTET_STRING", XNET_CO_MODEL_ERROR, "unsupported type: " + type);
    uint32_t size = static_cast<uint32_t>(unsigned_number(j.at("size"), 4096, type + " size"));
    require(size > 0, XNET_CO_MODEL_ERROR, "string size must be positive");
    return size;
}

static Value make_value(const Json& j, uint8_t sub, bool is_application_object) {
    keys(j, {"subindex", "type", "size", "value", "access", "pdo", "local_write"}, "OD value");
    Value v;
    v.sub = sub;
    v.type = j.at("type").get<std::string>();
    v.size = type_size(v.type, j);
    v.storage.resize((v.size + 7) / 8);
    const auto access = j.value("access", std::string("rw"));
    require(access == "ro" || access == "wo" || access == "rw", XNET_CO_MODEL_ERROR, "invalid access");
    v.attr = static_cast<OD_attr_t>(access == "ro" ? ODA_SDO_R : access == "wo" ? ODA_SDO_W : ODA_SDO_RW);
    const auto pdo = j.value("pdo", std::string("none"));
    require(pdo == "none" || pdo == "tx" || pdo == "rx" || pdo == "both", XNET_CO_MODEL_ERROR, "invalid pdo attribute");
    if (pdo == "tx" || pdo == "both") v.attr |= ODA_TPDO;
    if (pdo == "rx" || pdo == "both") v.attr |= ODA_RPDO;
    require(pdo == "none" || (v.type != "VISIBLE_STRING" && v.type != "OCTET_STRING"),
            XNET_CO_MODEL_ERROR, "string PDO mapping is not supported");
    v.allows_local_write = is_application_object && j.value("local_write", false);
    const auto& data = j.at("value");
    if (v.type == "VISIBLE_STRING") {
        const auto text = data.get<std::string>();
        require(text.size() < v.size && text.find('\0') == std::string::npos
            && std::all_of(text.begin(), text.end(), [](unsigned char c) { return c >= 0x20 && c <= 0x7e; }),
            XNET_CO_MODEL_ERROR, "VISIBLE_STRING requires printable ASCII and space for NUL");
        std::memcpy(v.data(), text.data(), text.size());
        v.attr |= ODA_STR;
    } else if (v.type == "OCTET_STRING") {
        require(data.is_array() && data.size() <= v.size, XNET_CO_MODEL_ERROR, "invalid OCTET_STRING");
        auto* bytes = static_cast<uint8_t*>(v.data());
        for (size_t i = 0; i < data.size(); ++i) bytes[i] = static_cast<uint8_t>(unsigned_number(data[i], 255, "octet"));
    } else if (v.type == "REAL32" || v.type == "REAL64") {
        require(data.is_number(), XNET_CO_MODEL_ERROR, "expected real number");
        const double number = data.get<double>();
        require(std::isfinite(number), XNET_CO_MODEL_ERROR, "non-finite real number");
        if (v.size == 4) {
            const float number32 = static_cast<float>(number);
            require(std::isfinite(number32), XNET_CO_MODEL_ERROR, "REAL32 overflow");
            std::memcpy(v.data(), &number32, 4);
        } else std::memcpy(v.data(), &number, 8);
        v.attr |= ODA_MB;
    } else if (v.type.compare(0, 7, "INTEGER") == 0) {
        require(data.is_number_integer() && (!data.is_number_unsigned() || data.get<uint64_t>() <= INT64_MAX),
                XNET_CO_MODEL_ERROR, "signed value must be an in-range JSON integer");
        const int64_t number = data.get<int64_t>();
        const unsigned bits = v.size * 8;
        require(bits == 64 || (number >= -(int64_t(1) << (bits - 1)) && number < (int64_t(1) << (bits - 1))),
                XNET_CO_MODEL_ERROR, "signed value out of range");
        std::memcpy(v.data(), &number, v.size);
        if (v.size > 1) v.attr |= ODA_MB;
    } else {
        uint64_t number = v.type == "BOOLEAN" && data.is_boolean() ? uint64_t(data.get<bool>())
            : unsigned_number(data, v.size == 8 ? UINT64_MAX : (uint64_t(1) << (v.size * 8)) - 1, v.type);
        require(v.type != "BOOLEAN" || number <= 1, XNET_CO_MODEL_ERROR, "BOOLEAN must be 0 or 1");
        std::memcpy(v.data(), &number, v.size);
        if (v.size > 1) v.attr |= ODA_MB;
    }
    const auto* bytes = static_cast<const uint8_t*>(v.data());
    v.initial.assign(bytes, bytes + v.size);
    return v;
}

static Json scalar(const char* type, const Json& value, const char* access = "ro") {
    return Json{{"type", type}, {"value", value}, {"access", access}};
}

void Model::load(const Json& root) {
    keys(root, {"schema_version", "node_id", "bitrate_kbps", "identity", "heartbeat_ms", "worker_period_us",
                "device_type", "objects", "rpdos", "tpdos"}, "model");
    require(unsigned_number(root.at("schema_version"), 1, "schema_version") == 1, XNET_CO_MODEL_ERROR, "unsupported schema_version");
    startup_node = static_cast<uint8_t>(unsigned_number(root.at("node_id"), 255, "node_id"));
    require((startup_node >= 1 && startup_node <= 127) || startup_node == 255, XNET_CO_MODEL_ERROR, "invalid node_id");
    startup_bitrate = static_cast<uint16_t>(unsigned_number(root.at("bitrate_kbps"), 1000, "bitrate_kbps"));
    require(valid_bitrate(startup_bitrate), XNET_CO_MODEL_ERROR, "unsupported bitrate");
    worker_period_us = static_cast<uint32_t>(unsigned_number(root.value("worker_period_us", Json(1000)), 100000, "worker_period_us"));
    require(worker_period_us >= 100, XNET_CO_MODEL_ERROR, "worker_period_us must be 100..100000");
    const auto& id = root.at("identity");
    keys(id, {"vendor_id", "product_code", "revision_number", "serial_number"}, "identity");
    const char* id_keys[] = {"vendor_id", "product_code", "revision_number", "serial_number"};
    for (size_t i = 0; i < 4; ++i) identity[i] = static_cast<uint32_t>(unsigned_number(id.at(id_keys[i]), UINT32_MAX, id_keys[i]));
    require(identity[0] != 0 && identity[3] != 0, XNET_CO_MODEL_ERROR, "provide nonzero vendor_id and unique serial_number");
    auto add = [&](uint16_t index, std::vector<Json> subs, bool is_var_object, bool is_application_object = false) -> Object& {
        auto obj = std::make_unique<Object>();
        obj->index = index;
        obj->is_var_object = is_var_object;
        std::set<uint8_t> seen;
        for (size_t i = 0; i < subs.size(); ++i) {
            uint8_t sub = static_cast<uint8_t>(unsigned_number(subs[i].value("subindex", Json(i)), 254, "subindex"));
            require(seen.insert(sub).second, XNET_CO_MODEL_ERROR, "duplicate subindex");
            obj->values.push_back(make_value(subs[i], sub, is_application_object));
        }
        require(!obj->values.empty() && obj->values.size() <= 255 && seen.count(0), XNET_CO_MODEL_ERROR, "missing subindex 0 or too many subentries");
        objects.push_back(std::move(obj));
        return *objects.back();
    };
    auto var = [&](uint16_t index, Json j) -> Object& { return add(index, {std::move(j)}, true); };
    // Generate required communication records centrally; custom JSON objects cannot redefine them.
    var(0x1000, scalar("UNSIGNED32", root.value("device_type", Json(0))));
    var(0x1001, scalar("UNSIGNED8", 0));
    std::vector<Json> history{scalar("UNSIGNED8", 0, "rw")};
    for (int i = 0; i < 8; ++i) history.push_back(scalar("UNSIGNED32", 0));
    add(0x1003, history, false);
    var(0x1005, scalar("UNSIGNED32", 0x80));
    var(0x1006, scalar("UNSIGNED32", 0));
    var(0x1007, scalar("UNSIGNED32", 0));
    auto& em = var(0x1014, scalar("UNSIGNED32", 0x80)); em.values[0].is_node_relative = true;
    var(0x1015, scalar("UNSIGNED16", 0));
    var(0x1017, scalar("UNSIGNED16", root.value("heartbeat_ms", Json(1000)), "rw"));
    std::vector<Json> identity_subs{scalar("UNSIGNED8", 4)};
    for (auto number : identity) identity_subs.push_back(scalar("UNSIGNED32", number));
    add(0x1018, identity_subs, false);
    var(0x1019, scalar("UNSIGNED8", 0));
    auto& sdo = add(0x1200, {scalar("UNSIGNED8", 2), scalar("UNSIGNED32", 0x600), scalar("UNSIGNED32", 0x580)}, false);
    sdo.values[1].is_node_relative = sdo.values[2].is_node_relative = true;
    require(root.at("objects").is_array() && root.at("objects").size() <= 4096, XNET_CO_MODEL_ERROR, "objects must be an array of at most 4096 entries");
    for (const auto& j : root.at("objects")) {
        const uint16_t index = static_cast<uint16_t>(unsigned_number(j.at("index"), 0xbfff, "index"));
        require(index >= 0x2000, XNET_CO_MODEL_ERROR, "custom objects must be 0x2000..0xBFFF");
        try {
            const auto kind = j.value("kind", std::string("var"));
            if (kind == "var") {
                keys(j, {"index", "kind", "type", "size", "value", "access", "pdo", "local_write"}, "VAR");
                Json value = j; value.erase("index"); value.erase("kind");
                add(index, {value}, true, true);
            } else if (kind == "record") {
                keys(j, {"index", "kind", "members"}, "RECORD");
                require(j.at("members").is_array(), XNET_CO_MODEL_ERROR, "members must be an array");
                std::vector<Json> members = j.at("members").get<std::vector<Json>>();
                for (const auto& member : members) require(member.contains("subindex"), XNET_CO_MODEL_ERROR, "record members require subindex");
                auto& object = add(index, members, false, true);
                auto& zero = *std::find_if(object.values.begin(), object.values.end(), [](const Value& v) { return v.sub == 0; });
                require(zero.type == "UNSIGNED8", XNET_CO_MODEL_ERROR, "record subindex 0 must be UNSIGNED8");
                auto highest = std::max_element(object.values.begin(), object.values.end(),
                    [](const Value& a, const Value& b) { return a.sub < b.sub; })->sub;
                require(*static_cast<const uint8_t*>(zero.data()) == highest && !zero.allows_local_write
                    && !(zero.attr & (ODA_SDO_W | ODA_TRPDO)), XNET_CO_MODEL_ERROR,
                    "record subindex 0 must be read-only, non-mappable, and contain the highest subindex");
            } else if (kind == "array") {
                keys(j, {"index", "kind", "type", "size", "values", "access", "pdo", "local_write"}, "ARRAY");
                require(j.at("values").is_array() && j.at("values").size() <= 254, XNET_CO_MODEL_ERROR, "array requires at most 254 elements");
                std::vector<Json> members{scalar("UNSIGNED8", j.at("values").size())};
                // Lower arrays to record descriptors while retaining their external subindex semantics.
                for (const auto& item : j.at("values")) {
                    Json member = j; member.erase("index"); member.erase("kind"); member.erase("values");
                    member["value"] = item;
                    members.push_back(std::move(member));
                }
                add(index, members, false, true);
            } else throw Error(XNET_CO_MODEL_ERROR, "unsupported object kind: " + kind);
        } catch (const std::exception& e) {
            std::ostringstream where; where << "OD 0x" << std::hex << index << ": " << e.what();
            throw Error(XNET_CO_MODEL_ERROR, where.str());
        }
    }
    auto pdos = [&](const char* key, bool is_transmit_pdo) {
        const Json list = root.value(key, Json::array());
        require(list.is_array() && list.size() <= 4, XNET_CO_MODEL_ERROR, std::string(key) + " supports at most four PDOs");
        if (is_transmit_pdo) tpdo_count = static_cast<uint16_t>(list.size()); else rpdo_count = static_cast<uint16_t>(list.size());
        for (size_t i = 0; i < list.size(); ++i) {
            const auto& j = list[i];
            keys(j, {"cob_id", "transmission_type", "event_ms", "inhibit_100us", "mapping"}, key);
            const uint32_t type = static_cast<uint32_t>(unsigned_number(j.value("transmission_type", Json(255)), 255, "transmission_type"));
            require(type <= 240 || type >= 254, XNET_CO_MODEL_ERROR, "unsupported PDO transmission type");
            const auto default_id = static_cast<uint32_t>((is_transmit_pdo ? 0x180 : 0x200) + i * 0x100);
            const uint32_t cob = static_cast<uint32_t>(unsigned_number(j.value("cob_id", Json(default_id)), 0x7ff, "cob_id"));
            require(!j.contains("cob_id") || !CO_IS_RESTRICTED_CAN_ID(cob), XNET_CO_MODEL_ERROR, "restricted PDO COB-ID");
            const uint16_t comm_index = static_cast<uint16_t>((is_transmit_pdo ? 0x1800 : 0x1400) + i);
            std::vector<Json> comm{scalar("UNSIGNED8", is_transmit_pdo ? 6 : 5), scalar("UNSIGNED32", cob), scalar("UNSIGNED8", type)};
            if (is_transmit_pdo) {
                comm.push_back(scalar("UNSIGNED16", j.value("inhibit_100us", Json(0))));
                comm.push_back(scalar("UNSIGNED8", 0));
            } else {
                Json timeout = scalar("UNSIGNED16", j.value("event_ms", Json(0))); timeout["subindex"] = 5;
                comm.push_back(timeout);
            }
            if (is_transmit_pdo) {
                comm.push_back(scalar("UNSIGNED16", j.value("event_ms", Json(0))));
                comm.push_back(scalar("UNSIGNED8", 0));
            }
            auto& comm_obj = add(comm_index, comm, false);
            comm_obj.values[1].is_node_relative = !j.contains("cob_id");
            require(j.at("mapping").is_array() && !j.at("mapping").empty() && j.at("mapping").size() <= 8,
                    XNET_CO_MODEL_ERROR, "PDO mapping requires 1..8 entries");
            std::vector<Json> mapping{scalar("UNSIGNED8", j.at("mapping").size())};
            uint32_t bytes = 0;
            for (const auto& m : j.at("mapping")) {
                keys(m, {"index", "subindex"}, "PDO mapping");
                uint16_t index = static_cast<uint16_t>(unsigned_number(m.at("index"), 0xbfff, "mapping index"));
                uint8_t sub = static_cast<uint8_t>(unsigned_number(m.value("subindex", Json(0)), 254, "mapping subindex"));
                auto& v = value(index, sub);
                require((v.attr & (is_transmit_pdo ? ODA_TPDO : ODA_RPDO)) != 0 && v.size <= 8, XNET_CO_MODEL_ERROR, "PDO mapping attribute/type mismatch");
                bytes += v.size;
                // CiA mapping word: index in bits 31..16, subindex in 15..8, mapped bit length in 7..0.
                mapping.push_back(scalar("UNSIGNED32", (uint32_t(index) << 16) | (uint32_t(sub) << 8) | (v.size * 8)));
            }
            require(bytes <= 8, XNET_CO_MODEL_ERROR, "PDO payload exceeds 8 bytes");
            add(static_cast<uint16_t>((is_transmit_pdo ? 0x1a00 : 0x1600) + i), mapping, false);
        }
    };
    pdos("rpdos", false); pdos("tpdos", true);
    // OD_find uses binary search. Publish pointers only after all owning storage is finalized.
    std::sort(objects.begin(), objects.end(), [](const auto& a, const auto& b) { return a->index < b->index; });
    for (size_t i = 1; i < objects.size(); ++i) require(objects[i - 1]->index != objects[i]->index, XNET_CO_MODEL_ERROR, "duplicate OD index");
    entries.reserve(objects.size() + 1);
    for (auto& obj : objects) {
        OD_entry_t e{}; e.index = obj->index; e.subEntriesCount = static_cast<uint8_t>(obj->values.size());
        if (obj->is_var_object) {
            auto& v = obj->values[0]; obj->var = {v.data(), v.attr, v.size};
            e.odObjectType = ODT_VAR; e.odObject = &obj->var;
        } else {
            for (auto& v : obj->values) obj->record.push_back({v.data(), v.sub, v.attr, v.size});
            e.odObjectType = ODT_REC; e.odObject = obj->record.data();
        }
        entries.push_back(e);
    }
    entries.push_back({});
    od = {static_cast<uint16_t>(objects.size()), entries.data()};
    configure();
}

Value& Model::value(uint16_t index, uint8_t sub) {
    for (auto& obj : objects) if (obj->index == index)
        for (auto& v : obj->values) if (v.sub == sub) return v;
    std::ostringstream where; where << "OD 0x" << std::hex << index << ":" << unsigned(sub) << " not found";
    throw Error(XNET_CO_NOT_FOUND, where.str());
}

void Model::restore(bool reset_all_objects, uint8_t node) {
    // Communication objects (0x1000..0x1FFF) hold CANopen protocol configuration.
    // "Application objects" are device-specific data, such as simulated I/O and
    // parameters; this model accepts them from JSON at indices 0x2000..0xBFFF.
    // Reset Communication (false) restores only communication defaults, preserving
    // device data. Initialization and Reset Node (true) restore both ranges.
    for (auto& obj : objects) for (auto& v : obj->values) {
        if (obj->index < 0x2000 || reset_all_objects) std::memcpy(v.data(), v.initial.data(), v.size);
    }
    // Defaults contain base COB-IDs; resolve them for the selected node after copying.
    resolve_node(node);
}

void Model::resolve_node(uint8_t node) {
    // Keep this separate from restore(): stack rebuilds during start/restart need
    // node-dependent COB-IDs and collision checks without resetting other OD values.
    // Only values explicitly marked is_node_relative are rewritten.
    for (auto& obj : objects) for (auto& v : obj->values) {
        if (v.is_node_relative) {
            // Rebase from immutable defaults, not the previous node's already-resolved identifier.
            // This makes repeated resolution idempotent. Node 255 is unconfigured,
            // so retain the base identifier until LSS assigns a usable node ID.
            uint32_t base; std::memcpy(&base, v.initial.data(), 4);
            base += node == 255 ? 0 : node;
            std::memcpy(v.data(), &base, 4);
        }
    }
    if (node != 255) {
        std::set<uint32_t> identifiers{0, 0x80, 0x7e4, 0x7e5, uint32_t(0x80 + node),
            uint32_t(0x580 + node), uint32_t(0x600 + node), uint32_t(0x700 + node)};
        for (auto& obj : objects) {
            if ((obj->index >= 0x1400 && obj->index < 0x1404) || (obj->index >= 0x1800 && obj->index < 0x1804)) {
                auto& cob = value(obj->index, 1);
                uint32_t id; std::memcpy(&id, cob.data(), 4);
                require(identifiers.insert(id).second, XNET_CO_MODEL_ERROR, "PDO COB-ID collides with another local CANopen service");
            }
        }
    }
}

void Model::configure() {
    config = {};
    config.CNT_NMT = config.CNT_EM = config.CNT_SDO_SRV = config.CNT_SYNC = config.CNT_LSS_SLV = 1;
    config.CNT_ARR_1003 = 8;
    config.CNT_RPDO = rpdo_count; config.CNT_TPDO = tpdo_count;
    auto find = [&](uint16_t i) { return OD_find(&od, i); };
    config.ENTRY_H1001 = find(0x1001); config.ENTRY_H1003 = find(0x1003);
    config.ENTRY_H1005 = find(0x1005); config.ENTRY_H1006 = find(0x1006); config.ENTRY_H1007 = find(0x1007);
    config.ENTRY_H1014 = find(0x1014); config.ENTRY_H1015 = find(0x1015); config.ENTRY_H1017 = find(0x1017);
    config.ENTRY_H1019 = find(0x1019); config.ENTRY_H1200 = find(0x1200);
    config.ENTRY_H1400 = find(0x1400); config.ENTRY_H1600 = find(0x1600);
    config.ENTRY_H1800 = find(0x1800); config.ENTRY_H1A00 = find(0x1a00);
}
} // namespace xco
