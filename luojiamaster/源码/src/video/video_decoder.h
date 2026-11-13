#pragma once

#include <pch.h>

namespace RoboGo::video {

    class VideoDecoder {
    private:
        AVCodecContext *av_codec_context = nullptr;
        AVCodecParserContext *av_parser_context = nullptr;
        AVPacket *packet = nullptr;
        AVFrame *decoded_frame = nullptr;
        std::function<void(const AVFrame *frame)> frame_callback;
        std::vector<uint8_t> padded_input;
        void release_resources() noexcept;
        int drain_frames() const;

    public:
        explicit VideoDecoder(const std::function<void(const AVFrame *frame)> &frame_callback);

        ~VideoDecoder();

        static int get_nal_type(const std::vector<uint8_t> &payload);

        void decode_frame(const std::vector<uint8_t> &frame);
        void reset();
    };

}// namespace RoboGo::video
