#include "xnet_canopen.h"
#include "nlohmann/json.hpp"

#include <chrono>
#include <csignal>
#include <cstdint>
#include <cstdlib>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <limits>
#include <sstream>
#include <stdexcept>
#include <string>
#include <thread>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#else
#include <dlfcn.h>
#endif

namespace {
namespace fs = std::filesystem;
using Clock = std::chrono::steady_clock;
volatile std::sig_atomic_t shutdown_signal = 0;

extern "C" void request_shutdown(int signal) {
    shutdown_signal = signal;
}

struct Options {
    fs::path library;
    fs::path config;
    uint32_t duration_seconds = 0;
};

void usage(std::ostream& out) {
    out << "Usage: xnet_canopen_device --lib <shared-library> --config <runner.json>"
           " [--duration <positive-integer-seconds>]\n"
           "Prints status every 500 ms; SIGINT/SIGTERM request clean shutdown.\n";
}

Options parse_options(int argc, char** argv) {
    Options result;
    bool duration_seen = false;
    for (int i = 1; i < argc; ++i) {
        const std::string option = argv[i];
        if (option != "--lib" && option != "--config" && option != "--duration")
            throw std::runtime_error("unknown option: " + option);
        if (++i == argc || !*argv[i]) throw std::runtime_error("missing value for " + option);
        const std::string value = argv[i];
        if (option == "--lib") {
            if (!result.library.empty()) throw std::runtime_error("duplicate --lib");
            result.library = value;
        } else if (option == "--config") {
            if (!result.config.empty()) throw std::runtime_error("duplicate --config");
            result.config = value;
        } else {
            if (duration_seen) throw std::runtime_error("duplicate --duration");
            duration_seen = true;
            if (value.find_first_not_of("0123456789") != std::string::npos)
                throw std::runtime_error("--duration must be a positive integer in seconds");
            const auto seconds = std::stoull(value);
            if (seconds == 0 || seconds > std::numeric_limits<uint32_t>::max())
                throw std::runtime_error("--duration is outside 1..4294967295 seconds");
            result.duration_seconds = static_cast<uint32_t>(seconds);
        }
    }
    if (result.library.empty() || result.config.empty())
        throw std::runtime_error("--lib and --config are required");
    result.library = fs::absolute(result.library).lexically_normal();
    result.config = fs::absolute(result.config).lexically_normal();
    return result;
}

struct Config {
    std::string interface_name;
    fs::path model;
    fs::path persistence;
};

Config read_config(const fs::path& path) {
    std::ifstream input(path);
    if (!input) throw std::runtime_error("cannot open runner config: " + path.string());
    const auto json = nlohmann::json::parse(input);
    if (!json.is_object()) throw std::runtime_error("runner config must be a JSON object");
    for (const auto& item : json.items()) {
        if (item.key() != "interface_name" && item.key() != "model_path" && item.key() != "persistence_path")
            throw std::runtime_error("unknown runner config field: " + item.key());
    }
    const auto text = [&](const char* key) {
        const auto& value = json.at(key);
        if (!value.is_string()) throw std::runtime_error(std::string(key) + " must be a string");
        auto result = value.get<std::string>();
        if (result.empty() || result.find('\0') != std::string::npos)
            throw std::runtime_error(std::string(key) + " must be nonempty and contain no NUL");
        return result;
    };
    const auto resolve = [&](const std::string& value) {
        return (path.parent_path() / fs::path(value)).lexically_normal();
    };
    Config result{text("interface_name"), resolve(text("model_path")), resolve(text("persistence_path"))};
    if (!fs::is_regular_file(result.model))
        throw std::runtime_error("model is not a regular file: " + result.model.string());
    if (!fs::is_directory(result.persistence.parent_path()))
        throw std::runtime_error("persistence parent directory must exist: " + result.persistence.parent_path().string());
    if (fs::exists(result.persistence) && !fs::is_regular_file(result.persistence))
        throw std::runtime_error("persistence path is not a regular file: " + result.persistence.string());
    return result;
}

class Library {
public:
    explicit Library(const fs::path& path) {
#ifdef _WIN32
        module_ = LoadLibraryW(path.c_str());
        if (!module_) throw std::runtime_error("LoadLibrary failed (Windows error " + std::to_string(GetLastError())
                                               + "): " + path.string());
#else
        module_ = dlopen(path.c_str(), RTLD_NOW | RTLD_LOCAL);
        if (!module_) throw std::runtime_error(std::string("dlopen failed: ") + dlerror());
#endif
    }

