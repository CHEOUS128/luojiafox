#include "platform_util.h"
#include <cstdio>
#include <thread>
#include <mutex>
#include <condition_variable>
#include <deque>
#include <atomic>
#include <algorithm>
#include <cstring>
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <mmsystem.h>
#include <share.h>

namespace cw {
struct StreamFile::Impl { FILE *file = nullptr; };
StreamFile::StreamFile() : impl_(std::make_unique<Impl>()) {}
StreamFile::~StreamFile() { close(); }
bool StreamFile::open(const std::string &path, const char *mode) {
    close();
    impl_->file = _wfsopen(to_native_path(path).c_str(), to_native_path(mode).c_str(), _SH_DENYNO);
    return impl_->file != nullptr;
}
std::size_t StreamFile::read(void *data, std::size_t bytes) { return impl_->file ? std::fread(data, 1, bytes, impl_->file) : 0; }
bool StreamFile::write(const std::string &text) {
    if(!impl_->file || std::fwrite(text.data(), 1, text.size(), impl_->file) != text.size())return false;
    return std::fflush(impl_->file)==0;
}
bool StreamFile::rewind() { return impl_->file && _fseeki64(impl_->file, 0, SEEK_SET) == 0; }
void StreamFile::close() { if (impl_->file) { std::fclose(impl_->file); impl_->file = nullptr; } }

namespace {
std::wstring quoted(const std::string &s) {
    auto w = to_native_path(s);
    std::wstring r = L"\"";
    std::size_t slashes = 0;
    for (auto c : w) {
        if (c == L'\\') { ++slashes; continue; }
        r.append(slashes * (c == L'"' ? 2 : 1), L'\\');
        slashes = 0;
        if (c == L'"') r += L'\\';
        r += c;
    }
    r.append(slashes * 2, L'\\');
    return r + L'"';
}
}
struct PipeProcess::Impl {
    HANDLE process = nullptr, input = nullptr, output = nullptr, error = nullptr;
    std::thread reader, lines;
    std::mutex mutex;
    std::mutex stop_mutex;
    std::condition_variable cv;
    std::deque<std::vector<std::uint8_t>> chunks;
    std::deque<std::string> text;
    std::size_t bytes = 0, offset = 0, limit = 64U * 1024U * 1024U;
    bool eof = false, text_eof = false;
    std::atomic<bool> stopped{false};
};
PipeProcess::PipeProcess() : impl_(std::make_unique<Impl>()) {}
PipeProcess::~PipeProcess() { stop(); }
bool PipeProcess::start(const std::string &exe, const std::vector<std::string> &args, std::string &err) {
    auto &p = *impl_;
    SECURITY_ATTRIBUTES sa{sizeof(sa), nullptr, TRUE};
    HANDLE ci = nullptr, co = nullptr, ce = nullptr;
    auto cleanup = [&] { for (auto h : {ci, co, ce}) if (h) CloseHandle(h); };
    if (!CreatePipe(&ci, &p.input, &sa, 65536) || !CreatePipe(&p.output, &co, &sa, 65536) ||
        !CreatePipe(&p.error, &ce, &sa, 65536)) { cleanup(); err = "CreatePipe failed"; return false; }
    for (auto h : {p.input, p.output, p.error}) SetHandleInformation(h, HANDLE_FLAG_INHERIT, 0);
    STARTUPINFOEXW sx{}; sx.StartupInfo.cb = sizeof(sx);
    sx.StartupInfo.dwFlags = STARTF_USESTDHANDLES;
    sx.StartupInfo.hStdInput = ci; sx.StartupInfo.hStdOutput = co; sx.StartupInfo.hStdError = ce;
    SIZE_T attribute_bytes = 0;
    InitializeProcThreadAttributeList(nullptr, 1, 0, &attribute_bytes);
    std::vector<std::uint8_t> attributes(attribute_bytes);
    sx.lpAttributeList = reinterpret_cast<LPPROC_THREAD_ATTRIBUTE_LIST>(attributes.data());
    if(!InitializeProcThreadAttributeList(sx.lpAttributeList, 1, 0, &attribute_bytes)) {
        cleanup();err="InitializeProcThreadAttributeList failed";return false;
    }
    HANDLE inherited[] = {ci,co,ce};
    if(!UpdateProcThreadAttribute(sx.lpAttributeList,0,PROC_THREAD_ATTRIBUTE_HANDLE_LIST,
                                 inherited,sizeof(inherited),nullptr,nullptr)) {
        DeleteProcThreadAttributeList(sx.lpAttributeList);cleanup();err="UpdateProcThreadAttribute failed";return false;
    }
    PROCESS_INFORMATION pi{};
    std::wstring command = quoted(exe);
    for (const auto &a : args) command += L" " + quoted(a);
    const BOOL created=CreateProcessW(to_native_path(exe).c_str(), command.data(), nullptr, nullptr, TRUE,
                        CREATE_NO_WINDOW|EXTENDED_STARTUPINFO_PRESENT, nullptr, nullptr, &sx.StartupInfo, &pi);
    DeleteProcThreadAttributeList(sx.lpAttributeList);
    if (!created) {
        cleanup(); err = "CreateProcessW failed: " + std::to_string(GetLastError()); return false;
    }
    cleanup(); p.process = pi.hProcess; CloseHandle(pi.hThread);
    p.reader = std::thread([&p] {
        for (;;) {
            std::vector<std::uint8_t> b(65536); DWORD n = 0;
            if (!ReadFile(p.output, b.data(), static_cast<DWORD>(b.size()), &n, nullptr) || !n) break;
            b.resize(n);
            std::unique_lock lock(p.mutex);
            p.cv.wait(lock, [&] { return p.stopped || p.bytes < p.limit; });
            if (p.stopped) break;
            p.bytes += n; p.chunks.push_back(std::move(b)); p.cv.notify_all();
        }
        std::lock_guard lock(p.mutex); p.eof = true; p.cv.notify_all();
    });
    p.lines = std::thread([&p] {
        std::string line; char buffer[4096]; DWORD n = 0;
        while (ReadFile(p.error, buffer, sizeof(buffer), &n, nullptr) && n) {
            for (DWORD i = 0; i < n; ++i) {
                if (buffer[i] == '\n') {
                    std::unique_lock lock(p.mutex);
                    p.cv.wait(lock, [&] { return p.stopped || p.text.size() < 64; });
                    if (p.stopped) break;
                    p.text.push_back(std::move(line)); line.clear(); p.cv.notify_all();
                } else if (buffer[i] != '\r' && line.size() < 16384) line += buffer[i];
            }
            if (p.stopped) break;
        }
        std::lock_guard lock(p.mutex);
        if (!line.empty()) p.text.push_back(std::move(line));
        p.text_eof = true; p.cv.notify_all();
    });
    return true;
}
bool PipeProcess::write(const void *data, std::size_t size) {
    auto &p = *impl_; auto b = static_cast<const std::uint8_t *>(data);
    while (size && !p.stopped) {
        DWORD n = 0;
        if (!WriteFile(p.input, b, static_cast<DWORD>(std::min<std::size_t>(size, 65536)), &n, nullptr) || !n) return false;
        size -= n; b += n;
    }
    return size == 0;
}
bool PipeProcess::read(void *data, std::size_t size) {
    auto &p = *impl_; auto b = static_cast<std::uint8_t *>(data);
    std::unique_lock lock(p.mutex);
    while (size) {
        p.cv.wait(lock, [&] { return p.stopped || p.eof || !p.chunks.empty(); });
        if (p.stopped || p.chunks.empty()) return false;
        auto &c = p.chunks.front(); auto n = std::min(size, c.size() - p.offset);
        std::memcpy(b, c.data() + p.offset, n); b += n; size -= n; p.bytes -= n; p.offset += n;
        if (p.offset == c.size()) { p.chunks.pop_front(); p.offset = 0; }
        p.cv.notify_all();
    }
    return true;
}
bool PipeProcess::line(std::string &line) {
    auto &p = *impl_; std::unique_lock lock(p.mutex);
    p.cv.wait(lock, [&] { return p.stopped || p.text_eof || !p.text.empty(); });
    if (p.stopped || p.text.empty()) return false;
    line = std::move(p.text.front()); p.text.pop_front(); p.cv.notify_all(); return true;
}
void PipeProcess::buffer_limit(std::size_t bytes) { std::lock_guard lock(impl_->mutex); impl_->limit = std::max<std::size_t>(bytes, 65536); impl_->cv.notify_all(); }
void PipeProcess::close_input() { if (impl_->input) { CloseHandle(impl_->input); impl_->input = nullptr; } }
void PipeProcess::stop() {
    auto &p = *impl_;
    std::lock_guard stop_lock(p.stop_mutex);
    // Idempotent; caller joins its worker before destroying the process wrapper.
    p.stopped = true; p.cv.notify_all();
    if (p.process) TerminateProcess(p.process, 0);
    if (p.reader.joinable()) p.reader.join();
    if (p.lines.joinable()) p.lines.join();
    for (auto h : {p.input, p.output, p.error, p.process}) if (h) CloseHandle(h);
    p.input = p.output = p.error = p.process = nullptr;
}
} // namespace cw

namespace cw {
TimingResolution::TimingResolution(){timeBeginPeriod(1);}
TimingResolution::~TimingResolution(){timeEndPeriod(1);}
}
