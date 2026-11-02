#include "teammate_video_receiver.h"

#include "logging/logging.h"

namespace RoboGo::video {

namespace {

// 一次解码会话：解析器 + 解码器 + 到 RGBA 的转换器。
// 参数集变了（换分辨率、对端重开流）就整体重建。
struct DecodeSession {
    AVCodecContext *codec_context = nullptr;
    AVPacket *packet = nullptr;
    AVFrame *frame = nullptr;
    SwsContext *sws_context = nullptr;
    int source_width = 0;
    int source_height = 0;
    AVPixelFormat source_format = AV_PIX_FMT_NONE;
    std::vector<std::uint8_t> rgba;
    bool configured = false;

    ~DecodeSession() {
        if (sws_context != nullptr) {
            sws_freeContext(sws_context);
            sws_context = nullptr;
        }
        if (frame != nullptr) {
            av_frame_free(&frame);
        }
        if (packet != nullptr) {
            av_packet_free(&packet);
        }
        if (codec_context != nullptr) {
            avcodec_free_context(&codec_context);
        }
    }

    // 只在第一次（或重建后）打开解码器。故意不用 hevc_cuvid：
    // 硬解拿到不完整的参数集会直接终止进程，而队友链路是可能丢包的。
    bool open(std::string &error_message) {
        const AVCodec *codec = avcodec_find_decoder_by_name("hevc");
        if (codec == nullptr) {
            codec = avcodec_find_decoder(AV_CODEC_ID_HEVC);
        }
        if (codec == nullptr) {
            error_message = "找不到 HEVC 解码器";
            return false;
        }
        codec_context = avcodec_alloc_context3(codec);
        if (codec_context == nullptr) {
            error_message = "分配解码上下文失败";
            return false;
        }
        codec_context->flags |= AV_CODEC_FLAG_LOW_DELAY;
        codec_context->flags2 |= AV_CODEC_FLAG2_FAST;
        // 容忍丢包：画面糊一点也比整条链路断掉好
        codec_context->err_recognition = 0;
        if (avcodec_open2(codec_context, codec, nullptr) < 0) {
            error_message = "打开 HEVC 解码器失败";
            return false;
        }
        packet = av_packet_alloc();
        frame = av_frame_alloc();
        if (packet == nullptr || frame == nullptr) {
            error_message = "分配解码帧缓存失败";
            return false;
        }
        configured = true;
        return true;
    }

