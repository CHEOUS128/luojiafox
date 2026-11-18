#pragma once
#include <string_view>
#ifdef _WIN32
#include <windows.h>
#include <array>
#include <string>
#else
#include <spawn.h>
#include <sys/wait.h>
#include <unistd.h>
extern char **environ;
#endif

namespace RoboGo::platform_util {

// Both targets are receive-free local renderers in this same executable. Never
// forward a host name, vehicle identity, current options or arbitrary arguments.
inline bool OpenWindowCheck(std::string_view option) {
    const bool uc = option == "--window-check-uc";
    if (!uc && option != "--window-check-ul") return false;
#ifdef _WIN32
    struct Child {
        HANDLE process = nullptr;
        DWORD pid = 0;
        ~Child() { if (process) CloseHandle(process); }
    };
    static std::array<Child, 2> children;
    auto &child = children[uc ? 1 : 0];
    if (child.process) {
        const DWORD state = WaitForSingleObject(child.process, 0);
        if (state == WAIT_OBJECT_0) {
            CloseHandle(child.process);
            child.process = nullptr;
        } else if (state == WAIT_TIMEOUT) {
            struct Search { DWORD pid; HWND window; } found{child.pid, nullptr};
            EnumWindows([](HWND hwnd, LPARAM opaque) -> BOOL {
                auto *s = reinterpret_cast<Search *>(opaque);
                DWORD pid = 0; GetWindowThreadProcessId(hwnd, &pid);
                if (pid == s->pid && IsWindowVisible(hwnd) && !GetWindow(hwnd,GW_OWNER)) {
                    s->window = hwnd; return FALSE;
                }
                return TRUE;
            }, reinterpret_cast<LPARAM>(&found));
            if (found.window) {
                if (IsIconic(found.window)) ShowWindow(found.window, SW_RESTORE);
                SetForegroundWindow(found.window);
            }
            return true;
        } else return false;
    }
    std::wstring exe(32768, L'\0');
    const auto n = GetModuleFileNameW(nullptr, exe.data(), static_cast<DWORD>(exe.size()));
    if (!n || n >= exe.size()) return false;
    exe.resize(n);
    std::wstring command = L"\"" + exe + (uc ? L"\" --window-check-uc" : L"\" --window-check-ul");
    STARTUPINFOW si{}; si.cb = sizeof(si);
    PROCESS_INFORMATION pi{};
    if (!CreateProcessW(exe.c_str(), command.data(), nullptr, nullptr, FALSE,
                        0, nullptr, nullptr, &si, &pi)) return false;
    CloseHandle(pi.hThread);
    child.process = pi.hProcess;
    child.pid = pi.dwProcessId;
    return true;
#else
    static pid_t children[2]{};
    auto &pid = children[uc ? 1 : 0];
    if (pid > 0 && waitpid(pid, nullptr, WNOHANG) == 0) return true;
    char path[] = "/proc/self/exe", ul_arg[] = "--window-check-ul", uc_arg[] = "--window-check-uc";
    char *argv[]{path, uc ? uc_arg : ul_arg, nullptr};
    return posix_spawn(&pid, path, nullptr, nullptr, argv, environ) == 0;
#endif
}
} // namespace RoboGo::platform_util
