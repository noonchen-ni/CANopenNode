#include <chrono>
#include <csignal>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <stdexcept>
#include <string>
#include <thread>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#else
#include <cerrno>
#include <fcntl.h>
#include <sys/wait.h>
#include <unistd.h>
#endif

namespace {
std::string read_output(const std::filesystem::path& path) {
    std::ifstream input(path);
    return {std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>()};
}
}

int main(int argc, char** argv) {
    if (argc != 6) {
        std::cerr << "Expected app, library, config, output file and signal (INT or TERM).\n";
        return EXIT_FAILURE;
    }
    const std::string requested = argv[5];
    if (requested != "INT" && requested != "TERM") return EXIT_FAILURE;
    const std::filesystem::path output = argv[4];
#ifdef _WIN32
    // A private console prevents the test's Ctrl+C from reaching the test runner.
    SECURITY_ATTRIBUTES security{sizeof(SECURITY_ATTRIBUTES), nullptr, TRUE};
    HANDLE file = CreateFileW(output.c_str(), GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE,
                              &security, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE) {
        std::cerr << "Cannot create signal-test output: " << GetLastError() << '\n';
        return EXIT_FAILURE;
    }
    STARTUPINFOW startup{};
    startup.cb = sizeof(startup);
    startup.dwFlags = STARTF_USESTDHANDLES | STARTF_USESHOWWINDOW;
    startup.wShowWindow = SW_HIDE;
    startup.hStdInput = nullptr;
    startup.hStdOutput = file;
    startup.hStdError = file;
    PROCESS_INFORMATION process{};
    const auto quote = [](const char* path) { return L"\"" + std::filesystem::path(path).wstring() + L"\""; };
    auto command = quote(argv[1]) + L" --lib " + quote(argv[2]) + L" --config " + quote(argv[3]);
    const bool created = CreateProcessW(nullptr, command.data(), nullptr, nullptr, TRUE,
                                       CREATE_NEW_CONSOLE, nullptr, nullptr, &startup, &process) != 0;
    const auto create_error = GetLastError();
    CloseHandle(file);
    if (!created) {
        std::cerr << "Cannot launch signal-test child: " << create_error << '\n';
        return EXIT_FAILURE;
    }
    CloseHandle(process.hThread);
#else
    const int file = open(output.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0600);
    if (file < 0) {
        std::cerr << "Cannot create signal-test output: " << errno << '\n';
        return EXIT_FAILURE;
    }
    const pid_t child = fork();
    if (child < 0) {
        close(file);
        std::cerr << "fork failed: " << errno << '\n';
        return EXIT_FAILURE;
    }
    if (child == 0) {
        if (dup2(file, STDOUT_FILENO) < 0 || dup2(file, STDERR_FILENO) < 0) _exit(126);
        close(file);
        execl(argv[1], argv[1], "--lib", argv[2], "--config", argv[3], static_cast<char*>(nullptr));
        _exit(127);
    }
    close(file);
    bool child_reaped = false;
#endif
    bool ok = false;
    try {
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
        while (read_output(output).find("runtime_state=RUNNING") == std::string::npos) {
            if (std::chrono::steady_clock::now() >= deadline) throw std::runtime_error("child did not become ready");
            std::this_thread::sleep_for(std::chrono::milliseconds(25));
        }
#ifdef _WIN32
        FreeConsole();
        if (!AttachConsole(process.dwProcessId)) throw std::runtime_error("cannot attach to child's private console");
        if (!SetConsoleCtrlHandler(nullptr, TRUE)) throw std::runtime_error("cannot ignore Ctrl+C in test helper");
        if (!GenerateConsoleCtrlEvent(CTRL_C_EVENT, 0)) throw std::runtime_error("cannot send Ctrl+C");
        if (WaitForSingleObject(process.hProcess, 10000) != WAIT_OBJECT_0)
            throw std::runtime_error("child did not shut down after Ctrl+C");
        DWORD code = 1;
        if (!GetExitCodeProcess(process.hProcess, &code) || code != 0)
            throw std::runtime_error("child returned a failure exit code");
#else
        const int signal = requested == "INT" ? SIGINT : SIGTERM;
        if (kill(child, signal) != 0) throw std::runtime_error("cannot signal child");
        int status = 0;
        while (true) {
            const auto result = waitpid(child, &status, WNOHANG);
            if (result == child) {
                child_reaped = true;
                break;
            }
            if (result < 0) throw std::runtime_error("waitpid failed");
            if (std::chrono::steady_clock::now() >= deadline) throw std::runtime_error("child did not shut down");
            std::this_thread::sleep_for(std::chrono::milliseconds(25));
        }
        if (!WIFEXITED(status) || WEXITSTATUS(status) != 0)
            throw std::runtime_error("child returned a failure exit code");
#endif
        const auto text = read_output(output);
        if (text.find("Shutdown requested by signal") == std::string::npos
            || text.find("Device destroyed; cleanup complete.") == std::string::npos)
            throw std::runtime_error("signal did not reach the graceful cleanup path");
        ok = true;
    } catch (const std::exception& error) {
        std::cerr << "Signal test failed: " << error.what() << '\n' << read_output(output);
    }
#ifdef _WIN32
    if (!ok) {
        TerminateProcess(process.hProcess, 1);
        WaitForSingleObject(process.hProcess, 10000);
    }
    CloseHandle(process.hProcess);
#else
    if (!ok && !child_reaped) {
        kill(child, SIGKILL);
        waitpid(child, nullptr, 0);
    }
#endif
    if (ok) std::cout << "Signal shutdown and native cleanup passed.\n";
    return ok ? EXIT_SUCCESS : EXIT_FAILURE;
}
