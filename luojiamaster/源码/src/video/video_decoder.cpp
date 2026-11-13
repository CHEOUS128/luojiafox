#include "video_decoder.h"
#include "logging/logging.h"

namespace RoboGo::video {

VideoDecoder::VideoDecoder(const std::function<void(const AVFrame *frame)> &callback)
    : frame_callback(callback) {
    try {
        const AVCodec *candidates[] = {avcodec_find_decoder_by_name("hevc_cuvid"),
                                      avcodec_find_decoder_by_name("hevc")};
        for (const auto *codec : candidates) {
            if (codec == nullptr) continue;
            av_codec_context = avcodec_alloc_context3(codec);
            if (av_codec_context == nullptr) continue;
            av_codec_context->flags |= AV_CODEC_FLAG_LOW_DELAY;
            av_codec_context->flags2 |= AV_CODEC_FLAG2_FAST;
            if (avcodec_open2(av_codec_context, codec, nullptr) >= 0) {
                SPDLOG_INFO("HEVC decoder enabled codec={}", codec->name);
                break;
            }
            SPDLOG_WARN("HEVC decoder open failed codec={}", codec->name);
            avcodec_free_context(&av_codec_context);
        }
        if (av_codec_context == nullptr) throw std::runtime_error("Unable to open HEVC decoder");
        av_parser_context = av_parser_init(AV_CODEC_ID_HEVC);
        packet = av_packet_alloc();
        decoded_frame = av_frame_alloc();
        if (av_parser_context == nullptr || packet == nullptr || decoded_frame == nullptr)
            throw std::runtime_error("Unable to allocate HEVC parser/frame/packet");
    } catch (...) {
        release_resources();
        throw;
    }
}

VideoDecoder::~VideoDecoder() { release_resources(); }

void VideoDecoder::release_resources() noexcept {
    if (av_parser_context != nullptr) {
        av_parser_close(av_parser_context);
        av_parser_context = nullptr;
    }
    avcodec_free_context(&av_codec_context);
    av_packet_free(&packet);
    av_frame_free(&decoded_frame);
}

void VideoDecoder::reset() {
    avcodec_flush_buffers(av_codec_context);
    av_packet_unref(packet);
    av_frame_unref(decoded_frame);
    if (av_parser_context != nullptr) av_parser_close(av_parser_context);
    av_parser_context = av_parser_init(AV_CODEC_ID_HEVC);
    if (av_parser_context == nullptr) throw std::runtime_error("Unable to reset HEVC parser");
}

int VideoDecoder::drain_frames() const {
    int count = 0;
    for (; count < 256; ++count) {
        const int status = avcodec_receive_frame(av_codec_context, decoded_frame);
        if (status == AVERROR(EAGAIN) || status == AVERROR_EOF) return count;
        if (status < 0) {
            SPDLOG_WARN("HEVC receive_frame failed status={}", status);
            return count;
        }
        try {
            if (frame_callback) frame_callback(decoded_frame);
        } catch (...) {
            av_frame_unref(decoded_frame);
            throw;
        }
        av_frame_unref(decoded_frame);
    }
    throw std::runtime_error("HEVC decoder exceeded bounded output work");
}

void VideoDecoder::decode_frame(const std::vector<uint8_t> &frame) {
    constexpr std::size_t kMaximumFrameBytes = 8U * 1024U * 1024U;
    if (frame.empty()) return;
    if (frame.size() > kMaximumFrameBytes || av_codec_context == nullptr || av_parser_context == nullptr) {
        throw std::runtime_error("Invalid/uninitialized HEVC decoder input");
    }
    padded_input.resize(frame.size() + AV_INPUT_BUFFER_PADDING_SIZE);
    std::memcpy(padded_input.data(), frame.data(), frame.size());
    std::fill(padded_input.begin() + static_cast<std::ptrdiff_t>(frame.size()), padded_input.end(), 0);
    const uint8_t *data = padded_input.data();
    int remaining = static_cast<int>(frame.size());
    unsigned no_progress = 0;
    while (remaining > 0) {
        uint8_t *parsed_data = nullptr;
        int parsed_size = 0;
        const int consumed = av_parser_parse2(av_parser_context, av_codec_context,
                                             &parsed_data, &parsed_size, data, remaining,
                                             AV_NOPTS_VALUE, AV_NOPTS_VALUE, 0);
        if (consumed < 0 || consumed > remaining) {
            reset();
            throw std::runtime_error("HEVC parser rejected input/progress");
        }
        if (consumed == 0 && (++no_progress > 8 || parsed_size == 0)) {
            reset();
            throw std::runtime_error("HEVC parser made no progress");
        }
        if (consumed != 0) no_progress = 0;
        data += consumed;
        remaining -= consumed;
        if (parsed_size <= 0) continue;
        if (static_cast<std::size_t>(parsed_size) > kMaximumFrameBytes) {
            reset();
            throw std::runtime_error("HEVC access unit exceeds size limit");
        }
        av_packet_unref(packet);
        if (av_new_packet(packet, parsed_size) < 0) throw std::runtime_error("HEVC packet allocation failed");
        std::memcpy(packet->data, parsed_data, static_cast<std::size_t>(parsed_size));
        int sent = avcodec_send_packet(av_codec_context, packet);
        if (sent == AVERROR(EAGAIN)) {
            drain_frames();
            sent = avcodec_send_packet(av_codec_context, packet);
        }
        av_packet_unref(packet);
        if (sent < 0) {
            SPDLOG_WARN("HEVC send_packet failed status={}", sent);
            continue;
        }
        drain_frames();
    }
}

    int VideoDecoder::get_nal_type(const std::vector<uint8_t> &payload) {
        if (payload.size() < 4) {
            SPDLOG_DEBUG("视频帧过短，无法识别NAL类型 bytes={}", payload.size());
            return -1;
        }

        size_t start_offset = std::string::npos;
        if (payload[0] == 0x00 && payload[1] == 0x00 &&
            payload[2] == 0x00 && payload[3] == 0x01) {
            start_offset = 4;
        } else if (payload[0] == 0x00 && payload[1] == 0x00 && payload[2] == 0x01) {
            start_offset = 3;
        }

        if (start_offset == std::string::npos || payload.size() <= start_offset) {
            SPDLOG_DEBUG("视频帧缺少起始码，无法识别NAL类型 bytes={}", payload.size());
            return -1;
        }

        const int nal_type = payload[start_offset] >> 1 & 0x3F;
        SPDLOG_TRACE("识别到NAL类型 nal_type={} frame_bytes={}", nal_type, payload.size());
        return nal_type;
    }


} // namespace RoboGo::video