    void reset_buffers() {
        if (codec_context != nullptr) {
            avcodec_flush_buffers(codec_context);
        }
    }
};

int StartCodeBytesAt(const std::vector<std::uint8_t> &data, const std::size_t position) {
    if (position + 4 <= data.size() && data[position] == 0 && data[position + 1] == 0
        && data[position + 2] == 0 && data[position + 3] == 1) {
        return 4;
    }
    if (position + 3 <= data.size() && data[position] == 0 && data[position + 1] == 0
        && data[position + 2] == 1) {
        return 3;
    }
    return 0;
}

int NalTypeAt(const std::vector<std::uint8_t> &data, const std::size_t position) {
    if (position >= data.size()) {
        return -1;
    }
    return (data[position] >> 1) & 0x3F;
}

// 这一帧里有没有 VPS/SPS/PPS。有的话字节完全相同就说明参数集没变，
// 不必重建解码上下文；变了就说明对端换了分辨率或重开了流。
bool SameParameterSets(const std::vector<std::uint8_t> &lhs, const std::vector<std::uint8_t> &rhs) {
    return !lhs.empty() && lhs == rhs;
}

void CollectParameterSetBytes(const std::vector<std::uint8_t> &frame, std::vector<std::uint8_t> &out) {
    out.clear();
    std::size_t position = 0;
    while (position + 3 <= frame.size()) {
        const int start_code_bytes = StartCodeBytesAt(frame, position);
        if (start_code_bytes == 0) {
            ++position;
            continue;
        }
        const std::size_t nal_start = position;
        const int nal_type = NalTypeAt(frame, position + static_cast<std::size_t>(start_code_bytes));
        std::size_t next = position + static_cast<std::size_t>(start_code_bytes) + 1;
        while (next + 3 <= frame.size() && StartCodeBytesAt(frame, next) == 0) {
            ++next;
        }
        if (next + 3 > frame.size()) {
            next = frame.size();
        }
        if (nal_type == 32 || nal_type == 33 || nal_type == 34) {
            out.insert(out.end(),
                       frame.begin() + static_cast<std::ptrdiff_t>(nal_start),
                       frame.begin() + static_cast<std::ptrdiff_t>(next));
        }
        position = next;
    }
}

bool EnsureSwsContext(DecodeSession &session, const AVFrame *frame) {
    const auto frame_format = static_cast<AVPixelFormat>(frame->format);
    if (session.sws_context != nullptr && session.source_width == frame->width
        && session.source_height == frame->height && session.source_format == frame_format) {
        return true;
    }
    if (session.sws_context != nullptr) {
        sws_freeContext(session.sws_context);
        session.sws_context = nullptr;
    }
    session.sws_context = sws_getContext(frame->width,
                                         frame->height,
                                         frame_format,
                                         frame->width,
                                         frame->height,
                                         AV_PIX_FMT_RGBA,
                                         SWS_FAST_BILINEAR,
                                         nullptr,
                                         nullptr,
                                         nullptr);
    if (session.sws_context == nullptr) {
        return false;
    }
    session.source_width = frame->width;
    session.source_height = frame->height;
    session.source_format = frame_format;
    return true;
}

// 把解出来的 AVFrame 转成 RGBA。注意：这里不做 180° 翻转——
// 官方图传是在渲染阶段用 GPU 的 UV 翻转做的，队友画面的倒置状态随握手一起传过来，
// 由 UI 层用同样的方式处理，这样缩放/放大时才不会重复翻转。
bool ConvertToRgba(DecodeSession &session,
                   const AVFrame *frame,
                   const TeammateVideoConfig &config,
                   app::VideoFrame &out) {
    if (frame->width <= 0 || frame->height <= 0) {
        return false;
    }
    if (frame->width > config.max_dimension || frame->height > config.max_dimension) {
        SPDLOG_WARN("队友画面分辨率超出上限 width={} height={} limit={}",
                    frame->width,
                    frame->height,
                    config.max_dimension);
        return false;
    }
    if (!EnsureSwsContext(session, frame)) {
        return false;
    }

    const std::size_t width = static_cast<std::size_t>(frame->width);
    const std::size_t height = static_cast<std::size_t>(frame->height);
    session.rgba.resize(width * height * 4u);

    // sws_scale 内部可能用 SIMD 写，目标缓冲按 FFmpeg 对齐要求分配再拷回来最稳。
    std::uint8_t *dst_slices[4] = {nullptr, nullptr, nullptr, nullptr};
    int dst_stride[4] = {0, 0, 0, 0};
    if (av_image_alloc(dst_slices, dst_stride, frame->width, frame->height, AV_PIX_FMT_RGBA, 32) < 0) {
        return false;
    }
    const int scaled_height = sws_scale(session.sws_context,
                                        frame->data,
                                        frame->linesize,
                                        0,
                                        frame->height,
                                        dst_slices,
                                        dst_stride);
    if (scaled_height != frame->height) {
        av_freep(&dst_slices[0]);
        return false;
    }
    for (int y = 0; y < frame->height; ++y) {
        const auto *src_row = dst_slices[0] + static_cast<std::ptrdiff_t>(y) * dst_stride[0];
        auto *dst_row = session.rgba.data() + static_cast<std::size_t>(y) * width * 4u;
        std::memcpy(dst_row, src_row, width * 4u);
    }
    av_freep(&dst_slices[0]);

    out.buffer = session.rgba;// 后面交给主循环上传纹理，拷一份避免被下一帧覆盖
    out.width = frame->width;
    out.height = frame->height;
    return true;
}

}// namespace

struct TeammateVideoReceiver::Shared {
    TeammateVideoConfig config;
    app::VideoFrameHandler on_frame;

    std::mutex queue_mutex;
    std::condition_variable queue_cv;
    std::deque<std::vector<std::uint8_t>> input_queue;
    std::atomic<bool> running{true};

