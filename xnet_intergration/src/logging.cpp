/**
 * @file
 * Private spdlog file backend shared by all device handles.
 * No global spdlog registry/default logger is changed. Synchronous, low-volume
 * events avoid an unbounded async queue and are flushed before a log call returns.
 */
#include "internal.hpp"
#include "logging_config.h"

#include "spdlog/logger.h"
#include "spdlog/sinks/rotating_file_sink.h"

#include <algorithm>
#include <cstdio>
#include <cstring>

namespace xco {
uint64_t next_log_instance() noexcept {
    static std::atomic<uint64_t> next{1};
    return next.fetch_add(1, std::memory_order_relaxed);
}

std::shared_ptr<spdlog::logger> file_logger() {
    if (!std::filesystem::path(XNET_CANOPEN_LOG_PATH).is_absolute())
        throw Error(XNET_CO_IO_ERROR, "XNET_CANOPEN_LOG_PATH must be an absolute file path");
    try {
        // Function-local initialization is thread-safe and retries after a failed
        // open. One sink serializes rotation/writes from every handle and worker.
        static const auto logger = [] {
            auto sink = std::make_shared<spdlog::sinks::rotating_file_sink_mt>(
                XNET_CANOPEN_LOG_PATH, 5U * 1024U * 1024U, 3U);
            auto result = std::make_shared<spdlog::logger>("xnet_canopen", sink);
            result->set_pattern("[%Y-%m-%d %H:%M:%S.%e] [pid=%P tid=%t] [%l] %v");
            result->set_level(spdlog::level::info);
            result->flush_on(spdlog::level::info);
            // spdlog normally handles sink errors internally. Propagate them to
            // the integration so failures are also visible through diagnostics.
            result->set_error_handler([](const std::string& message) { throw spdlog::spdlog_ex(message); });
            result->info("file logging initialized: {}", XNET_CANOPEN_LOG_PATH);
            return result;
        }();
        return logger;
    } catch (const spdlog::spdlog_ex& e) {
        throw Error(XNET_CO_IO_ERROR, std::string("cannot initialize file logging at ")
            + XNET_CANOPEN_LOG_PATH + ": " + e.what());
    }
}
} // namespace xco

void xnet_canopen_device::log(xco::LogLevel level, std::string_view message) noexcept {
    if (!logger) return; // Logging starts during initialize, not handle allocation.
    const auto severity = level == xco::LogLevel::error ? spdlog::level::err
        : level == xco::LogLevel::warning ? spdlog::level::warn : spdlog::level::info;
    try {
        logger->log(severity,
            "[device={} interface={} identity={:08x}:{:08x}:{:08x}:{:08x} node={} kbps={}] {}",
            log_instance, interface_name.empty() ? std::string_view("-") : std::string_view(interface_name),
            log_identity[0], log_identity[1], log_identity[2], log_identity[3],
            co ? unsigned(active_node) : 0U, unsigned(active_bitrate), message);
    } catch (const std::exception& e) {
        log_failure(e.what());
    }
}

void xnet_canopen_device::log_failure(const char* message) noexcept {
    // Avoid allocation and recursive logging, including after a formatting failure.
    std::fprintf(stderr, "xnet_canopen device=%llu: file logging failed: %s\n",
        static_cast<unsigned long long>(log_instance), message);
    std::fflush(stderr);
    const bool primary = error_code == XNET_CO_OK;
    auto used = primary ? size_t{0} : std::strlen(error_message.data());
    if (primary) error_code = XNET_CO_IO_ERROR;
    const auto append = [&](std::string_view text) {
        const auto size = std::min(text.size(), error_message.size() - 1 - used);
        std::memcpy(error_message.data() + used, text.data(), size);
        used += size;
        error_message[used] = 0;
    };
    append(primary ? "file logging failed: " : "; file logging failed: ");
    append(message);
}
