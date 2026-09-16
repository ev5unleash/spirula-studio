#include "app/Subprocess.h"

#include <algorithm>
#include <cctype>
#include <cerrno>
#include <chrono>
#include <cstring>
#include <filesystem>
#include <map>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#else
#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <sys/wait.h>
#include <unistd.h>
#include <cstdlib>
#ifdef __linux__
#include <sys/prctl.h>
#endif
#endif

namespace fs = std::filesystem;

namespace app::proc {

namespace {

void emit_lines(std::string& acc, const char* buf, size_t n,
                const std::function<void(const std::string&)>& on_line) {
    acc.append(buf, n);
    size_t pos = 0, nl;
    while ((nl = acc.find_first_of("\r\n", pos)) != std::string::npos) {
        if (acc[nl] == '\r' && nl + 1 >= acc.size())
            break;
        size_t next = nl + 1;
        if (acc[nl] == '\r' && acc[next] == '\n') next++;
        if (nl > pos && on_line) on_line(acc.substr(pos, nl - pos));
        pos = next;
    }
    acc.erase(0, pos);
}

void emit_tail(std::string& acc, const std::function<void(const std::string&)>& on_line) {
    if (!acc.empty() && on_line) on_line(acc);
    acc.clear();
}

}  // namespace

#ifdef _WIN32

namespace {

std::wstring utf8_to_wide(const std::string& s) {
    if (s.empty()) return std::wstring();
    int n = MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), nullptr, 0);
    if (n <= 0) return std::wstring();
    std::wstring w(n, 0);
    MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), w.data(), n);
    return w;
}

std::wstring quote_arg_w(const std::wstring& a) {
    if (!a.empty() && a.find_first_of(L" \t\"") == std::wstring::npos) return a;
    std::wstring out = L"\"";
    size_t bs = 0;
    for (wchar_t c : a) {
        if (c == L'\\') { bs++; continue; }
        if (c == L'"') { out.append(bs * 2 + 1, L'\\'); out += L'"'; bs = 0; continue; }
        out.append(bs, L'\\'); bs = 0;
        out += c;
    }
    out.append(bs * 2, L'\\');
    out += L'"';
    return out;
}

struct CaseInsensitiveWCompare {
    bool operator()(const std::wstring& a, const std::wstring& b) const {
        return _wcsicmp(a.c_str(), b.c_str()) < 0;
    }
};

}  // namespace

