#pragma once

#ifdef _WIN32
#include <windows.h>
#include <string>
#else
#include <spawn.h>
extern char **environ;
#endif

namespace RoboGo::platform_util {

// Return to the competition picker in a fresh process. The legacy client
// keeps several panel selections and ImGui settings in process-static storage.
inline bool RestartAtCompetitionLauncher() {
#ifdef _WIN32
    std::wstring exe(32768, L'\0');
    const DWORD size = GetModuleFileNameW(nullptr, exe.data(), static_cast<DWORD>(exe.size()));
    if (!size || size >= exe.size()) return false;
    exe.resize(size);
    std::wstring command = L"\"" + exe + L"\" --welcome";
    STARTUPINFOW startup{};
    startup.cb = sizeof(startup);
    PROCESS_INFORMATION process{};
    if (!CreateProcessW(exe.c_str(), command.data(), nullptr, nullptr, FALSE, 0, nullptr,
                        nullptr, &startup, &process)) return false;
    CloseHandle(process.hThread);
    CloseHandle(process.hProcess);
    return true;
#else
    char path[] = "/proc/self/exe";
    char *argv[]{path, nullptr};
    pid_t pid;
    return posix_spawn(&pid, path, nullptr, nullptr, argv, environ) == 0;
#endif
}

} // namespace RoboGo::platform_util
