#include "platform_util.h"

#include <cerrno>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cwctype>

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <shellapi.h>
#else
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>
#endif

namespace cw {
namespace {

#ifdef _WIN32

bool g_console_checked = false;
bool g_out_is_console = false;
bool g_err_is_console = false;

void detect_console() {
    if (g_console_checked) return;
    g_console_checked = true;
    DWORD mode = 0;
    g_out_is_console = ::GetConsoleMode(::GetStdHandle(STD_OUTPUT_HANDLE), &mode) != 0;
    mode = 0;
    g_err_is_console = ::GetConsoleMode(::GetStdHandle(STD_ERROR_HANDLE), &mode) != 0;
}

std::wstring utf8_to_wide(const std::string & s) {
    if (s.empty()) return {};
    const int n = ::MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), nullptr, 0);
    if (n <= 0) return {};
    std::wstring w(static_cast<std::size_t>(n), L'\0');
    ::MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), w.data(), n);
    return w;
}

std::string wide_to_utf8(const std::wstring & w) {
    if (w.empty()) return {};
    const int n = ::WideCharToMultiByte(CP_UTF8, 0, w.data(), static_cast<int>(w.size()),
                                        nullptr, 0, nullptr, nullptr);
    if (n <= 0) return {};
    std::string s(static_cast<std::size_t>(n), '\0');
    ::WideCharToMultiByte(CP_UTF8, 0, w.data(), static_cast<int>(w.size()),
                          s.data(), n, nullptr, nullptr);
    return s;
}

std::string win32_error_text(DWORD code) {
    LPWSTR buf = nullptr;
    const DWORD n = ::FormatMessageW(
      FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS,
      nullptr, code, MAKELANGID(LANG_NEUTRAL, SUBLANG_DEFAULT),
      reinterpret_cast<LPWSTR>(&buf), 0, nullptr);
    std::string text;
    if (n != 0 && buf != nullptr) {
        text = wide_to_utf8(std::wstring(buf, n));
        while (!text.empty() && (text.back() == '\r' || text.back() == '\n' || text.back() == ' ')) {
            text.pop_back();
        }
    }
    if (buf != nullptr) ::LocalFree(buf);
    if (text.empty()) text = "错误码 " + std::to_string(code);
    return text;
}

// Windows 命令行引号规则（与 CommandLineToArgvW 对称）
std::wstring quote_arg(const std::wstring & arg) {
    bool need = arg.empty();
    for (const wchar_t c : arg) {
        if (c == L' ' || c == L'\t' || c == L'\n' || c == L'\v' || c == L'"') { need = true; break; }
    }
    if (!need) return arg;

    std::wstring out;
    out += L'"';
    for (std::size_t i = 0; i < arg.size(); ++i) {
        std::size_t backslashes = 0;
        while (i < arg.size() && arg[i] == L'\\') { ++backslashes; ++i; }
        if (i == arg.size()) {
            // 结尾的反斜杠要翻倍，否则会把收尾的引号转义掉
            out.append(backslashes * 2, L'\\');
        } else if (arg[i] == L'"') {
            out.append(backslashes * 2 + 1, L'\\');
            out += L'"';
        } else {
            out.append(backslashes, L'\\');
            out += arg[i];
        }
    }
    out += L'"';
    return out;
}

bool is_regular_file(const std::wstring & path) {
    const DWORD attr = ::GetFileAttributesW(path.c_str());
    return attr != INVALID_FILE_ATTRIBUTES && (attr & FILE_ATTRIBUTE_DIRECTORY) == 0;
}

std::wstring with_exe_ext(std::wstring p) {
    if (p.size() >= 4) {
        std::wstring tail = p.substr(p.size() - 4);
        // 只处理 ASCII 大小写，不碰 locale（towlower 在不同 CRL 上行为不一致）
        for (auto & c : tail) {
            if (c >= L'A' && c <= L'Z') c = static_cast<wchar_t>(c - L'A' + L'a');
        }
        if (tail == L".exe") return p;
    }
    p += L".exe";
    return p;
}

#endif  // _WIN32

