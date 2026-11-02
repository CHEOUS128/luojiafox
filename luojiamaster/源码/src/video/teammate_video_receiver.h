#pragma once

#include <pch.h>

#include "app/application_types.h"
#include "video/video_decoder.h"

// 队友画面接收端：把 TeammateLink 收到的原始 HEVC 码流解成 RGBA，交给主循环上传纹理。
//
// 为什么不用现成的 VideoDecoder：
//   1) 它是无人值守的——裸调 avcodec_send_packet 后没有 drain，P 帧会被压在解码器里出不来；
//   2) HEVC 的 cuvid 解码器拿到不完整的参数集会直接终止进程，硬解不适合这种「可能丢包」的链路；
//   3) 队友链路中途可能换分辨率/重建流，需要能自己重建解码上下文。
// 所以这里是一个独立的小解码器，软解 + 每次收完都 drain，参数集变化时重建上下文。
namespace RoboGo::video {

struct TeammateVideoConfig {
    // 输入队列深度。解码跟不上时丢最旧的，绝不让主链路等。
    std::size_t input_queue_capacity = 16;
    // 分辨率上限，防坏流把内存吃光。
    int max_dimension = 4096;
};

struct TeammateVideoStats {
    std::uint64_t frames_submitted = 0;// 进入解码器的帧
    std::uint64_t frames_decoded = 0; // 解码出的画面
    std::uint64_t frames_dropped = 0; // 队列满丢掉的
    std::uint64_t decode_errors = 0;
    std::uint64_t context_rebuilds = 0;
    int width = 0;
    int height = 0;
};

class TeammateVideoReceiver {
public:
    TeammateVideoReceiver(TeammateVideoConfig config, app::VideoFrameHandler on_frame);
    ~TeammateVideoReceiver();

    TeammateVideoReceiver(const TeammateVideoReceiver &) = delete;
    TeammateVideoReceiver &operator=(const TeammateVideoReceiver &) = delete;
    TeammateVideoReceiver(TeammateVideoReceiver &&) = delete;
    TeammateVideoReceiver &operator=(TeammateVideoReceiver &&) = delete;

    // 接收线程调用：只入队，不做重活。
    void submit_frame(std::vector<std::uint8_t> &&frame);

    [[nodiscard]] TeammateVideoStats stats() const;

    void stop();

private:
    struct Shared;

    void run();

    std::shared_ptr<Shared> shared_;
    std::thread worker_thread_;
    bool stopped_ = false;
};

}// namespace RoboGo::video
