/*
 * File:        decoder_process.cpp
 * Module:      orc-stage-plugin-videodat
 * Purpose:     Runs videodat_decoder.py as a child process and streams its
 *              console output line by line
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2026 itewreed
 */

#include "decoder_process.h"

#if defined(_WIN32)
#include <windows.h>
#else
#include <dlfcn.h>
#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <spawn.h>
#include <sys/wait.h>
#include <unistd.h>
extern char** environ;
#endif

#include <cerrno>
#include <cstring>

namespace orc::plugins::videodat {

namespace {

// Splits |chunk| into lines, keeping an unterminated tail in |pending|.
// A carriage return (progress output) ends a non-empty line.
void feed(std::string& pending, const char* data, size_t n,
          const std::function<void(const std::string&)>& on_line)
{
    for (size_t i = 0; i < n; ++i) {
        const char c = data[i];
        if (c == '\n') {
            // Empty lines are kept: they structure the decoder's report.
            on_line(pending);
            pending.clear();
        } else if (c == '\r') {
            if (!pending.empty()) {
                on_line(pending);
                pending.clear();
            }
        } else {
            pending.push_back(c);
        }
    }
}

} // namespace

#if defined(_WIN32)

std::string plugin_directory()
{
    HMODULE hmod = nullptr;
    static const char kAnchor = 0;
    if (!GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                                GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                            &kAnchor, &hmod)) {
        return {};
    }
    char buf[MAX_PATH] = {};
    if (GetModuleFileNameA(hmod, buf, MAX_PATH) == 0) {
        return {};
    }
    std::string path(buf);
    const auto sep = path.find_last_of("\\/");
    return sep == std::string::npos ? std::string() : path.substr(0, sep + 1);
}

namespace {
std::string quote_arg(const std::string& a)
{
    if (!a.empty() && a.find_first_of(" \t\"") == std::string::npos) {
        return a;
    }
    std::string out = "\"";
    size_t backslashes = 0;
    for (char c : a) {
        if (c == '\\') {
            ++backslashes;
        } else if (c == '"') {
            out.append(backslashes * 2 + 1, '\\');
            backslashes = 0;
        } else {
            out.append(backslashes, '\\');
            backslashes = 0;
        }
        if (c != '\\') {
            out.push_back(c);
        }
    }
    out.append(backslashes * 2, '\\');
    out.push_back('"');
    return out;
}
} // namespace

ProcessResult run_process(const std::vector<std::string>& argv,
                          const std::function<void(const std::string&)>& on_line,
                          const std::atomic<bool>& cancel)
{
    ProcessResult result;
    SECURITY_ATTRIBUTES sa{sizeof(sa), nullptr, TRUE};
    HANDLE rd = nullptr, wr = nullptr;
    if (!CreatePipe(&rd, &wr, &sa, 0)) {
        result.error = "CreatePipe failed";
        return result;
    }
    SetHandleInformation(rd, HANDLE_FLAG_INHERIT, 0);

    std::string cmd;
    for (const auto& a : argv) {
        cmd += (cmd.empty() ? "" : " ") + quote_arg(a);
    }
    STARTUPINFOA si{};
    si.cb = sizeof(si);
    si.dwFlags = STARTF_USESTDHANDLES;
    si.hStdOutput = wr;
    si.hStdError = wr;
    si.hStdInput = GetStdHandle(STD_INPUT_HANDLE);
    PROCESS_INFORMATION pi{};
    std::vector<char> cmdline(cmd.begin(), cmd.end());
    cmdline.push_back('\0');
    if (!CreateProcessA(nullptr, cmdline.data(), nullptr, nullptr, TRUE,
                        CREATE_NO_WINDOW, nullptr, nullptr, &si, &pi)) {
        CloseHandle(rd);
        CloseHandle(wr);
        result.error = "cannot start " + argv.front();
        return result;
    }
    CloseHandle(wr);
    result.started = true;

    std::string pending;
    char buf[4096];
    for (;;) {
        if (cancel.load()) {
            TerminateProcess(pi.hProcess, 1);
            result.cancelled = true;
        }
        DWORD avail = 0;
        if (!PeekNamedPipe(rd, nullptr, 0, nullptr, &avail, nullptr)) {
            break;  // writer closed
        }
        if (avail == 0) {
            if (WaitForSingleObject(pi.hProcess, 100) == WAIT_OBJECT_0) {
                // drain what is left
                DWORD n = 0;
                while (PeekNamedPipe(rd, nullptr, 0, nullptr, &avail, nullptr) && avail > 0 &&
                       ReadFile(rd, buf, sizeof(buf), &n, nullptr) && n > 0) {
                    feed(pending, buf, n, on_line);
                }
                break;
            }
            continue;
        }
        DWORD n = 0;
        if (!ReadFile(rd, buf, sizeof(buf), &n, nullptr) || n == 0) {
            break;
        }
        feed(pending, buf, n, on_line);
    }
    if (!pending.empty()) {
        on_line(pending);
    }
    WaitForSingleObject(pi.hProcess, INFINITE);
    DWORD code = 1;
    GetExitCodeProcess(pi.hProcess, &code);
    result.exit_code = static_cast<int>(code);
    CloseHandle(pi.hProcess);
    CloseHandle(pi.hThread);
    CloseHandle(rd);
    return result;
}