// 先格式化，再决定走宽字符控制台还是直接写字节流
void vwrite_to(FILE * fp, bool is_console, const char * fmt, va_list ap) {
    char stack_buf[2048];
    va_list ap_copy;
    va_copy(ap_copy, ap);
    const int n = std::vsnprintf(stack_buf, sizeof(stack_buf), fmt, ap);
    if (n < 0) { va_end(ap_copy); return; }

    std::string msg;
    if (static_cast<std::size_t>(n) < sizeof(stack_buf)) {
        msg.assign(stack_buf, static_cast<std::size_t>(n));
    } else {
        msg.resize(static_cast<std::size_t>(n) + 1);
        std::vsnprintf(msg.data(), msg.size(), fmt, ap_copy);
        msg.resize(static_cast<std::size_t>(n));
    }
    va_end(ap_copy);

#ifdef _WIN32
    if (is_console) {
        // 控制台按 UTF-16 写：不受 chcp 影响，中文永远不会乱码
        const std::wstring w = utf8_to_wide(msg);
        std::wstring crlf;
        crlf.reserve(w.size() + 8);
        for (std::size_t i = 0; i < w.size(); ++i) {
            if (w[i] == L'\n' && (i == 0 || w[i - 1] != L'\r')) crlf += L'\r';
            crlf += w[i];
        }
        const HANDLE h = (fp == stderr) ? ::GetStdHandle(STD_ERROR_HANDLE)
                                        : ::GetStdHandle(STD_OUTPUT_HANDLE);
        DWORD written = 0;
        ::WriteConsoleW(h, crlf.c_str(), static_cast<DWORD>(crlf.size()), &written, nullptr);
        return;
    }
#else
    (void)is_console;
#endif
    std::fwrite(msg.data(), 1, msg.size(), fp);
}

}  // namespace

void init_console() {
#ifdef _WIN32
    detect_console();
    // ⚠ 这里**故意不改控制台代码页**（不要加 SetConsoleOutputCP）。
    //
    // 我们的两种输出方式都不依赖代码页：
    //   - 接在控制台上 → WriteConsoleW 直接写 UTF-16，chcp 是什么都不影响
    //   - 被重定向      → 原样写 UTF-8 字节，代码页根本不参与
    //
    // 反过来，改了代码页会害到**父进程**：PowerShell 的 [Console]::WriteLine
    // 在启动时就把编码固定成了当时的 OEM 代码页（936/GBK），如果子进程
    // 把控制台代码页改成 65001 又不改回来，父进程之后写的中文就按 UTF-8 解释，
    // 整段变乱码（"自检" → "�Լ�"）。编译脚本末尾那串乱码就是这么来的。
#endif
}

void out(const char * fmt, ...) {
#ifdef _WIN32
    detect_console();
#endif
    va_list ap;
    va_start(ap, fmt);
#ifdef _WIN32
    vwrite_to(stdout, g_out_is_console, fmt, ap);
#else
    vwrite_to(stdout, false, fmt, ap);
#endif
    va_end(ap);
}

void eout(const char * fmt, ...) {
#ifdef _WIN32
    detect_console();
#endif
    va_list ap;
    va_start(ap, fmt);
#ifdef _WIN32
    vwrite_to(stderr, g_err_is_console, fmt, ap);
#else
    vwrite_to(stderr, false, fmt, ap);
#endif
    va_end(ap);
}

#ifdef _WIN32
// ==========================================================================
//  Windows：全部走宽字符接口
// ==========================================================================

std::wstring to_native_path(const std::string & utf8) {
    return utf8_to_wide(utf8);
}

bool ensure_dir(const std::string & utf8_path, std::string & error) {
    if (utf8_path.empty()) { error = "目录名为空"; return false; }

    std::string p = utf8_path;
    for (auto & c : p) if (c == '\\') c = '/';
    const std::wstring w = to_native_path(p);

    std::size_t i = (w.size() >= 2 && w[1] == L':') ? 2 : 0;
    if (i < w.size() && w[i] == L'/') ++i;   // 跳过盘符后的根斜杠
    for (; i <= w.size(); ++i) {
        if (i != w.size() && w[i] != L'/') continue;
        const std::wstring sub = w.substr(0, i);
        if (sub.empty() || sub == L"/") continue;
        if (!::CreateDirectoryW(sub.c_str(), nullptr)) {
            const DWORD e = ::GetLastError();
            if (e != ERROR_ALREADY_EXISTS) {
                error = "建目录失败：" + wide_to_utf8(sub) + "（" + win32_error_text(e) + "）";
                return false;
            }
        }
    }

    const DWORD attr = ::GetFileAttributesW(w.c_str());
    if (attr == INVALID_FILE_ATTRIBUTES || (attr & FILE_ATTRIBUTE_DIRECTORY) == 0) {
        error = "建目录失败：" + utf8_path;
        return false;
    }
    return true;
}