    Library(const Library&) = delete;
    Library& operator=(const Library&) = delete;

    ~Library() {
        if (module_ && !retained_) close();
    }

    bool close() {
        if (retained_) return false;
        if (!module_) return true;
#ifdef _WIN32
        const bool ok = FreeLibrary(module_) != 0;
        if (!ok) std::cerr << "FreeLibrary failed: " << GetLastError() << '\n';
#else
        const bool ok = dlclose(module_) == 0;
        if (!ok) std::cerr << "dlclose failed: " << dlerror() << '\n';
#endif
        module_ = nullptr;
        return ok;
    }

    template<typename Function>
    Function resolve(const char* name) const {
#ifdef _WIN32
        const auto symbol = GetProcAddress(module_, name);
        if (!symbol) throw std::runtime_error(std::string("missing library symbol: ") + name);
#else
        dlerror();
        const auto symbol = dlsym(module_, name);
        if (const char* error = dlerror()) throw std::runtime_error(std::string("dlsym ") + name + ": " + error);
#endif
        return reinterpret_cast<Function>(symbol);
    }

    void retain() { retained_ = true; }

private:
#ifdef _WIN32
    HMODULE module_ = nullptr;
#else
    void* module_ = nullptr;
#endif
    bool retained_ = false;
};

struct Api {
    decltype(&xnet_canopen_create) create;
    decltype(&xnet_canopen_initialize) initialize;
    decltype(&xnet_canopen_start) start;
    decltype(&xnet_canopen_stop) stop;
    decltype(&xnet_canopen_uninitialize) uninitialize;
    decltype(&xnet_canopen_destroy) destroy;
    decltype(&xnet_canopen_get_status) get_status;
    decltype(&xnet_canopen_get_last_error) get_last_error;

    explicit Api(const Library& library)
        : create(library.resolve<decltype(create)>("xnet_canopen_create")),
          initialize(library.resolve<decltype(initialize)>("xnet_canopen_initialize")),
          start(library.resolve<decltype(start)>("xnet_canopen_start")),
          stop(library.resolve<decltype(stop)>("xnet_canopen_stop")),
          uninitialize(library.resolve<decltype(uninitialize)>("xnet_canopen_uninitialize")),
          destroy(library.resolve<decltype(destroy)>("xnet_canopen_destroy")),
          get_status(library.resolve<decltype(get_status)>("xnet_canopen_get_status")),
          get_last_error(library.resolve<decltype(get_last_error)>("xnet_canopen_get_last_error")) {}

    void diagnostic(xnet_canopen_handle handle) const {
        if (!handle) return;
        int32_t code = 0;
        uint32_t required = 0;
        char message[2048]{};
        const auto result = get_last_error(handle, &code, message, sizeof(message), &required);
        if (result != XNET_CO_OK) {
            std::cerr << "xnet_canopen_get_last_error returned " << result << " (required bytes=" << required << ")\n";
        } else if (code != XNET_CO_OK) {
            std::cerr << "Latest native diagnostic (retained, not necessarily new): code=" << code
                      << " message=" << message << '\n';
        }
    }

    bool check(int32_t result, const char* operation, xnet_canopen_handle handle) const {
        if (result == XNET_CO_OK) return true;
        std::cerr << operation << " returned " << result << '\n';
        diagnostic(handle);
        return false;
    }
};

class Device {
public:
    Device(const Api& api, Library& library) : api_(api), library_(library) {}
    Device(const Device&) = delete;
    Device& operator=(const Device&) = delete;
    ~Device() { cleanup(); }

    xnet_canopen_handle handle = nullptr;
    bool initialize_attempted = false;
    bool initialized = false;