ProcessResult run_process(const ProcessOptions& options) {
    if (options.argv.empty()) {
        return {ProcessOutcome::SpawnFailed, -1, "argv cannot be empty"};
    }

    std::wstring cmdline;
    for (size_t i = 0; i < options.argv.size(); ++i) {
        if (i > 0) cmdline += L" ";
        cmdline += quote_arg_w(utf8_to_wide(options.argv[i]));
    }

    std::map<std::wstring, std::wstring, CaseInsensitiveWCompare> env_map;
    LPWCH parent_env = GetEnvironmentStringsW();
    if (parent_env) {
        const wchar_t* p = parent_env;
        while (*p) {
            std::wstring entry = p;
            p += entry.size() + 1;
            size_t eq = entry.find(L'=', 1);
            if (eq != std::wstring::npos) {
                env_map[entry.substr(0, eq)] = entry.substr(eq + 1);
            }
        }
        FreeEnvironmentStringsW(parent_env);
    }
    for (const auto& [k, v] : options.env_overrides) {
        env_map[utf8_to_wide(k)] = utf8_to_wide(v);
    }
    std::vector<wchar_t> env_block;
    for (const auto& [k, v] : env_map) {
        env_block.insert(env_block.end(), k.begin(), k.end());
        env_block.push_back(L'=');
        env_block.insert(env_block.end(), v.begin(), v.end());
        env_block.push_back(L'\0');
    }
    env_block.push_back(L'\0');

    SECURITY_ATTRIBUTES sa{};
    sa.nLength = sizeof sa;
    sa.bInheritHandle = TRUE;

    HANDLE stdout_rd = nullptr, stdout_wr = nullptr;
    if (!CreatePipe(&stdout_rd, &stdout_wr, &sa, 0)) {
        return {ProcessOutcome::SpawnFailed, -1, "CreatePipe stdout failed"};
    }
    SetHandleInformation(stdout_rd, HANDLE_FLAG_INHERIT, 0);

    HANDLE stdin_rd = nullptr, stdin_wr = nullptr;
    if (!CreatePipe(&stdin_rd, &stdin_wr, &sa, 0)) {
        CloseHandle(stdout_rd);
        CloseHandle(stdout_wr);
        return {ProcessOutcome::SpawnFailed, -1, "CreatePipe stdin failed"};
    }
    SetHandleInformation(stdin_wr, HANDLE_FLAG_INHERIT, 0);

    std::vector<HANDLE> inherit_handles = { stdout_wr, stdin_rd };
    SIZE_T attr_size = 0;
    InitializeProcThreadAttributeList(nullptr, 1, 0, &attr_size);
    std::vector<uint8_t> attr_buf(attr_size);
    auto attr_list = reinterpret_cast<LPPROC_THREAD_ATTRIBUTE_LIST>(attr_buf.data());
    if (!InitializeProcThreadAttributeList(attr_list, 1, 0, &attr_size)) {
        CloseHandle(stdout_rd); CloseHandle(stdout_wr);
        CloseHandle(stdin_rd); CloseHandle(stdin_wr);
        return {ProcessOutcome::SpawnFailed, -1, "InitializeProcThreadAttributeList failed"};
    }
    if (!UpdateProcThreadAttribute(attr_list, 0,
                                   PROC_THREAD_ATTRIBUTE_HANDLE_LIST,
                                   inherit_handles.data(),
                                   inherit_handles.size() * sizeof(HANDLE),
                                   nullptr, nullptr)) {
        DeleteProcThreadAttributeList(attr_list);
        CloseHandle(stdout_rd); CloseHandle(stdout_wr);
        CloseHandle(stdin_rd); CloseHandle(stdin_wr);
        return {ProcessOutcome::SpawnFailed, -1, "UpdateProcThreadAttribute failed"};
    }

    STARTUPINFOEXW siex{};
    siex.StartupInfo.cb = sizeof siex;
    siex.StartupInfo.dwFlags = STARTF_USESTDHANDLES;
    siex.StartupInfo.hStdOutput = stdout_wr;
    siex.StartupInfo.hStdError  = stdout_wr;
    siex.StartupInfo.hStdInput  = stdin_rd;
    siex.lpAttributeList = attr_list;

    HANDLE job = CreateJobObjectW(nullptr, nullptr);
    JOBOBJECT_EXTENDED_LIMIT_INFORMATION jeli{};
    jeli.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
    if (!job || !SetInformationJobObject(job, JobObjectExtendedLimitInformation,
                                         &jeli, sizeof(jeli))) {
        DeleteProcThreadAttributeList(attr_list);
        CloseHandle(stdout_rd); CloseHandle(stdout_wr);
        CloseHandle(stdin_rd); CloseHandle(stdin_wr);
        if (job) CloseHandle(job);
        return {ProcessOutcome::SpawnFailed, -1,
                "cannot establish process-tree ownership"};
    }

    PROCESS_INFORMATION pi{};
    DWORD creation_flags = CREATE_NO_WINDOW | CREATE_UNICODE_ENVIRONMENT |
                           CREATE_SUSPENDED | EXTENDED_STARTUPINFO_PRESENT;
    std::wstring wcwd = utf8_to_wide(options.cwd);
    std::vector<wchar_t> cmd_buf(cmdline.begin(), cmdline.end());
    cmd_buf.push_back(L'\0');

    BOOL ok = CreateProcessW(
        nullptr, cmd_buf.data(), nullptr, nullptr, TRUE,
        creation_flags, env_block.data(),
        wcwd.empty() ? nullptr : wcwd.c_str(),
        &siex.StartupInfo, &pi);

    DeleteProcThreadAttributeList(attr_list);
    CloseHandle(stdout_wr);
    CloseHandle(stdin_rd);

    if (!ok) {
        CloseHandle(stdout_rd);
        CloseHandle(stdin_wr);
        CloseHandle(job);
        return {ProcessOutcome::SpawnFailed, -1, "CreateProcessW failed: " + std::to_string(GetLastError())};
    }

    if (!AssignProcessToJobObject(job, pi.hProcess)) {
        const DWORD error = GetLastError();
        TerminateProcess(pi.hProcess, 1);
        WaitForSingleObject(pi.hProcess, INFINITE);
        CloseHandle(pi.hThread);
        CloseHandle(pi.hProcess);
        CloseHandle(stdout_rd);
        CloseHandle(stdin_wr);
        CloseHandle(job);
        return {ProcessOutcome::SpawnFailed, -1,
                "cannot assign process tree: " + std::to_string(error)};
    }
    if (ResumeThread(pi.hThread) == static_cast<DWORD>(-1)) {
        const DWORD error = GetLastError();
        TerminateJobObject(job, 1);
        WaitForSingleObject(pi.hProcess, INFINITE);
        CloseHandle(pi.hThread);
        CloseHandle(pi.hProcess);
        CloseHandle(stdout_rd);
        CloseHandle(stdin_wr);
        CloseHandle(job);
        return {ProcessOutcome::SpawnFailed, -1,
                "cannot resume process: " + std::to_string(error)};
    }
    CloseHandle(pi.hThread);

    std::string acc;
    char buf[4096];
    bool killed = false;
    bool stop_sent = false;
    auto stop_time = std::chrono::steady_clock::time_point::min();

    for (;;) {
        if (options.cancel && options.cancel->load() && !killed) {
            killed = true;
            TerminateJobObject(job, 1);
            TerminateProcess(pi.hProcess, 1);
            if (stdin_wr) {
                CloseHandle(stdin_wr);
                stdin_wr = nullptr;
            }
        }
        if (options.stop && options.stop->load() && !stop_sent && !killed) {
            stop_sent = true;
            stop_time = std::chrono::steady_clock::now();
            if (stdin_wr) {
                if (!options.stop_token.empty()) {
                    DWORD written = 0;
                    WriteFile(stdin_wr, options.stop_token.data(),
                              static_cast<DWORD>(options.stop_token.size()), &written, nullptr);
                }
                CloseHandle(stdin_wr);
                stdin_wr = nullptr;
            }
        }
        if (stop_sent && !killed) {
            auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
                std::chrono::steady_clock::now() - stop_time).count();
            if (elapsed > options.grace_period_ms) {
                killed = true;
                TerminateJobObject(job, 1);
                TerminateProcess(pi.hProcess, 1);
            }
        }

        DWORD avail = 0;
        if (!PeekNamedPipe(stdout_rd, nullptr, 0, nullptr, &avail, nullptr)) break;
        if (avail > 0) {
            DWORD got = 0;
            if (!ReadFile(stdout_rd, buf, static_cast<DWORD>(std::min<size_t>(sizeof buf, avail)), &got, nullptr) || !got)
                break;
            emit_lines(acc, buf, got, options.on_line);
        } else {
            if (WaitForSingleObject(pi.hProcess, 50) == WAIT_OBJECT_0) {
                while (PeekNamedPipe(stdout_rd, nullptr, 0, nullptr, &avail, nullptr) && avail) {
                    DWORD got = 0;
                    if (!ReadFile(stdout_rd, buf, static_cast<DWORD>(std::min<size_t>(sizeof buf, avail)), &got, nullptr) || !got)
                        break;
                    emit_lines(acc, buf, got, options.on_line);
                }
                break;
            }
        }
    }
    emit_tail(acc, options.on_line);
    if (stdin_wr) {
        CloseHandle(stdin_wr);
        stdin_wr = nullptr;
    }
    WaitForSingleObject(pi.hProcess, INFINITE);
    DWORD exit_code = 1;
    GetExitCodeProcess(pi.hProcess, &exit_code);
    CloseHandle(pi.hProcess);
    CloseHandle(job);
    CloseHandle(stdout_rd);

    ProcessResult res;
    res.exit_code = static_cast<int>(exit_code);
    if (killed) {
        res.outcome = ProcessOutcome::Cancelled;
    } else if (stop_sent) {
        res.outcome = ProcessOutcome::Stopped;
    } else if ((exit_code & 0xF0000000U) == 0xC0000000U) {
        res.outcome = ProcessOutcome::Crashed;
        res.error_message = "Process crashed (exception code " + std::to_string(exit_code) + ")";
    } else {
        res.outcome = ProcessOutcome::Success;
    }
    return res;
}

