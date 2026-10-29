#include "internal.hpp"
#include "logging_config.h"

#include <fstream>
#include <iostream>

int main() {
    const std::filesystem::path path(XNET_CANOPEN_LOG_PATH);
    try {
        std::filesystem::remove(path);
        if (!std::filesystem::create_directory(path)) throw std::runtime_error("cannot create blocked log path");
        bool rejected = false;
        try {
            xco::file_logger();
        } catch (const xco::Error& e) {
            rejected = e.code == XNET_CO_IO_ERROR
                && std::string(e.what()).find("cannot initialize file logging") != std::string::npos;
        }
        if (!rejected) throw std::runtime_error("logging to a directory was not reported as an I/O failure");
        std::filesystem::remove(path);
        xco::file_logger();
        std::ifstream file(path);
        std::string line;
        if (!std::getline(file, line) || line.find("file logging initialized") == std::string::npos)
            throw std::runtime_error("logging initialization did not recover after the path was repaired");
        std::cout << "PASS logging open failure and retry\n";
        return 0;
    } catch (const std::exception& e) {
        std::cerr << "FAIL logging open failure and retry: " << e.what() << "\n";
        return 1;
    }
}
