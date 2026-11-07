#include "raw_io.h"

#include "platform_util.h"

#include <algorithm>
#include <cstdio>
#include <utility>

// 读相机原片用。⚠ STB_IMAGE_WRITE 的实现已经在 sim_runner.cpp 里定义过，
//   这里只定义读的那一半（STB_IMAGE_IMPLEMENTATION），两者互不冲突。
#define STB_IMAGE_IMPLEMENTATION
#include "stb_image.h"

namespace cw {

namespace {

bool read_whole_file(const std::string & path, std::vector<std::uint8_t> & out, std::string & error) {
    // ⚠ 不能改用 std::ifstream：Windows 上它内部是 fopen，按 ANSI(GBK) 解释路径，
    //   路径里的中文会让后续文件名错位。统一走 platform_util 的宽字符实现。
    return read_file(path, out, error);
}

}  // namespace

bool normalize_pix_fmt(const std::string & in, std::string & out) {
    // 兼容外部工具的常见别名；内部只认 gray / yuv420p / bgr24 三种。
    if (in == "gray" || in == "grey" || in == "y" || in == "gray8" || in == "GRAY8" ||
        in == "mono" || in == "mono8") {
        out = "gray";
        return true;
    }
    if (in == "yuv420p" || in == "yuv" || in == "i420") {
        out = "yuv420p";
        return true;
    }
    if (in == "bgr24" || in == "bgr" || in == "BGR24" || in == "bgr8" || in == "rgb24") {
        out = "bgr24";
        return true;
    }
    return false;
}

std::size_t frame_bytes_for(const std::string & pix_fmt, int width, int height) {
    if (width <= 0 || height <= 0) return 0;
    const std::size_t pixels = static_cast<std::size_t>(width) * static_cast<std::size_t>(height);
    if (pix_fmt == "gray") return pixels;
    if (pix_fmt == "yuv420p") return pixels * 3 / 2;
    if (pix_fmt == "bgr24") return pixels * 3;
    return 0;
}

bool read_png_frames(const std::string & dir,
                     int max_frames,
                     std::vector<RawFrame> & out,
                     std::string & error) {
    out.clear();
    const int limit = max_frames < 0 ? 1000000 : max_frames;
    for (int i = 0; i < limit; ++i) {
        char name[512];
        std::snprintf(name, sizeof(name), "%s/%06d.png", dir.c_str(), i);
        std::vector<std::uint8_t> raw;
        std::string rd_err;
        if (!read_whole_file(name, raw, rd_err)) {
            if (i == 0) {
                error = "第一张就没了：" + std::string(name) + "（" + rd_err + "）";
                return false;
            }
            break;   // 正常收尾：编号断了就停
        }
        int w = 0, h = 0, comp = 0;
        unsigned char * px = stbi_load_from_memory(raw.data(), static_cast<int>(raw.size()),
                                                   &w, &h, &comp, 0);
        if (px == nullptr) {
            error = std::string("解不开 ") + name;
            return false;
        }
        RawFrame f;
        f.width = w;
        f.height = h;
        const std::size_t n = static_cast<std::size_t>(w) * h;
        if (comp == 1) {
            f.gray = true;
            f.bgr = false;
            f.bytes.assign(px, px + n);
        } else {
            // stb 给的是 RGB/RGBA，翻成 BGR（相机/OpenCV 的顺序）
            f.gray = false;
            f.bgr = true;
            f.bytes.resize(n * 3);
            for (std::size_t k = 0; k < n; ++k) {
                f.bytes[k * 3 + 0] = px[k * comp + 2];
                f.bytes[k * 3 + 1] = px[k * comp + 1];
                f.bytes[k * 3 + 2] = px[k * comp + 0];
            }
        }
        stbi_image_free(px);
        out.push_back(std::move(f));
    }
    if (out.empty()) {
        error = "目录里没有 000000.png：" + dir;
        return false;
    }
    // 尺寸必须一致，否则播放器会错位
    for (std::size_t i = 1; i < out.size(); ++i) {
        if (out[i].width != out[0].width || out[i].height != out[0].height) {
            error = "PNG 尺寸不一致，第 " + std::to_string(i) + " 帧和第一帧不同";
            out.clear();
            return false;
        }
    }
    return true;
}

bool read_raw_frames(const std::string & path,
                     int width,
                     int height,
                     const std::string & pix_fmt_in,
                     int max_frames,
                     std::vector<RawFrame> & out,
                     std::string & error) {
    out.clear();
    if (width <= 0 || height <= 0) {
        error = "raw 尺寸非法";
        return false;
    }
    std::string pix_fmt;
    if (!normalize_pix_fmt(pix_fmt_in, pix_fmt)) {
        error = "不支持的像素格式：" + pix_fmt_in + "（只认 gray / yuv420p / bgr24）";
        return false;
    }
    std::vector<std::uint8_t> blob;
    if (!read_whole_file(path, blob, error)) return false;

    const std::size_t frame_bytes = frame_bytes_for(pix_fmt, width, height);
    if (frame_bytes == 0) {
        error = "单帧字节数为 0";
        return false;
    }
    if (blob.size() % frame_bytes != 0) {
        error = "raw 文件大小不是单帧字节数的整数倍（可能尺寸或像素格式填错了）";
        return false;
    }

    const std::size_t total = blob.size() / frame_bytes;
    const std::size_t count =
      max_frames < 0 ? total : std::min<std::size_t>(total, static_cast<std::size_t>(max_frames));

    out.reserve(count);
    for (std::size_t i = 0; i < count; ++i) {
        RawFrame frame;
        frame.width = width;
        frame.height = height;
        frame.gray = (pix_fmt == "gray");
        frame.bgr = (pix_fmt == "bgr24");
        frame.bytes.assign(blob.begin() + static_cast<std::ptrdiff_t>(i * frame_bytes),
                           blob.begin() + static_cast<std::ptrdiff_t>((i + 1) * frame_bytes));
        out.push_back(std::move(frame));
    }
    return true;
}

namespace {

struct Nal {
    std::size_t start;     // start code 的第一个字节
    std::size_t payload;   // NAL 头所在位置
    std::size_t end;       // 下一个 NAL 的 start（或文件末尾）
    int type;
};

// 返回从 pos 开始的下一个 start code 的起始位置，并把 nal 起始位置写进 nal_pos。
// 没找到返回 npos。
std::size_t find_start_code(const std::vector<std::uint8_t> & b, std::size_t pos, std::size_t & nal_pos) {
    for (std::size_t i = pos; i + 3 <= b.size(); ++i) {
        if (b[i] == 0 && b[i + 1] == 0 && b[i + 2] == 1) {
            nal_pos = i + 3;
            return i;
        }
    }
    return std::string::npos;
}

// 一个 VCL NAL 的 slice header 里 first_mb_in_slice 是否为 0。
// 这是判「新的一帧开始」的**唯一可靠**依据：x264 开 slices=N 时一帧有 N 个 VCL NAL，
// 只有第一个 first_mb_in_slice==0，后面的 1、2、3… 都是同一帧的 slice。
//
// slice header 的第一个字段就是 first_mb_in_slice，用 ue(v) 编码，
// 所以它一定是 NAL 头后第一个 bit 开始的无符号指数哥伦布码。
bool slice_starts_new_frame(const std::vector<std::uint8_t> & b, std::size_t nal_payload) {
    // nal_payload 指向 NAL 头字节；跳过 1 字节头就是 slice header 起点
    std::size_t pos = nal_payload + 1;
    if (pos >= b.size()) return false;
    // 读 ue(v)：数前导零
    int leading_zeros = 0;
    while (pos < b.size()) {
        const std::uint8_t byte = b[pos];
        for (int bit = 7; bit >= 0; --bit) {
            const bool set = ((byte >> bit) & 1U) != 0U;
            if (!set) {
                ++leading_zeros;
                if (leading_zeros > 31) return false;   // 非法码
            } else {
                // 前导零之后的 '1' 结束一元码；码值 = 2^L - 1 + 剩下 L 个 bit 的值
                // 第一个字段值是否为 0，只取决于「前导零个数」：
                //   L=0 → 码值 0；L≥1 → 码值 ≥ 2^L - 1 ≥ 1
                return leading_zeros == 0;
            }
        }
        ++pos;
    }
    return false;
}

}  // namespace

bool split_access_units(const std::string & h264_path,
                        std::vector<AccessUnit> & out,
                        std::string & error) {
    out.clear();
    std::vector<std::uint8_t> blob;
    if (!read_whole_file(h264_path, blob, error)) return false;
    if (blob.size() < 4) {
        error = "H.264 文件太小";
        return false;
    }

    std::vector<Nal> nals;
    std::size_t nal_pos = 0;
    for (std::size_t sc = find_start_code(blob, 0, nal_pos); sc != std::string::npos;
         sc = find_start_code(blob, nal_pos, nal_pos)) {
        nals.push_back(Nal{sc, nal_pos, blob.size(), static_cast<int>(blob[nal_pos] & 0x1F)});
        // 从 NAL 头之后继续找，避免把同一个 start code 里的 00 00 01 重复识别
        nal_pos += 1;
    }
    if (nals.empty()) {
        error = "没有找到任何 NAL（不是 Annex-B 码流？）";
        return false;
    }
    for (std::size_t i = 0; i + 1 < nals.size(); ++i) nals[i].end = nals[i + 1].start;

    // ★ 切点：一个 VCL NAL 且 first_mb_in_slice == 0 → 新访问单元开始。
    //   这条规则对 slices=1 和 slices=4 都成立，而且**不需要 AUD**——
    //   不加 AUD 才能让码流字节数和实车完全一致（AUD 每帧多 6 字节，会对账失真）。
    //
    //   为什么不用「AUD 存在就按 AUD 切」：x264 加 aud=1 时，低分辨率小帧也确实会
    //   出 AUD，可以切。但那会让每帧多 6 字节，破坏与实车的字节级可比性。
    //   所以优先用 first_mb，AUD 只当兜底（真有 AUD 时它更权威）。
    std::vector<std::size_t> cut_points;   // 每个 AU 的第一个 NAL 下标
    {
        bool saw_aud = false;
        for (std::size_t i = 0; i < nals.size(); ++i) {
            if (nals[i].type == 9) {
                saw_aud = true;
                break;
            }
        }
        if (saw_aud) {
            for (std::size_t i = 0; i < nals.size(); ++i) {
                if (nals[i].type == 9) cut_points.push_back(i);
            }
            // 第一个 AUD 之前的 SPS/PPS 不能丢，否则第一帧解不出来
            if (!cut_points.empty() && cut_points.front() != 0) {
                cut_points.insert(cut_points.begin(), 0);
            }
        } else {
            for (std::size_t i = 0; i < nals.size(); ++i) {
                if (nals[i].type < 1 || nals[i].type > 5) continue;   // 只看 VCL
                if (slice_starts_new_frame(blob, nals[i].payload)) cut_points.push_back(i);
            }
            // 第一个 VCL 之前的 SPS/PPS/SEI 属于第一帧，把第一刀从「首个 VCL」
            // 往前挪到 0（不是插一刀——插一刀会多出一个「只有参数集」的空帧）
            if (cut_points.empty() || cut_points.front() != 0) {
                if (!cut_points.empty()) cut_points.front() = 0;
                else cut_points.push_back(0);
            }
        }
    }

    if (cut_points.empty()) {
        error = "切帧失败：没有分到任何访问单元";
        return false;
    }

    for (std::size_t k = 0; k < cut_points.size(); ++k) {
        const std::size_t first = cut_points[k];
        const std::size_t last = (k + 1 < cut_points.size()) ? cut_points[k + 1] : nals.size();
        if (first >= last) continue;
        AccessUnit au;
        const std::size_t begin = nals[first].start;
        const std::size_t end = nals[last - 1].end;
        au.bytes.assign(blob.begin() + static_cast<std::ptrdiff_t>(begin),
                        blob.begin() + static_cast<std::ptrdiff_t>(end));
        for (std::size_t idx = first; idx < last; ++idx) {
            if (nals[idx].type == 5) {
                au.keyframe = true;
                break;
            }
        }
        out.push_back(std::move(au));
    }
    return true;
}

}  // namespace cw