bool command_exists(const std::string& exe) {
    if (exe.find('\\') != std::string::npos || exe.find('/') != std::string::npos) {
        std::error_code ec;
        return fs::exists(fs::u8path(exe), ec);
    }
    wchar_t found[MAX_PATH];
    std::wstring wexe = utf8_to_wide(exe);
    return SearchPathW(nullptr, wexe.c_str(), L".exe", MAX_PATH, found, nullptr) > 0;
}

bool open_url(const std::string& url) {
    std::string cmd = "cmd /c start \"\" \"" + url + "\"";
    std::wstring wcmd = utf8_to_wide(cmd);
    STARTUPINFOW si{};
    si.cb = sizeof si;
    si.dwFlags = STARTF_USESHOWWINDOW;
    si.wShowWindow = SW_HIDE;
    PROCESS_INFORMATION pi{};
    std::vector<wchar_t> cmd_buf(wcmd.begin(), wcmd.end());
    cmd_buf.push_back(L'\0');
    if (!CreateProcessW(nullptr, cmd_buf.data(), nullptr, nullptr, FALSE,
                        CREATE_NO_WINDOW, nullptr, nullptr, &si, &pi))
        return false;
    CloseHandle(pi.hProcess);
    CloseHandle(pi.hThread);
    return true;
}