bool write_file(const std::string & utf8_path, const void * data, std::size_t size,
                std::string & error) {
    const std::wstring w = to_native_path(utf8_path);
    const HANDLE h = ::CreateFileW(w.c_str(), GENERIC_WRITE, 0, nullptr,
                                   CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) {
        error = "打开文件失败：" + utf8_path + "（" + win32_error_text(::GetLastError()) + "）";
        return false;
    }
    const auto * p = static_cast<const std::uint8_t *>(data);
    std::size_t left = size;
    while (left > 0) {
        const DWORD chunk = static_cast<DWORD>(left > (1u << 20) ? (1u << 20) : left);
        DWORD written = 0;
        if (!::WriteFile(h, p, chunk, &written, nullptr) || written != chunk) {
            const DWORD e = ::GetLastError();
            ::CloseHandle(h);
            error = "写文件失败：" + utf8_path + "（" + win32_error_text(e) + "）";
            return false;
        }
        p += chunk;
        left -= chunk;
    }
    ::CloseHandle(h);
    return true;
}

bool read_file(const std::string & utf8_path, std::vector<std::uint8_t> & out_bytes,
               std::string & error) {
    const std::wstring w = to_native_path(utf8_path);
    const HANDLE h = ::CreateFileW(w.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr,
                                   OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) {
        error = "打开文件失败：" + utf8_path + "（" + win32_error_text(::GetLastError()) + "）";
        return false;
    }
    LARGE_INTEGER sz{};
    if (!::GetFileSizeEx(h, &sz) || sz.QuadPart < 0) {
        ::CloseHandle(h);
        error = "读取文件大小失败：" + utf8_path;
        return false;
    }
    out_bytes.resize(static_cast<std::size_t>(sz.QuadPart));
    std::size_t got = 0;
    while (got < out_bytes.size()) {
        const DWORD want = static_cast<DWORD>(
          (out_bytes.size() - got) > (1u << 20) ? (1u << 20) : (out_bytes.size() - got));
        DWORD read_n = 0;
        if (!::ReadFile(h, out_bytes.data() + got, want, &read_n, nullptr)) {
            const DWORD e = ::GetLastError();
            ::CloseHandle(h);
            error = "读文件失败：" + utf8_path + "（" + win32_error_text(e) + "）";
            return false;
        }
        if (read_n == 0) break;   // 文件被并发截短
        got += read_n;
    }
    out_bytes.resize(got);
    ::CloseHandle(h);
    return true;
}

bool file_size(const std::string & utf8_path, std::uint64_t & size_bytes) {
    const std::wstring w = to_native_path(utf8_path);
    const HANDLE h = ::CreateFileW(w.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr,
                                   OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) return false;
    LARGE_INTEGER sz{};
    const bool ok = ::GetFileSizeEx(h, &sz) != 0 && sz.QuadPart >= 0;
    ::CloseHandle(h);
    if (!ok) return false;
    size_bytes = static_cast<std::uint64_t>(sz.QuadPart);
    return true;
}

int run_process(const std::string & exe_utf8, const std::vector<std::string> & args_utf8) {
    const std::wstring exe = to_native_path(exe_utf8);

    std::wstring cmdline = quote_arg(exe);
    for (const auto & a : args_utf8) {
        cmdline += L' ';
        cmdline += quote_arg(to_native_path(a));
    }
    std::vector<wchar_t> buf(cmdline.begin(), cmdline.end());
    buf.push_back(L'\0');

    STARTUPINFOW si{};
    si.cb = sizeof(si);
    PROCESS_INFORMATION pi{};

    // ⚠ 这里必须是 CreateProcessW：路径里的「记录」这种中文字，走 ANSI 的 CreateProcessA
    //   会被按 GBK 解码，和我们的 UTF-8 字节对不上。
    if (!::CreateProcessW(exe.c_str(), buf.data(), nullptr, nullptr, TRUE, 0, nullptr, nullptr,
                          &si, &pi)) {
        eout("[进程] 启动失败：%s（%s）\n", exe_utf8.c_str(),
             win32_error_text(::GetLastError()).c_str());
        return -1;
    }
    ::WaitForSingleObject(pi.hProcess, INFINITE);
    DWORD code = 1;
    ::GetExitCodeProcess(pi.hProcess, &code);
    ::CloseHandle(pi.hThread);
    ::CloseHandle(pi.hProcess);
    return static_cast<int>(code);
}