    std::atomic<std::uint64_t> frames_submitted{0};
    std::atomic<std::uint64_t> frames_decoded{0};
    std::atomic<std::uint64_t> frames_dropped{0};
    std::atomic<std::uint64_t> decode_errors{0};
    std::atomic<std::uint64_t> context_rebuilds{0};
    std::atomic<int> width{0};
    std::atomic<int> height{0};
};

TeammateVideoReceiver::TeammateVideoReceiver(TeammateVideoConfig config, app::VideoFrameHandler on_frame)
    : shared_(std::make_shared<Shared>()) {
    shared_->config = config;
    shared_->on_frame = std::move(on_frame);
    worker_thread_ = std::thread([this]() { run(); });
    SPDLOG_INFO("队友画面解码器已启动");
}

TeammateVideoReceiver::~TeammateVideoReceiver() {
    stop();
}

void TeammateVideoReceiver::submit_frame(std::vector<std::uint8_t> &&frame) {
    if (frame.empty() || !shared_->running.load(std::memory_order_relaxed)) {
        return;
    }
    {
        std::lock_guard lock(shared_->queue_mutex);
        while (shared_->input_queue.size() >= shared_->config.input_queue_capacity) {
            shared_->input_queue.pop_front();
            shared_->frames_dropped.fetch_add(1, std::memory_order_relaxed);
        }
        shared_->input_queue.push_back(std::move(frame));
        shared_->frames_submitted.fetch_add(1, std::memory_order_relaxed);
    }
    shared_->queue_cv.notify_one();
}

TeammateVideoStats TeammateVideoReceiver::stats() const {
    TeammateVideoStats result;
    result.frames_submitted = shared_->frames_submitted.load(std::memory_order_relaxed);
    result.frames_decoded = shared_->frames_decoded.load(std::memory_order_relaxed);
    result.frames_dropped = shared_->frames_dropped.load(std::memory_order_relaxed);
    result.decode_errors = shared_->decode_errors.load(std::memory_order_relaxed);
    result.context_rebuilds = shared_->context_rebuilds.load(std::memory_order_relaxed);
    result.width = shared_->width.load(std::memory_order_relaxed);
    result.height = shared_->height.load(std::memory_order_relaxed);
    return result;
}

void TeammateVideoReceiver::stop() {
    if (stopped_) {
        return;
    }
    stopped_ = true;
    shared_->running.store(false, std::memory_order_relaxed);
    shared_->queue_cv.notify_all();
    if (worker_thread_.joinable()) {
        worker_thread_.join();
    }
    SPDLOG_INFO("队友画面解码器已停止");
}

void TeammateVideoReceiver::run() {
    // 用 unique_ptr 装会话：DecodeSession 持有裸指针，重建时必须彻底销毁旧的再建新的。
    std::unique_ptr<DecodeSession> session;
    std::vector<std::uint8_t> current_parameter_sets;
    std::string open_error;
    bool opened = false;
    bool stream_started_logged = false;
    auto last_error_time = std::chrono::steady_clock::now() - std::chrono::seconds(10);
    std::string last_error_text;

    auto report_error = [&](const std::string &message) {
        shared_->decode_errors.fetch_add(1, std::memory_order_relaxed);
        const auto now = std::chrono::steady_clock::now();
        if (last_error_text != message || now - last_error_time >= std::chrono::seconds(2)) {
            last_error_text = message;
            last_error_time = now;
            SPDLOG_WARN("队友画面解码：{}", message);
        }
    };

    while (shared_->running.load(std::memory_order_relaxed)) {
        std::vector<std::uint8_t> frame_bytes;
        {
            std::unique_lock lock(shared_->queue_mutex);
            shared_->queue_cv.wait_for(lock, std::chrono::milliseconds(50), [this]() {
                return !shared_->input_queue.empty()
                       || !shared_->running.load(std::memory_order_relaxed);
            });
            if (shared_->input_queue.empty()) {
                continue;
            }
            frame_bytes = std::move(shared_->input_queue.front());
            shared_->input_queue.pop_front();
        }
        if (frame_bytes.empty()) {
            continue;
        }

        std::vector<std::uint8_t> parameter_sets;
        CollectParameterSetBytes(frame_bytes, parameter_sets);

        // 参数集变了（或第一次收到）→ 重建解码上下文。
        // 这样对端中途换分辨率、或者重开一路流，这边都能自己恢复，不用重启客户端。
        const bool needs_rebuild = !opened || (!parameter_sets.empty()
                                              && !SameParameterSets(parameter_sets, current_parameter_sets));
        if (needs_rebuild) {
            opened = false;
            session = std::make_unique<DecodeSession>();
            if (!session->open(open_error)) {
                session.reset();
                report_error(open_error);
                continue;
            }
            opened = true;
            if (!parameter_sets.empty()) {
                current_parameter_sets = parameter_sets;
            }
            shared_->context_rebuilds.fetch_add(1, std::memory_order_relaxed);
        }

        // send_packet 之后一定要 drain：HEVC 的 P 帧可能被压在解码器里，不 drain 就永远出不来画面。
        session->packet->data = frame_bytes.data();
        session->packet->size = static_cast<int>(frame_bytes.size());
        int ret = avcodec_send_packet(session->codec_context, session->packet);
        session->packet->data = nullptr;
        session->packet->size = 0;
        if (ret < 0 && ret != AVERROR(EAGAIN)) {
            // 丢一帧不影响后面的 P 帧，只记一次。
            report_error("送包失败 ret=" + std::to_string(ret));
            continue;
        }

        while (true) {
            ret = avcodec_receive_frame(session->codec_context, session->frame);
            if (ret == AVERROR(EAGAIN)) {
                // EAGAIN 是正常的：说明解码器要等下一个包
                break;
            }
            if (ret == AVERROR_EOF) {
                break;
            }
            if (ret < 0) {
                report_error("收帧失败 ret=" + std::to_string(ret));
                break;
            }

            app::VideoFrame video_frame;
            const bool converted = ConvertToRgba(*session, session->frame, shared_->config, video_frame);
            av_frame_unref(session->frame);
            if (!converted) {
                report_error("转换为 RGBA 失败");
                continue;
            }

            shared_->frames_decoded.fetch_add(1, std::memory_order_relaxed);
            shared_->width.store(video_frame.width, std::memory_order_relaxed);
            shared_->height.store(video_frame.height, std::memory_order_relaxed);
            if (shared_->on_frame) {
                shared_->on_frame(std::move(video_frame));
            }
            if (!stream_started_logged) {
                stream_started_logged = true;
                SPDLOG_INFO("队友画面已开始");
            }
        }
    }

    SPDLOG_INFO("队友画面解码线程已退出");
}

}// namespace RoboGo::video