#else

std::string plugin_directory()
{
    Dl_info info{};
    if (dladdr(reinterpret_cast<const void*>(&plugin_directory), &info) == 0 ||
        info.dli_fname == nullptr) {
        return {};
    }
    std::string path(info.dli_fname);
    const auto sep = path.rfind('/');
    return sep == std::string::npos ? std::string() : path.substr(0, sep + 1);
}

ProcessResult run_process(const std::vector<std::string>& argv,
                          const std::function<void(const std::string&)>& on_line,
                          const std::atomic<bool>& cancel)
{
    ProcessResult result;
    int fds[2];
    if (pipe(fds) != 0) {
        result.error = std::string("pipe: ") + std::strerror(errno);
        return result;
    }
    fcntl(fds[0], F_SETFD, FD_CLOEXEC);

    posix_spawn_file_actions_t actions;
    posix_spawn_file_actions_init(&actions);
    posix_spawn_file_actions_adddup2(&actions, fds[1], STDOUT_FILENO);
    posix_spawn_file_actions_adddup2(&actions, fds[1], STDERR_FILENO);
    posix_spawn_file_actions_addclose(&actions, fds[1]);

    std::vector<char*> cargv;
    for (const auto& a : argv) {
        cargv.push_back(const_cast<char*>(a.c_str()));
    }
    cargv.push_back(nullptr);

    pid_t pid = 0;
    const int rc = posix_spawnp(&pid, cargv[0], &actions, nullptr, cargv.data(), environ);
    posix_spawn_file_actions_destroy(&actions);
    close(fds[1]);
    if (rc != 0) {
        close(fds[0]);
        result.error = "cannot start " + argv.front() + ": " + std::strerror(rc);
        return result;
    }
    result.started = true;

    std::string pending;
    char buf[4096];
    bool killed = false;
    for (;;) {
        if (cancel.load() && !killed) {
            kill(pid, SIGTERM);
            killed = true;
            result.cancelled = true;
        }
        pollfd p{fds[0], POLLIN, 0};
        const int pr = poll(&p, 1, 200);
        if (pr < 0) {
            if (errno == EINTR) {
                continue;
            }
            break;
        }
        if (pr == 0) {
            continue;
        }
        const ssize_t n = read(fds[0], buf, sizeof(buf));
        if (n < 0 && errno == EINTR) {
            continue;
        }
        if (n <= 0) {
            break;  // EOF: child closed its output
        }
        feed(pending, buf, static_cast<size_t>(n), on_line);
    }
    if (!pending.empty()) {
        on_line(pending);
    }
    close(fds[0]);

    int status = 0;
    while (waitpid(pid, &status, 0) < 0 && errno == EINTR) {
    }
    if (WIFEXITED(status)) {
        result.exit_code = WEXITSTATUS(status);
    } else if (WIFSIGNALED(status)) {
        result.exit_code = 128 + WTERMSIG(status);
    }
    // posix_spawnp reports a missing executable through exit code 127.
    if (result.exit_code == 127 && !result.cancelled) {
        result.error = "cannot start " + argv.front() + " (not found?)";
    }
    return result;
}

#endif

} // namespace orc::plugins::videodat