    bool cleanup() {
        if (!handle) return true;
        bool ok = true;
        if (initialized) ok = api_.check(api_.stop(handle), "xnet_canopen_stop", handle) && ok;
        if (initialize_attempted)
            ok = api_.check(api_.uninitialize(handle), "xnet_canopen_uninitialize", handle) && ok;
        const auto result = api_.destroy(handle);
        if (!api_.check(result, "xnet_canopen_destroy", handle)) {
            library_.retain();
            std::cerr << "Cleanup failed; library remains loaded until process exit to avoid unloading live code.\n";
            ok = false;
        } else {
            std::cerr << "Device destroyed; cleanup complete.\n";
        }
        handle = nullptr;
        return ok;
    }

private:
    const Api& api_;
    Library& library_;
};

const char* runtime_name(uint32_t state) {
    switch (state) {
        case XNET_CO_RUNTIME_UNINITIALIZED: return "UNINITIALIZED";
        case XNET_CO_RUNTIME_STOPPED: return "STOPPED";
        case XNET_CO_RUNTIME_RUNNING: return "RUNNING";
        case XNET_CO_RUNTIME_FAILED: return "FAILED";
        default: return "UNKNOWN";
    }
}

const char* nmt_name(uint32_t state) {
    switch (state) {
        case 0: return "INITIALIZING/UNCONFIGURED";
        case 4: return "STOPPED";
        case 5: return "OPERATIONAL";
        case 127: return "PRE_OPERATIONAL";
        default: return "UNKNOWN";
    }
}

std::string timestamp() {
    const auto now = std::chrono::system_clock::now();
    const auto time = std::chrono::system_clock::to_time_t(now);
    std::tm utc{};
#ifdef _WIN32
    if (gmtime_s(&utc, &time) != 0) throw std::runtime_error("cannot format UTC timestamp");
#else
    if (!gmtime_r(&time, &utc)) throw std::runtime_error("cannot format UTC timestamp");
#endif
    const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(now.time_since_epoch()).count() % 1000;
    std::ostringstream out;
    out << std::put_time(&utc, "%Y-%m-%dT%H:%M:%S") << '.' << std::setfill('0') << std::setw(3) << ms << 'Z';
    return out.str();
}

void print_status(const xnet_canopen_status_t& s) {
    static_assert(sizeof(s) == 40, "device app requires the ten-field status ABI");
    std::cout << timestamp()
              << " runtime_state=" << runtime_name(s.runtime_state) << '(' << s.runtime_state << ')'
              << " nmt_state=" << nmt_name(s.nmt_state) << '(' << s.nmt_state << ')'
              << " active_node_id=" << s.active_node_id << " pending_node_id=" << s.pending_node_id
              << " active_bitrate_kbps=" << s.active_bitrate_kbps << " pending_bitrate_kbps=" << s.pending_bitrate_kbps
              << " can_error_status=0x" << std::hex << std::setfill('0') << std::setw(8) << s.can_error_status << std::dec
              << " node_guarding_timeout=" << s.node_guarding_timeout
              << " reset_node_count=" << s.reset_node_count
              << " reset_communication_count=" << s.reset_communication_count << std::endl;
    if (!std::cout) throw std::runtime_error("cannot write status to stdout");
}

void print_changes(const xnet_canopen_status_t& previous, const xnet_canopen_status_t& current) {
    if (previous.nmt_state != current.nmt_state)
        std::cerr << "NMT changed: " << nmt_name(previous.nmt_state) << " -> " << nmt_name(current.nmt_state) << '\n';
    if (previous.runtime_state != current.runtime_state)
        std::cerr << "Runtime changed: " << runtime_name(previous.runtime_state) << " -> "
                  << runtime_name(current.runtime_state) << '\n';
    if (previous.can_error_status != current.can_error_status)
        std::cerr << "CAN errors changed: 0x" << std::hex << previous.can_error_status << " -> 0x"
                  << current.can_error_status << std::dec << '\n';
    if (previous.node_guarding_timeout != current.node_guarding_timeout)
        std::cerr << "Node guarding timeout changed: " << current.node_guarding_timeout << '\n';
    const uint32_t node_resets = current.reset_node_count - previous.reset_node_count;
    const uint32_t communication_resets = current.reset_communication_count - previous.reset_communication_count;
    if (node_resets || communication_resets)
        std::cerr << "Completed resets since last snapshot: node=" << node_resets
                  << " communication=" << communication_resets << '\n';
}

bool run(const Api& api, Device& device, uint32_t duration_seconds) {
    const auto begin = Clock::now();
    auto next = begin;
    const auto period = std::chrono::milliseconds(500);
    xnet_canopen_status_t previous{};
    bool have_previous = false;
    while (!shutdown_signal) {
        const auto now = Clock::now();
        if (duration_seconds && now - begin >= std::chrono::seconds(duration_seconds)) {
            std::cerr << "Requested duration elapsed.\n";
            break;
        }
        if (now >= next) {
            xnet_canopen_status_t status{};
            if (!api.check(api.get_status(device.handle, &status), "xnet_canopen_get_status", device.handle)) return false;
            print_status(status);
            if (have_previous) print_changes(previous, status);
            previous = status;
            have_previous = true;
            if (status.runtime_state != XNET_CO_RUNTIME_RUNNING) {
                std::cerr << "Device is no longer running; shutting down without automatic recovery.\n";
                api.diagnostic(device.handle);
                return false;
            }
            next += period;
            if (next <= Clock::now()) next = Clock::now() + period;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }
    if (shutdown_signal) std::cerr << "Shutdown requested by signal " << shutdown_signal << ".\n";
    return true;
}
}

int main(int argc, char** argv) {
    if (argc == 2 && std::string(argv[1]) == "--help") {
        usage(std::cout);
        return EXIT_SUCCESS;
    }
    try {
        const auto options = parse_options(argc, argv);
        const auto config = read_config(options.config);
        if (std::signal(SIGINT, request_shutdown) == SIG_ERR || std::signal(SIGTERM, request_shutdown) == SIG_ERR)
            throw std::runtime_error("cannot install shutdown signal handlers");
#ifndef _WIN32
        // A closed output pipe should fail through normal cleanup, not SIGPIPE.
        if (std::signal(SIGPIPE, SIG_IGN) == SIG_ERR) throw std::runtime_error("cannot ignore SIGPIPE");
#endif
        Library library(options.library);
        const Api api(library);
        Device device(api, library);
        std::cerr << "Library: " << options.library << "\nRunner config: " << options.config
                  << "\nInterface: " << config.interface_name << "\nModel: " << config.model
                  << "\nPersistence: " << config.persistence << (fs::exists(config.persistence) ? " (exists)" : " (not yet created)")
                  << "\nSaved LSS settings may override model startup settings; this app never deletes persistence."
                  << "\nUse a library built with the matching 40-byte status ABI."
                  << "\nNative log location is fixed when the library is built; its directory must be writable."
                  << "\nThis is a passive slave: an external master controls NMT; no application/drive model is supplied.\n";
        bool ok = false;
        try {
            if (!api.check(api.create(&device.handle), "xnet_canopen_create", device.handle))
                throw std::runtime_error("device creation failed");
            if (!shutdown_signal) {
                device.initialize_attempted = true;
                const auto model = config.model.string();
                const auto persistence = config.persistence.string();
                if (!api.check(api.initialize(device.handle, config.interface_name.c_str(), model.c_str(), persistence.c_str()),
                               "xnet_canopen_initialize", device.handle))
                    throw std::runtime_error("device initialization failed");
                device.initialized = true;
                api.diagnostic(device.handle);
                if (!shutdown_signal && !api.check(api.start(device.handle), "xnet_canopen_start", device.handle))
                    throw std::runtime_error("device start failed");
            }
            ok = run(api, device, options.duration_seconds);
        } catch (const std::exception& error) {
            std::cerr << "Error: " << error.what() << '\n';
        }
        const bool clean = device.cleanup();
        const bool unloaded = library.close();
        return ok && clean && unloaded ? EXIT_SUCCESS : EXIT_FAILURE;
    } catch (const std::exception& error) {
        std::cerr << "Error: " << error.what() << '\n';
        usage(std::cerr);
        return EXIT_FAILURE;
    }
}