std::string find_executable(const std::string & name_utf8) {
    if (name_utf8.empty()) return {};

    const std::wstring name = to_native_path(name_utf8);
    const bool has_dir = name.find_first_of(L"/\\") != std::wstring::npos;

    if (has_dir) {
        const std::wstring p = is_regular_file(name) ? name : with_exe_ext(name);
        return is_regular_file(p) ? wide_to_utf8(p) : std::string();
    }

    const std::wstring target = with_exe_ext(name);

    // 1) 当前目录
    {
        const DWORD n = ::GetCurrentDirectoryW(0, nullptr);
        if (n > 0) {
            std::wstring cwd(n, L'\0');
            ::GetCurrentDirectoryW(n, cwd.data());
            cwd.resize(n > 0 ? n - 1 : 0);
            const std::wstring p = cwd + L"\\" + target;
            if (is_regular_file(p)) return wide_to_utf8(p);
        }
    }

    // 2) 本 exe 所在目录（cw_sim.exe 旁边放 ffmpeg.exe 时最常用）
    {
        const DWORD n = ::GetModuleFileNameW(nullptr, nullptr, 0);
        if (n > 0) {
            std::wstring self(n, L'\0');
            ::GetModuleFileNameW(nullptr, self.data(), n);
            self.resize(n > 0 ? n - 1 : 0);
            const std::size_t cut = self.find_last_of(L"\\/");
            if (cut != std::wstring::npos) {
                const std::wstring p = self.substr(0, cut + 1) + target;
                if (is_regular_file(p)) return wide_to_utf8(p);
            }
        }
    }

    // 3) PATH
    {
        const DWORD n = ::GetEnvironmentVariableW(L"PATH", nullptr, 0);
        if (n > 0) {
            std::wstring path(n, L'\0');
            ::GetEnvironmentVariableW(L"PATH", path.data(), n);
            path.resize(n > 0 ? n - 1 : 0);

            std::size_t i = 0;
            while (i <= path.size()) {
                const std::size_t j = path.find(L';', i);
                const std::size_t end = (j == std::wstring::npos) ? path.size() : j;
                if (end > i) {
                    std::wstring dir = path.substr(i, end - i);
                    if (!dir.empty() && dir.back() != L'\\' && dir.back() != L'/') dir += L'\\';
                    const std::wstring p = dir + target;
                    if (is_regular_file(p)) return wide_to_utf8(p);
                }
                if (j == std::wstring::npos) break;
                i = j + 1;
            }
        }
    }

    return {};
}

std::vector<std::string> collect_args(int argc, char ** argv) {
    (void)argc;
    (void)argv;

    std::vector<std::string> args;
    int wargc = 0;
    LPWSTR * wargv = ::CommandLineToArgvW(::GetCommandLineW(), &wargc);
    if (wargv == nullptr) return args;

    for (int i = 1; i < wargc; ++i) {
        args.push_back(wide_to_utf8(wargv[i]));
    }
    ::LocalFree(wargv);
    return args;
}

#else
// ==========================================================================
//  Linux：路径本来就是 UTF-8 字节串，直接透传
// ==========================================================================

std::string to_native_path(const std::string & utf8) {
    return utf8;
}

bool ensure_dir(const std::string & utf8_path, std::string & error) {
    if (utf8_path.empty()) { error = "目录名为空"; return false; }

    std::string cur;
    std::size_t i = 0;
    if (utf8_path[0] == '/') { cur = "/"; i = 1; }

    while (i < utf8_path.size()) {
        std::size_t j = utf8_path.find('/', i);
        if (j == std::string::npos) j = utf8_path.size();
        if (j > i) {
            if (!cur.empty() && cur.back() != '/') cur += '/';
            cur.append(utf8_path, i, j - i);
            if (::mkdir(cur.c_str(), 0777) != 0 && errno != EEXIST) {
                error = "建目录失败：" + cur + "（" + std::strerror(errno) + "）";
                return false;
            }
        }
        if (j == utf8_path.size()) break;
        i = j + 1;
    }
    return true;
}