#else

namespace {

std::string resolve_exe_path(const std::string& exe) {
    if (exe.find('/') != std::string::npos) return exe;
    const char* path = std::getenv("PATH");
    if (!path) return "";
    std::string p = path;
    size_t pos = 0;
    while (pos <= p.size()) {
        size_t colon = p.find(':', pos);
        std::string dir = p.substr(pos, colon == std::string::npos ? std::string::npos : colon - pos);
        if (!dir.empty()) {
            std::string candidate = dir + "/" + exe;
            if (access(candidate.c_str(), X_OK) == 0) return candidate;
        }
        if (colon == std::string::npos) break;
        pos = colon + 1;
    }
    return "";
}

}  // namespace

ProcessResult run_process(const ProcessOptions& options) {
    if (options.argv.empty()) {
        return {ProcessOutcome::SpawnFailed, -1, "argv cannot be empty"};
    }
    std::string resolved = resolve_exe_path(options.argv[0]);
    if (resolved.empty()) {
        return {ProcessOutcome::SpawnFailed, -1, "executable not found: " + options.argv[0]};
    }

    std::vector<std::string> args_storage = options.argv;
    std::vector<char*> args;
    for (auto& s : args_storage) args.push_back(s.data());
    args.push_back(nullptr);

    extern char** environ;
    std::map<std::string, std::string> env_map;
    for (char** ep = environ; ep && *ep; ++ep) {
        std::string entry = *ep;
        size_t eq = entry.find('=');
        if (eq != std::string::npos) env_map[entry.substr(0, eq)] = entry.substr(eq + 1);
    }
    for (const auto& [k, v] : options.env_overrides) {
        env_map[k] = v;
    }
    std::vector<std::string> env_storage;
    for (const auto& [k, v] : env_map) env_storage.push_back(k + "=" + v);
    std::vector<char*> envp;
    for (auto& s : env_storage) envp.push_back(s.data());
    envp.push_back(nullptr);

    int stdout_fds[2];
    if (pipe(stdout_fds) != 0) {
        return {ProcessOutcome::SpawnFailed, -1, "pipe stdout failed"};
    }
    int stdin_fds[2];
    if (pipe(stdin_fds) != 0) {
        close(stdout_fds[0]); close(stdout_fds[1]);
        return {ProcessOutcome::SpawnFailed, -1, "pipe stdin failed"};
    }
    int liveness_fds[2];
    if (pipe(liveness_fds) != 0) {
        close(stdout_fds[0]); close(stdout_fds[1]);
        close(stdin_fds[0]); close(stdin_fds[1]);
        return {ProcessOutcome::SpawnFailed, -1, "pipe liveness failed"};
    }

    fcntl(stdout_fds[0], F_SETFD, FD_CLOEXEC);
    fcntl(stdin_fds[1], F_SETFD, FD_CLOEXEC);
    fcntl(liveness_fds[1], F_SETFD, FD_CLOEXEC);

    pid_t pid = fork();
    if (pid < 0) {
        close(stdout_fds[0]); close(stdout_fds[1]);
        close(stdin_fds[0]); close(stdin_fds[1]);
        close(liveness_fds[0]); close(liveness_fds[1]);
        return {ProcessOutcome::SpawnFailed, -1, "fork failed"};
    }

    if (pid == 0) {
        setpgid(0, 0);
#ifdef __linux__
        prctl(PR_SET_PDEATHSIG, SIGTERM);
#endif
        dup2(stdout_fds[1], STDOUT_FILENO);
        dup2(stdout_fds[1], STDERR_FILENO);
        dup2(stdin_fds[0], STDIN_FILENO);

        close(stdout_fds[0]); close(stdout_fds[1]);
        close(stdin_fds[0]); close(stdin_fds[1]);
        close(liveness_fds[1]);
        fcntl(liveness_fds[0], F_SETFD, FD_CLOEXEC);

        if (!options.cwd.empty() && chdir(options.cwd.c_str()) != 0) {
            _exit(127);
        }
        execve(resolved.c_str(), args.data(), envp.data());
        _exit(127);
    }

    close(stdout_fds[1]);
    close(stdin_fds[0]);
    close(liveness_fds[0]);

    std::string acc;
    char buf[4096];
    bool killed = false;
    bool stop_sent = false;
    auto stop_time = std::chrono::steady_clock::time_point::min();

    for (;;) {
        if (options.cancel && options.cancel->load() && !killed) {
            kill(-pid, SIGKILL);
            killed = true;
            if (stdin_fds[1] >= 0) {
                close(stdin_fds[1]);
                stdin_fds[1] = -1;
            }
        }
        if (options.stop && options.stop->load() && !stop_sent && !killed) {
            stop_sent = true;
            stop_time = std::chrono::steady_clock::now();
            if (stdin_fds[1] >= 0) {
                if (!options.stop_token.empty()) {
                    (void)write(stdin_fds[1], options.stop_token.data(), options.stop_token.size());
                }
                close(stdin_fds[1]);
                stdin_fds[1] = -1;
            }
        }
        if (stop_sent && !killed) {
            auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
                std::chrono::steady_clock::now() - stop_time).count();
            if (elapsed > options.grace_period_ms) {
                kill(-pid, SIGKILL);
                killed = true;
            }
        }

        struct pollfd pfd{stdout_fds[0], POLLIN, 0};
        int pr = poll(&pfd, 1, 50);
        if (pr > 0) {
            ssize_t got = read(stdout_fds[0], buf, sizeof buf);
            if (got <= 0) break;
            emit_lines(acc, buf, static_cast<size_t>(got), options.on_line);
        } else if (pr < 0 && errno != EINTR) {
            break;
        }
    }
    emit_tail(acc, options.on_line);
    close(stdout_fds[0]);
    if (stdin_fds[1] >= 0) close(stdin_fds[1]);
    close(liveness_fds[1]);

    int status = 0;
    waitpid(pid, &status, 0);

    ProcessResult res;
    if (killed) {
        res.outcome = ProcessOutcome::Cancelled;
        res.exit_code = -2;
    } else if (WIFSIGNALED(status)) {
        res.outcome = ProcessOutcome::Crashed;
        res.exit_code = 128 + WTERMSIG(status);
        res.error_message = "Terminated by signal " + std::to_string(WTERMSIG(status));
    } else if (WIFEXITED(status)) {
        int code = WEXITSTATUS(status);
        if (code == 127) {
            res.outcome = ProcessOutcome::SpawnFailed;
            res.exit_code = 127;
            res.error_message = "execve failed";
        } else if (stop_sent) {
            res.outcome = ProcessOutcome::Stopped;
            res.exit_code = code;
        } else {
            res.outcome = ProcessOutcome::Success;
            res.exit_code = code;
        }
    }
    return res;
}

