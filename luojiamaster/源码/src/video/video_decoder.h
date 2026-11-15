#pragma once

#include <pch.h>

namespace RoboGo::video {

    struct VideoDecoderConfig {
        // Optional offline observation: parser output bytes and its absolute byte
        // offset establish provenance across arbitrary input chunk boundaries.
        // Returning AV_NOPTS_VALUE leaves the decoded image explicitly unassociated.
        std::function<std::int64_t(std::span<const std::uint8_t>, std::int64_t, std::int64_t)> on_access_unit;
    };

    struct VideoDecoderStatistics {
        std::uint64_t parsed_packets = 0;
        std::uint64_t packet_allocations = 0;
        std::uint64_t packet_buffer_reuses = 0;
        std::uint64_t copied_packet_bytes = 0;
    };

    class VideoDecoder {
    private:
        AVCodecContext *av_codec_context = nullptr;
        AVCodecParserContext *av_parser_context = nullptr;
        AVPacket *packet = nullptr;
        AVFrame *decoded_frame = nullptr;
        std::function<void(const AVFrame *frame)> frame_callback;
        std::vector<uint8_t> padded_input;
        VideoDecoderConfig config_;
        std::atomic<std::uint64_t> parsed_packets_{0};
        std::atomic<std::uint64_t> packet_allocations_{0};
        std::atomic<std::uint64_t> copied_packet_bytes_{0};
        void release_resources() noexcept;
        int drain_frames() const;

    public:
        explicit VideoDecoder(const std::function<void(const AVFrame *frame)> &frame_callback,
                              VideoDecoderConfig config = {});

        ~VideoDecoder();

        static int get_nal_type(const std::vector<uint8_t> &payload);

        // Observation token is optional. The parser, not output order, associates it
        // with an access unit; a decoded frame without a PTS remains unassociated.
        void decode_frame(const std::vector<uint8_t> &frame, std::int64_t input_token = AV_NOPTS_VALUE);
        void reset();
        [[nodiscard]] VideoDecoderStatistics statistics() const noexcept;
    };

}// namespace RoboGo::video