bool write_file(const std::string & utf8_path, const void * data, std::size_t size,
                std::string & error) {
    FILE * f = std::fopen(utf8_path.c_str(), "wb");
    if (f == nullptr) {
        error = "打开文件失败：" + utf8_path + "（" + std::strerror(errno) + "）";
        return false;
    }
    const std::size_t n = std::fwrite(data, 1, size, f);
    const int close_rc = std::fclose(f);
    if (n != size || close_rc != 0) {
        error = "写文件失败：" + utf8_path;
        return false;
    }
    return true;
}

bool read_file(const std::string & utf8_path, std::vector<std::uint8_t> & out_bytes,
               std::string & error) {
    FILE * f = std::fopen(utf8_path.c_str(), "rb");
    if (f == nullptr) {
        error = "打开文件失败：" + utf8_path + "（" + std::strerror(errno) + "）";
        return false;
    }
    if (std::fseek(f, 0, SEEK_END) != 0) {
        std::fclose(f);
        error = "读文件失败：" + utf8_path;
        return false;
    }
    const long sz = std::ftell(f);
    if (sz < 0) {
        std::fclose(f);
        error = "读文件失败：" + utf8_path;
        return false;
    }
    std::rewind(f);
    out_bytes.resize(static_cast<std::size_t>(sz));
    const std::size_t n =
      out_bytes.empty() ? 0 : std::fread(out_bytes.data(), 1, out_bytes.size(), f);
    std::fclose(f);
    if (n != out_bytes.size()) {
        error = "读文件失败：" + utf8_path;
        out_bytes.clear();
        return false;
    }
    return true;
}

bool file_size(const std::string & utf8_path, std::uint64_t & size_bytes) {
    // Linux 侧文件名本来就是字节串；用 stat 而不是读整个文件。
    struct stat st {};
    if (::stat(utf8_path.c_str(), &st) != 0) return false;
    if (st.st_size < 0) return false;
    size_bytes = static_cast<std::uint64_t>(st.st_size);
    return true;
}

int run_process(const std::string & exe_utf8, const std::vector<std::string> & args_utf8) {
    std::vector<char *> av;
    av.reserve(args_utf8.size() + 2);
    av.push_back(const_cast<char *>(exe_utf8.c_str()));
    for (const auto & a : args_utf8) av.push_back(const_cast<char *>(a.c_str()));
    av.push_back(nullptr);

    const pid_t pid = ::fork();
    if (pid < 0) {
        eout("[进程] fork 失败：%s\n", std::strerror(errno));
        return -1;
    }
    if (pid == 0) {
        ::execvp(exe_utf8.c_str(), av.data());
        ::_exit(127);
    }
    int status = 0;
    if (::waitpid(pid, &status, 0) < 0) return -1;
    if (WIFEXITED(status)) return WEXITSTATUS(status);
    if (WIFSIGNALED(status)) return 128 + WTERMSIG(status);
    return -1;
}

std::string find_executable(const std::string & name_utf8) {
    if (name_utf8.empty()) return {};

    if (name_utf8.find('/') != std::string::npos) {
        return ::access(name_utf8.c_str(), X_OK) == 0 ? name_utf8 : std::string();
    }

    const char * path = std::getenv("PATH");
    if (path == nullptr) return {};

    const std::string p(path);
    std::size_t i = 0;
    while (i <= p.size()) {
        const std::size_t j = p.find(':', i);
        const std::size_t end = (j == std::string::npos) ? p.size() : j;
        if (end > i) {
            std::string cand = p.substr(i, end - i);
            if (!cand.empty() && cand.back() != '/') cand += '/';
            cand += name_utf8;
            if (::access(cand.c_str(), X_OK) == 0) return cand;
        }
        if (j == std::string::npos) break;
        i = j + 1;
    }
    return {};
}

std::vector<std::string> collect_args(int argc, char ** argv) {
    std::vector<std::string> args;
    args.reserve(static_cast<std::size_t>(argc > 0 ? argc - 1 : 0));
    for (int i = 1; i < argc; ++i) args.emplace_back(argv[i]);
    return args;
}

#endif

}  // namespace cw