bool command_exists(const std::string& exe) {
    if (exe.find('/') != std::string::npos)
        return access(exe.c_str(), X_OK) == 0;
    const char* path = std::getenv("PATH");
    if (!path) return false;
    std::string p = path;
    size_t pos = 0;
    while (pos <= p.size()) {
        size_t colon = p.find(':', pos);
        std::string dir = p.substr(pos, colon == std::string::npos ? std::string::npos : colon - pos);
        if (!dir.empty() && access((dir + "/" + exe).c_str(), X_OK) == 0)
            return true;
        if (colon == std::string::npos) break;
        pos = colon + 1;
    }
    return false;
}

bool open_url(const std::string& url) {
#ifdef __APPLE__
    const char* openers[] = {"open"};
#else
    const char* openers[] = {"xdg-open", "gio", "x-www-browser"};
#endif
    for (const char* opener : openers) {
        if (!command_exists(opener)) continue;
        const pid_t pid = fork();
        if (pid < 0) return false;
        if (pid == 0) {
            if (fork() == 0) {
                setsid();
                int devnull = open("/dev/null", O_RDWR);
                if (devnull >= 0) {
                    dup2(devnull, STDIN_FILENO);
                    dup2(devnull, STDOUT_FILENO);
                    dup2(devnull, STDERR_FILENO);
                    if (devnull > STDERR_FILENO) close(devnull);
                }
                if (std::strcmp(opener, "gio") == 0)
                    execlp(opener, opener, "open", url.c_str(), static_cast<char*>(nullptr));
                else
                    execlp(opener, opener, url.c_str(), static_cast<char*>(nullptr));
            }
            _exit(0);
        }
        int status = 0;
        waitpid(pid, &status, 0);
        return true;
    }
    return false;
}

#endif

std::vector<std::string> split_args(const std::string& s) {
    std::vector<std::string> out;
    std::string cur;
    bool in_quote = false, have = false;
    char quote = 0;
    for (char c : s) {
        if (in_quote) {
            if (c == quote) in_quote = false;
            else cur += c;
        } else if (c == '"' || c == '\'') {
            in_quote = true;
            quote = c;
            have = true;
        } else if (std::isspace(static_cast<unsigned char>(c))) {
            if (have) { out.push_back(cur); cur.clear(); have = false; }
        } else {
            cur += c;
            have = true;
        }
    }
    if (have) out.push_back(cur);
    return out;
}

}  // namespace app::proc
