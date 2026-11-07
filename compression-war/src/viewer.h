// viewer.h —— 左右对比播放器的导出器
//
// ★ 这个播放器的语义是 CH 2026-11-07 定的，别再按"两栏同步播放"去理解：
//
//   左边 = **相机原片，原封不动**（彩色、1280×1024、没转灰度、没缩放、没编码）。
//          它在 t = 这一帧被拍到的时刻出现。
//   右边 = 这一帧**处理完**以后客户端才收到的画面（缩放 + 转灰度 + 编码 + 拆包 +
//          排队发出去 + 解码），所以它必然比左边晚。晚多少，屏幕上实时显示。
//
//   同一帧的编号一样（左 L00042 / 右 R00042），但出现时刻不同——这就是"延迟"。
//   丢帧则表现为右边干脆不出现这一帧（右边停在上一个处理完的画面上）。
//
// 为什么不能两栏按同一个 20fps 傻放：
//   那样丢帧只是画面卡一下看不出来，延迟也被抹成 0。按真实时刻放，
//   掉帧会表现为**画面冻住 + 时间轴跳格**，和实车看到的完全一样。
#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace cw {

struct ViewerFrame {
    std::uint32_t encoded_index = 0;    // 对应**相机帧号**（= 左边那一帧）
    double capture_ms = 0.0;            // 相机拍到它的时刻（左边出现）
    double arrive_ms = 0.0;             // 最后一包发出去的时刻（已含编码，包是编完才有的）
    double ready_ms = 0.0;              // 客户端能看见它的时刻 = arrive + 呈现开销
    // 编码线程把这一帧编完的时刻。★ 它必须参与"什么时候能显示"：
    //   真实客户端收到第一包就会解码，但编码器是**编完才吐第一包**的，
    //   所以整帧到齐只可能晚于它。单包模式下两者本来就相等；
    //   多包时第一包 10~30ms 就到，不加这条会在"还没编完"的时刻就把画面放出来（假快）。
    double encode_done_ms = 0.0;
    int width = 0;
    int height = 0;
    const std::uint8_t * y_plane = nullptr;   // 解码器输出的亮度面（W*H）
};

struct ViewerSpec {
    std::string out_dir;                // 导出目录（会在其下建 left/ right/ player.html）
    std::string title = "压片仿真";
    std::string subtitle;
    double fps = 20.0;
    std::size_t frame_count = 0;        // **相机**帧总数（决定左侧时间轴长度）
    int width = 192;
    int height = 152;
    bool gray = true;

    // 编码耗时（毫秒/帧）。实车 VideoSender 在独立线程里编码，这段时间也是延迟的一部分。
    // 0 = 只算链路、不算编码（"假设编码免费"，看基线用）。
    // ⚠ 要用实车实测值填，否则右边的延迟是编出来的——播放器会把 0 标注成「未计编码」。
    double encode_ms = 0.0;
    // 固定链路开销（裁判系统/图传/客户端解码排队）。目前没有实测值，默认 0。
    double wire_extra_ms = 0.0;
    // 编码线程跟不上的帧数（仅用于在页面上说清楚"为什么右栏跳帧"）
    std::size_t encode_dropped = 0;

    // ---- 左侧"真原片"（可选）----
    // 给了 ref_planes 就用它当左图：同一取景、原分辨率、**没经过任何处理**。
    // 不给就退回用 src_planes（编码器输入，192×152）——那种情况下"左侧看起来糊"
    // 是正常的，因为那本来就是压缩前的样子，不是仿真把它压糊了。
    // ⚠ ref_planes / src_planes 都按**相机帧号**索引（大小 = frame_count）。
    int ref_width = 0;
    int ref_height = 0;
    std::vector<const std::uint8_t *> ref_planes;   // 每帧 W*H（gray）或 W*H*3（bgr）
    bool ref_is_bgr = false;                        // 相机原生是 BGR
};

// 写 left/*.png、right/*.png、player.html。
// ⚠ src_planes 是「每帧一个指针」，不是一段连续内存：raw_io 把每帧存成独立的
//   vector<RawFrame>，每帧各自在堆上分配，拿首帧地址往后加 i*stride 会读到别的堆块
//   （画面会竖着错位、出重影）。这个坑踩过一次，别改回去。
//   每帧按 gray(W*H) 或 yuv420p(W*H*3/2) 解释。
//
// ⚠ 左右两栏在浏览器里**显示尺寸相同**（都撑满半屏），但落盘的 PNG 分辨率不同：
//   左 1280×1024（ref_planes），右 W×H（解码器输出就那么点大）。
//   于是"右边糊"就是真糊，眼睛一对比立刻能看出来——这才是 CH 要的对比。
bool export_viewer(const ViewerSpec & spec,
                   const std::vector<const std::uint8_t *> & src_planes,
                   const std::vector<ViewerFrame> & decoded,
                   std::string & error);

}  // namespace cw
