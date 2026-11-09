// platform_util.h —— 跨平台的一层「路径 / 子进程 / 控制台」胶水
//
// 为什么需要这一层（这是 Windows 上最容易踩的一类坑，踩过一次就够）：
//
//   本工程内部所有字符串都是 **UTF-8**（源码带中文注释，MSVC 用 /utf-8 编）。
//   可是 Windows 上 C 运行库的 fopen / system / mkdir 走的是 **ANSI 接口**，
//   而中文系统的 ANSI 代码页是 GBK(936)。于是：
//
//     路径 "…/Desktop/robot记录/luojiafox/compression-war/runs/60000/viewer/left/L00000.png"
//     交给 fopen 时，系统先按 GBK 解码。我们的字节是 UTF-8，GBK 解码器对着
//     「记录」这两个字的 6 个字节会解出别的字（或解错长度），**后面所有字节都跟着错位**，
//     于是路径指向一个根本不存在的文件。
//
//   stb 的 stbi_write_png 也是 fopen，同样中招。
//   std::system 走 cmd.exe /c，cmd.exe 同样按 ANSI 解释命令行。
//
//   所以：凡是**碰文件系统、碰子进程、碰控制台**的地方，Windows 上必须走宽字符接口
//   （CreateFileW / CreateProcessW / CreateDirectoryW / WriteConsoleW），
//   把 UTF-8 先转成 UTF-16 再交给系统。Linux 上原样透传即可。
//
// 这一层的接口刻意做得很小：路径一律收 UTF-8，出错一律返回 false + 原因。
#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>
#include <memory>

namespace cw {

// 程序启动第一件事：Windows 上把 stdout/stderr 接到 WriteConsoleW，
// 这样源码里的 UTF-8 中文能正确显示，且**不受当前 chcp 影响**（chcp 936 也不会乱码）。
// Linux 上什么都不做。
void init_console();

// 输出（fmt 是 UTF-8，语义同 printf）。内部先把整条消息格式化好，再决定怎么送到终端：
//   - 接在控制台上 → 转 UTF-16 走 WriteConsoleW（任何代码页都不乱码）
//   - 被重定向到文件/管道 → 原样写 UTF-8 字节
void out(const char * fmt, ...);
void eout(const char * fmt, ...);

// UTF-8 → 平台原生路径。
// Linux: 原样返回（Linux 文件名本来就是字节串，UTF-8 直接用）
// Windows: 转成 UTF-16
#ifdef _WIN32
std::wstring to_native_path(const std::string & utf8);
#else
std::string to_native_path(const std::string & utf8);
#endif

// 递归建目录（相当于 mkdir -p）。路径是 UTF-8。
bool ensure_dir(const std::string & utf8_path, std::string & error);

// 一次性把一段内存写成文件（覆盖）。
bool write_file(const std::string & utf8_path, const void * data, std::size_t size,
                std::string & error);

// 读整个文件。失败时返回 false 并把原因写进 error。
bool read_file(const std::string & utf8_path, std::vector<std::uint8_t> & out_bytes,
               std::string & error);

// 只取文件大小（不读内容）。失败返回 (std::size_t)-1。
// 用途：算相机帧数时不能为了「有多少帧」把 1 GB 素材读进内存。
bool file_size(const std::string & utf8_path, std::uint64_t & size_bytes);

// 跑一个子进程并等它结束，返回退出码（能跑起来但非 0 = 子进程自己的退出码；
// 起不来返回 -1）。参数表由调用方给，内部负责转义/加引号。
//   - Windows: CreateProcessW（路径可以含中文、空格）
//   - Linux:   fork + execvp（不经过 shell，避免中文/空格二次解析）
int run_process(const std::string & exe_utf8,
                const std::vector<std::string> & args_utf8);

// 在 PATH 和当前目录里找可执行文件。找不到返回空串。
// 用途：`--ffmpeg` 默认给的是裸文件名 "ffmpeg.exe"，需要先解析成绝对路径才能
// 交给 CreateProcessW（它不会自己去 PATH 里找）。
std::string find_executable(const std::string & name_utf8);

// 取真正的命令行参数（UTF-8）。
//   - Windows: GetCommandLineW + CommandLineToArgvW，中文路径原样进得来
//   - Linux:   argv 原样
std::vector<std::string> collect_args(int argc, char ** argv);

// Streaming I/O for the live simulator. All OS paths/process creation stay here.
class StreamFile {
public:
    StreamFile();
    ~StreamFile();
    bool open(const std::string &path, const char *mode);
    std::size_t read(void *data, std::size_t bytes);
    bool write(const std::string &text);
    bool rewind();
    void close();
private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

class PipeProcess {
public:
    PipeProcess();
    ~PipeProcess();
    bool start(const std::string &exe, const std::vector<std::string> &args, std::string &error);
    bool write(const void *data, std::size_t bytes);
    bool read(void *data, std::size_t bytes);
    bool line(std::string &text);
    void close_input();
    void stop();
    void buffer_limit(std::size_t bytes);
private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};
// Temporary, process-lifetime request; avoids 15.6ms rounding of packet waits.
class TimingResolution {
public:
    TimingResolution();
    ~TimingResolution();
};

}  // namespace cw
