#include "internal.hpp"

#include <fstream>
#include <cstring>
#include <system_error>
#ifdef _WIN32
#define NOMINMAX
#include <windows.h>
#else
#include <cerrno>
#include <fcntl.h>
#include <unistd.h>
#endif

namespace xco {
Json read_json(const std::filesystem::path& path) {
    std::error_code ec;
    auto size = std::filesystem::file_size(path, ec);
    require(!ec && size <= 4 * 1024 * 1024, XNET_CO_IO_ERROR,
            "cannot read JSON file (missing, inaccessible, or over 4 MiB): " + path.string());
    std::ifstream file(path, std::ios::binary);
    require(file.is_open(), XNET_CO_IO_ERROR, "cannot open " + path.string());
    try {
        Json result; file >> result;
        require(!file.bad(), XNET_CO_IO_ERROR, "read failed: " + path.string());
        file >> std::ws;
        require(file.eof(), XNET_CO_MODEL_ERROR, "trailing data in " + path.string());
        return result;
    } catch (const Json::exception& e) {
        throw Error(XNET_CO_MODEL_ERROR, path.string() + ": " + e.what());
    }
}

void write_json_atomic(const std::filesystem::path& path, const Json& value) {
    auto directory = path.parent_path();
    require(!directory.empty(), XNET_CO_IO_ERROR, "persistence path must have a parent directory");
    static std::atomic<uint64_t> sequence{0};
    const auto temporary = std::filesystem::path(path.string() + ".tmp."
        + std::to_string(Clock::now().time_since_epoch().count()) + "." + std::to_string(sequence++));
    const std::string bytes = value.dump(2) + "\n";
#ifdef _WIN32
    HANDLE file = CreateFileW(temporary.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_NEW, FILE_ATTRIBUTE_NORMAL, nullptr);
    require(file != INVALID_HANDLE_VALUE, XNET_CO_IO_ERROR, "cannot create " + temporary.string());
    DWORD written = 0;
    bool ok = WriteFile(file, bytes.data(), static_cast<DWORD>(bytes.size()), &written, nullptr)
        && written == bytes.size() && FlushFileBuffers(file);
    bool closed = CloseHandle(file) != 0;
    if (!ok || !closed || !MoveFileExW(temporary.c_str(), path.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
        std::error_code ec; std::filesystem::remove(temporary, ec);
        throw Error(XNET_CO_IO_ERROR, "persistence replacement failed: " + path.string());
    }
#else
    int fd = ::open(temporary.c_str(), O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC, 0600);
    require(fd >= 0, XNET_CO_IO_ERROR, "cannot create " + temporary.string() + ": " + std::strerror(errno));
    size_t offset = 0;
    bool ok = true;
    while (offset < bytes.size()) {
        const auto written = ::write(fd, bytes.data() + offset, bytes.size() - offset);
        if (written < 0 && errno == EINTR) continue;
        if (written <= 0) { ok = false; break; }
        offset += static_cast<size_t>(written);
    }
    if (ok) ok = ::fsync(fd) == 0;
    if (::close(fd) != 0) ok = false;
    if (!ok || ::rename(temporary.c_str(), path.c_str()) != 0) {
        const std::string reason = std::strerror(errno);
        std::error_code ec; std::filesystem::remove(temporary, ec);
        throw Error(XNET_CO_IO_ERROR, "persistence replacement failed: " + path.string() + ": " + reason);
    }
    int parent = ::open(directory.c_str(), O_RDONLY | O_DIRECTORY | O_CLOEXEC);
    require(parent >= 0, XNET_CO_IO_ERROR, "cannot open persistence directory: " + directory.string());
    const bool synced = ::fsync(parent) == 0;
    const bool closed = ::close(parent) == 0;
    require(synced && closed, XNET_CO_IO_ERROR, "cannot synchronize persistence directory: " + directory.string());
#endif
}
} // namespace xco
