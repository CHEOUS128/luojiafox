#include "custom_byte_block_video_decoder.h"

namespace RoboGo::video {

namespace {

constexpr std::size_t kSerializedPacketHeaderBytes = 2;
constexpr std::size_t kMaxSupportedPacketBytes = 300;
constexpr std::size_t kTransportLengthPrefixBytes = 2;
constexpr std::size_t kMaxInnerPacketBytes = kMaxSupportedPacketBytes - kTransportLengthPrefixBytes;
constexpr auto kSequenceResyncIdleTimeout = std::chrono::milliseconds(800);
constexpr std::uint16_t kSequenceResyncBackwardThreshold = 2048;

enum class WireFormat {
    Unknown,
    SerializedPacket,
    TransportBundle,
};

struct SerializedPacket {
    std::uint16_t sequence_id = 0;
    std::vector<std::uint8_t> payload;
};

std::uint16_t read_u16_be(const std::uint8_t *input) {
    return static_cast<std::uint16_t>(
            (static_cast<std::uint16_t>(input[0]) << 8) |
            static_cast<std::uint16_t>(input[1]));
}

std::string ffmpeg_error_to_string(const int errnum) {
    std::array<char, AV_ERROR_MAX_STRING_SIZE> buffer{};
    av_strerror(errnum, buffer.data(), buffer.size());
    return std::string(buffer.data());
}

void validate_config(const CustomByteBlockVideoDecoderConfig &config) {
    if (config.max_reorder_frames <= 0) {
        throw std::invalid_argument("CustomByteBlock decoder max_reorder_frames must be positive");
    }
    if (config.max_access_unit_bytes == 0U) {
        throw std::invalid_argument("CustomByteBlock decoder max_access_unit_bytes must be positive");
    }
    if (config.max_assembly_bytes == 0U) {
        throw std::invalid_argument("CustomByteBlock decoder max_assembly_bytes must be positive");
    }
    if (config.max_frame_dimension <= 0) {
        throw std::invalid_argument("CustomByteBlock decoder max_frame_dimension must be positive");
    }
    if (config.max_frame_pixels == 0U) {
        throw std::invalid_argument("CustomByteBlock decoder max_frame_pixels must be positive");
    }
}

SerializedPacket deserialize_serialized_packet(const std::span<const std::uint8_t> wire_bytes) {
    if (wire_bytes.size() < kSerializedPacketHeaderBytes) {
        throw std::invalid_argument("wire packet is smaller than the serialized packet header");
    }
    if (wire_bytes.size() > kMaxSupportedPacketBytes) {
        throw std::invalid_argument("wire packet exceeds 300 bytes");
    }

    SerializedPacket packet;
    packet.sequence_id = read_u16_be(wire_bytes.data());
    packet.payload.assign(wire_bytes.begin() + static_cast<std::ptrdiff_t>(kSerializedPacketHeaderBytes), wire_bytes.end());

    if (packet.payload.empty()) {
        throw std::invalid_argument("serialized packet has empty payload");
    }

    return packet;
}

std::vector<std::span<const std::uint8_t>> unpack_transport_packet_spans(const std::span<const std::uint8_t> transport_packet) {
    if (transport_packet.empty()) {
        throw std::invalid_argument("transport packet is empty");
    }
    if (transport_packet.size() > kMaxSupportedPacketBytes) {
        throw std::invalid_argument("transport packet exceeds 300 bytes");
    }

    std::vector<std::span<const std::uint8_t>> packets;
    std::size_t read_offset = 0U;
    while (read_offset + kTransportLengthPrefixBytes <= transport_packet.size()) {
        const std::uint16_t packet_size = read_u16_be(
                transport_packet.data() + static_cast<std::ptrdiff_t>(read_offset));
        read_offset += kTransportLengthPrefixBytes;

        if (packet_size == 0U) {
            const bool has_non_zero_padding = std::any_of(
                    transport_packet.begin() + static_cast<std::ptrdiff_t>(read_offset),
                    transport_packet.end(),
                    [](const std::uint8_t value) { return value != 0U; });
            if (has_non_zero_padding) {
                throw std::invalid_argument("transport packet has non-zero bytes after padding marker");
            }
            break;
        }

        if (packet_size > kMaxInnerPacketBytes) {
            throw std::invalid_argument("transport packet has an inner packet larger than 298 bytes");
        }
        if (read_offset + packet_size > transport_packet.size()) {
            throw std::invalid_argument("transport packet has a truncated inner packet");
        }

        packets.emplace_back(
                transport_packet.data() + static_cast<std::ptrdiff_t>(read_offset),
                packet_size);
        read_offset += packet_size;
    }

    return packets;
}

const char *wire_format_to_string(const WireFormat format) {
    switch (format) {
        case WireFormat::SerializedPacket:
            return "serialized-packet";
        case WireFormat::TransportBundle:
            return "transport-bundle";
        case WireFormat::Unknown:
        default:
            return "unknown";
    }
}

bool sequence_is_newer(const std::uint16_t lhs, const std::uint16_t rhs) {
    const std::uint16_t delta = static_cast<std::uint16_t>(lhs - rhs);
    return delta != 0U && delta < 0x8000U;
}

std::size_t find_start_code_prefix(const std::uint8_t *data,
                                   const std::size_t size,
                                   const std::size_t offset,
                                   std::size_t *prefix_len) {
    if (prefix_len == nullptr) {
        throw std::invalid_argument("find_start_code_prefix() received null prefix_len");
    }
    if (data == nullptr && size != 0U) {
        throw std::invalid_argument("find_start_code_prefix() received null data with non-zero size");
    }

    for (std::size_t i = offset; (i + 3U) <= size; ++i) {
        if ((i + 4U) <= size &&
            data[i] == 0U &&
            data[i + 1U] == 0U &&
            data[i + 2U] == 0U &&
            data[i + 3U] == 1U) {
            *prefix_len = 4U;
            return i;
        }
        if (data[i] == 0U && data[i + 1U] == 0U && data[i + 2U] == 1U) {
            *prefix_len = 3U;
            return i;
        }
    }

    return size;
}

bool access_unit_has_idr(const std::uint8_t *data, const std::size_t size) {
    if (data == nullptr && size != 0U) {
        throw std::invalid_argument("access_unit_has_idr() received null data with non-zero size");
    }

    std::size_t search_offset = 0U;
    while (search_offset < size) {
        std::size_t prefix_len = 0U;
        const std::size_t prefix_offset = find_start_code_prefix(data, size, search_offset, &prefix_len);
        if (prefix_offset >= size) {
            break;
        }

        const std::size_t nal_header_offset = prefix_offset + prefix_len;
        if (nal_header_offset >= size) {
            break;
        }

        const std::uint8_t nal_type = data[nal_header_offset] & 0x1fU;
        if (nal_type == 5U) {
            return true;
        }

        search_offset = nal_header_offset + 1U;
    }

    return false;
}

}// namespace

struct CustomByteBlockVideoDecoder::Impl {
    struct PendingFrame {
        std::uint32_t frame_id = 0;
        std::int64_t pts = 0;
        bool keyframe = false;
    };

    AVCodecContext *codec_context = nullptr;
    AVCodecParserContext *parser = nullptr;
    SwsContext *sws_context = nullptr;
    AVFrame *frame = nullptr;
    AVPacket *packet = nullptr;
    std::deque<PendingFrame> pending_frames;
    int source_width = 0;
    int source_height = 0;
    AVPixelFormat source_format = AV_PIX_FMT_NONE;
    std::uint32_t next_frame_id = 0;
    std::int64_t next_pts = 0;
    bool has_expected_sequence = false;
    std::uint16_t expected_sequence_id = 0;
    WireFormat wire_format = WireFormat::Unknown;
    std::chrono::steady_clock::time_point last_packet_time = std::chrono::steady_clock::time_point::min();
};

namespace {

bool multiplication_would_overflow(const std::size_t lhs, const std::size_t rhs) {
    return lhs != 0U && rhs > (std::numeric_limits<std::size_t>::max() / lhs);
}

std::size_t decoded_frame_byte_count(const int width,
                                     const int height,
                                     const CustomByteBlockVideoDecoderConfig &config) {
    if (width <= 0 || height <= 0) {
        throw std::runtime_error("Decoded frame has invalid dimensions");
    }
    if (width > config.max_frame_dimension || height > config.max_frame_dimension) {
        throw std::runtime_error("Decoded frame exceeds configured dimension limit");
    }

    const std::size_t width_u = static_cast<std::size_t>(width);
    const std::size_t height_u = static_cast<std::size_t>(height);
    if (multiplication_would_overflow(width_u, height_u)) {
        throw std::runtime_error("Decoded frame pixel count overflows size_t");
    }

    const std::size_t pixel_count = width_u * height_u;
    if (pixel_count > config.max_frame_pixels) {
        throw std::runtime_error("Decoded frame exceeds configured pixel limit");
    }
    if (multiplication_would_overflow(pixel_count, 4U)) {
        throw std::runtime_error("Decoded frame RGBA byte count overflows size_t");
    }

    return pixel_count * 4U;
}

void free_decoder_impl(CustomByteBlockVideoDecoder::Impl &impl) {
    if (impl.parser != nullptr) {
        av_parser_close(impl.parser);
        impl.parser = nullptr;
    }
    if (impl.packet != nullptr) {
        av_packet_free(&impl.packet);
    }
    if (impl.frame != nullptr) {
        av_frame_free(&impl.frame);
    }
    if (impl.codec_context != nullptr) {
        avcodec_free_context(&impl.codec_context);
    }
    if (impl.sws_context != nullptr) {
        sws_freeContext(impl.sws_context);
        impl.sws_context = nullptr;
    }
}

void reset_parser(CustomByteBlockVideoDecoder::Impl *impl) {
    if (impl == nullptr) {
        throw std::invalid_argument("reset_parser() received null impl");
    }
    if (impl->parser != nullptr) {
        av_parser_close(impl->parser);
        impl->parser = nullptr;
    }
    impl->parser = av_parser_init(AV_CODEC_ID_H264);
    if (impl->parser == nullptr) {
        throw std::runtime_error("Failed to allocate H.264 parser");
    }
}

void reset_decoder_after_loss(CustomByteBlockVideoDecoder::Impl *impl) {
    if (impl == nullptr) {
        throw std::invalid_argument("reset_decoder_after_loss() received null impl");
    }
    if (impl->codec_context != nullptr) {
        avcodec_flush_buffers(impl->codec_context);
    }
    impl->pending_frames.clear();
    reset_parser(impl);
}

void ensure_sws_context(CustomByteBlockVideoDecoder::Impl &impl, const AVFrame *frame) {
    const auto frame_format = static_cast<AVPixelFormat>(frame->format);
    if (impl.sws_context != nullptr &&
        impl.source_width == frame->width &&
        impl.source_height == frame->height &&
        impl.source_format == frame_format) {
        return;
    }

    if (impl.sws_context != nullptr) {
        sws_freeContext(impl.sws_context);
        impl.sws_context = nullptr;
    }

    impl.sws_context = sws_getContext(
            frame->width, frame->height, frame_format,
            frame->width, frame->height, AV_PIX_FMT_RGBA,
            SWS_BILINEAR, nullptr, nullptr, nullptr);
    if (impl.sws_context == nullptr) {
        throw std::runtime_error("Failed to create decoder swscale context");
    }

    impl.source_width = frame->width;
    impl.source_height = frame->height;
    impl.source_format = frame_format;
}

std::vector<CustomByteBlockDecodedFrame> drain_decoded_frames(CustomByteBlockVideoDecoder::Impl &impl,
                                                              const CustomByteBlockVideoDecoderConfig &config) {
    std::vector<CustomByteBlockDecodedFrame> frames;
    while (true) {
        const int ret_receive = avcodec_receive_frame(impl.codec_context, impl.frame);
        if (ret_receive == AVERROR(EAGAIN) || ret_receive == AVERROR_EOF) {
            break;
        }
        if (ret_receive < 0) {
            throw std::runtime_error("Failed to receive decoded frame: " + ffmpeg_error_to_string(ret_receive));
        }

        ensure_sws_context(impl, impl.frame);

        CustomByteBlockDecodedFrame output;
        output.width = impl.frame->width;
        output.height = impl.frame->height;
        output.rgba.resize(decoded_frame_byte_count(output.width, output.height, config));

        std::uint8_t *dst_slices[4] = {nullptr, nullptr, nullptr, nullptr};
        int dst_stride[4] = {0, 0, 0, 0};

        // sws_scale may use SIMD stores that assume FFmpeg-style aligned buffers.
        const int alloc_ret = av_image_alloc(dst_slices,
                                             dst_stride,
                                             output.width,
                                             output.height,
                                             AV_PIX_FMT_RGBA,
                                             32);
        if (alloc_ret < 0) {
            throw std::runtime_error("Failed to allocate temporary RGBA image buffer");
        }

        const int scaled_height = sws_scale(
                impl.sws_context,
                impl.frame->data,
                impl.frame->linesize,
                0,
                impl.frame->height,
                dst_slices,
                dst_stride);
        if (scaled_height != output.height) {
            av_freep(&dst_slices[0]);
            throw std::runtime_error("Failed to convert decoded frame to RGBA");
        }
        for (int y = 0; y < output.height; ++y) {
            const auto *src_row = dst_slices[0] + static_cast<std::ptrdiff_t>(y) * dst_stride[0];
            auto *dst_row = output.rgba.data() +
                            static_cast<std::size_t>(y) * static_cast<std::size_t>(output.width) * 4U;
            std::memcpy(dst_row, src_row, static_cast<std::size_t>(output.width) * 4U);
        }
        av_freep(&dst_slices[0]);

        if (!impl.pending_frames.empty()) {
            const CustomByteBlockVideoDecoder::Impl::PendingFrame meta = impl.pending_frames.front();
            impl.pending_frames.pop_front();
            output.frame_id = meta.frame_id;
            output.pts = meta.pts;
            output.keyframe = meta.keyframe;
        } else {
            output.frame_id = impl.next_frame_id++;
            output.pts = (impl.frame->pts == AV_NOPTS_VALUE) ? impl.next_pts++ : impl.frame->pts;
            output.keyframe = (impl.frame->flags & AV_FRAME_FLAG_KEY) != 0;
        }
        frames.push_back(std::move(output));
    }
    return frames;
}

std::vector<CustomByteBlockDecodedFrame> decode_access_unit(CustomByteBlockVideoDecoder::Impl *impl,
                                                            const CustomByteBlockVideoDecoderConfig &config,
                                                            const std::uint8_t *access_unit,
                                                            const std::size_t access_unit_size) {
    if (impl == nullptr) {
        throw std::invalid_argument("decode_access_unit() received null impl");
    }
    if (access_unit == nullptr && access_unit_size != 0U) {
        throw std::invalid_argument("decode_access_unit() received null data with non-zero size");
    }
    if (access_unit_size == 0U) {
        return {};
    }

    const bool keyframe = access_unit_has_idr(access_unit, access_unit_size);
    // 【诊断用改动】不再因为"还没收到关键帧"而丢弃数据。
    //
    // 原逻辑：waiting_for_keyframe 为 true 时，非 IDR 帧直接 return {}，
    //         必须等到一个完整 IDR 关键帧才开始解码。
    // 问题：发送端关键帧间隔 20 帧。若帧率很低（或链路丢包导致关键帧丢失），
    //       客户端可能长时间一帧都不解码，界面一直显示"等待视频流"，
    //       无法判断数据到底有没有到达。
    // 现在：所有帧都送进解码器。非 IDR 起始的流会先解出花屏/残缺画面，
    //       等 IDR 到达后自动恢复正常。这样能立刻确认"数据到了"。
    //
    // 目的：定位链路问题。确认链路无误后，可以恢复为只等关键帧以提升观感。
    (void)keyframe;

    if (access_unit_size > config.max_access_unit_bytes) {
        SPDLOG_WARN("丢弃过大的 CustomByteBlock 访问单元 bytes={} limit={}",
                    access_unit_size,
                    config.max_access_unit_bytes);
        reset_decoder_after_loss(impl);
        return {};
    }
    if (access_unit_size > static_cast<std::size_t>(std::numeric_limits<int>::max())) {
        SPDLOG_WARN("丢弃超出 FFmpeg 支持范围的 CustomByteBlock 访问单元 bytes={}",
                    access_unit_size);
        reset_decoder_after_loss(impl);
        return {};
    }

    const int ret_packet = av_new_packet(impl->packet, static_cast<int>(access_unit_size));
    if (ret_packet < 0) {
        SPDLOG_WARN("分配 CustomByteBlock 解码包失败 message={}", ffmpeg_error_to_string(ret_packet));
        reset_decoder_after_loss(impl);
        return {};
    }

    std::memcpy(impl->packet->data, access_unit, access_unit_size);
    impl->packet->pts = impl->next_pts;
    impl->packet->dts = impl->next_pts;
    impl->packet->flags = keyframe ? AV_PKT_FLAG_KEY : 0;

    const int ret_send = avcodec_send_packet(impl->codec_context, impl->packet);
    av_packet_unref(impl->packet);
    if (ret_send < 0) {
        SPDLOG_WARN("CustomByteBlock 解码器拒收访问单元 keyframe={} message={}",
                    keyframe,
                    ffmpeg_error_to_string(ret_send));
        reset_decoder_after_loss(impl);
        return {};
    }

    impl->pending_frames.push_back({
            impl->next_frame_id++,
            impl->next_pts++,
            keyframe,
    });

    try {
        return drain_decoded_frames(*impl, config);
    } catch (const std::exception &error) {
        SPDLOG_WARN("CustomByteBlock 解码输出失败 message={}", error.what());
        reset_decoder_after_loss(impl);
        return {};
    }
}

void append_decoded_frames(std::vector<CustomByteBlockDecodedFrame> &target,
                           std::vector<CustomByteBlockDecodedFrame> &&incoming) {
    target.insert(target.end(),
                  std::make_move_iterator(incoming.begin()),
                  std::make_move_iterator(incoming.end()));
}

std::vector<CustomByteBlockDecodedFrame> feed_parser(CustomByteBlockVideoDecoder::Impl *impl,
                                                     const CustomByteBlockVideoDecoderConfig &config,
                                                     const std::uint8_t *payload,
                                                     std::size_t payload_size) {
    if (impl == nullptr) {
        throw std::invalid_argument("feed_parser() received null impl");
    }
    if (payload == nullptr && payload_size != 0U) {
        throw std::invalid_argument("feed_parser() received null payload with non-zero size");
    }

    std::vector<CustomByteBlockDecodedFrame> output;
    const std::uint8_t *cursor = payload;
    while (payload_size > 0U) {
        std::uint8_t *parsed_data = nullptr;
        int parsed_size = 0;
        const int consumed = av_parser_parse2(
                impl->parser,
                impl->codec_context,
                &parsed_data,
                &parsed_size,
                cursor,
                static_cast<int>(payload_size),
                AV_NOPTS_VALUE,
                AV_NOPTS_VALUE,
                0);
        if (consumed < 0) {
            throw std::runtime_error("Failed to parse H.264 byte stream: " + ffmpeg_error_to_string(consumed));
        }

        cursor += consumed;
        payload_size -= static_cast<std::size_t>(consumed);

        if (parsed_size > 0) {
            append_decoded_frames(
                    output,
                    decode_access_unit(
                            impl,
                            config,
                            parsed_data,
                            static_cast<std::size_t>(parsed_size)));
        }

        if (consumed == 0 && parsed_size == 0) {
            break;
        }
    }

    return output;
}

std::vector<CustomByteBlockDecodedFrame> flush_parser(CustomByteBlockVideoDecoder::Impl *impl,
                                                      const CustomByteBlockVideoDecoderConfig &config) {
    if (impl == nullptr) {
        throw std::invalid_argument("flush_parser() received null impl");
    }

    std::vector<CustomByteBlockDecodedFrame> output;
    while (true) {
        std::uint8_t *parsed_data = nullptr;
        int parsed_size = 0;
        const int consumed = av_parser_parse2(
                impl->parser,
                impl->codec_context,
                &parsed_data,
                &parsed_size,
                nullptr,
                0,
                AV_NOPTS_VALUE,
                AV_NOPTS_VALUE,
                0);
        if (consumed < 0) {
            throw std::runtime_error("Failed while flushing H.264 parser: " + ffmpeg_error_to_string(consumed));
        }
        (void) consumed;

        if (parsed_size <= 0) {
            break;
        }

        append_decoded_frames(
                output,
                decode_access_unit(
                        impl,
                        config,
                        parsed_data,
                        static_cast<std::size_t>(parsed_size)));
    }

    return output;
}

std::vector<CustomByteBlockDecodedFrame> process_serialized_packet(CustomByteBlockVideoDecoder::Impl &impl,
                                                                   const CustomByteBlockVideoDecoderConfig &config,
                                                                   const SerializedPacket &packet) {
    const auto now = std::chrono::steady_clock::now();
    if (!impl.has_expected_sequence) {
        impl.expected_sequence_id = static_cast<std::uint16_t>(packet.sequence_id + 1U);
        impl.has_expected_sequence = true;
    } else if (packet.sequence_id == impl.expected_sequence_id) {
        impl.expected_sequence_id = static_cast<std::uint16_t>(impl.expected_sequence_id + 1U);
    } else if (sequence_is_newer(packet.sequence_id, impl.expected_sequence_id)) {
        // 【诊断用改动】丢包时不再重置解码器。
        //
        // 原逻辑：发现序列号跳变（丢包）就 reset_decoder_after_loss()，
        //         会 avcodec_flush_buffers + 清空待输出帧 + 重置解析器，
        //         并要求重新等关键帧。
        // 问题：链路一旦频繁丢包，每个包都会触发一次重置，
        //       解码器永远处于"刚被清空"的状态，一帧都出不来。
        // 现在：只记录日志，不重置。让 FFmpeg 自己去扛。
        //       花屏/残帧可以接受，重点是能出画面。
        SPDLOG_WARN("CustomByteBlock 丢包（不重置解码器）expected_sequence={} received_sequence={}",
                    impl.expected_sequence_id,
                    packet.sequence_id);
        impl.expected_sequence_id = static_cast<std::uint16_t>(packet.sequence_id + 1U);
        impl.has_expected_sequence = true;
    } else {
        const std::uint16_t backward_distance = static_cast<std::uint16_t>(impl.expected_sequence_id - packet.sequence_id);
        const bool idle_too_long = impl.last_packet_time != std::chrono::steady_clock::time_point::min()
                                   && (now - impl.last_packet_time) > kSequenceResyncIdleTimeout;
        const bool backward_jump_too_large = backward_distance > kSequenceResyncBackwardThreshold;
        if (idle_too_long || backward_jump_too_large) {
            const auto idle_ms = impl.last_packet_time == std::chrono::steady_clock::time_point::min()
                                         ? 0LL
                                         : std::chrono::duration_cast<std::chrono::milliseconds>(now - impl.last_packet_time).count();
            SPDLOG_INFO("CustomByteBlock 序列重同步（不重置解码器）expected_sequence={} received_sequence={} idle_ms={} backward_distance={}",
                        impl.expected_sequence_id,
                        packet.sequence_id,
                        idle_ms,
                        backward_distance);
            impl.expected_sequence_id = static_cast<std::uint16_t>(packet.sequence_id + 1U);
            impl.has_expected_sequence = true;
        } else {
            // 【诊断用改动】乱序/重复包也照常送进解码器，不再静默丢弃。
            // 原来的 return {} 会在网络抖动时大量丢包，
            // 极低帧率场景下可能一帧都到不了解码器。
            SPDLOG_DEBUG("CustomByteBlock 乱序或重复包（仍送入解码器）expected_sequence={} received_sequence={}",
                         impl.expected_sequence_id,
                         packet.sequence_id);
        }
    }

    impl.last_packet_time = now;
    return feed_parser(
            &impl,
            config,
            packet.payload.data(),
            packet.payload.size());
}

std::vector<CustomByteBlockDecodedFrame> decode_serialized_packet_bytes(CustomByteBlockVideoDecoder::Impl &impl,
                                                                         const CustomByteBlockVideoDecoderConfig &config,
                                                                         const std::span<const std::uint8_t> wire_bytes) {
    SerializedPacket packet = deserialize_serialized_packet(wire_bytes);
    return process_serialized_packet(impl, config, packet);
}

std::vector<CustomByteBlockDecodedFrame> decode_transport_bundle_bytes(CustomByteBlockVideoDecoder::Impl &impl,
                                                                       const CustomByteBlockVideoDecoderConfig &config,
                                                                       const std::span<const std::uint8_t> wire_bytes) {
    const auto packet_spans = unpack_transport_packet_spans(wire_bytes);
    if (packet_spans.empty()) {
        return {};
    }

    std::vector<CustomByteBlockDecodedFrame> decoded_frames;
    for (const std::span<const std::uint8_t> packet_span: packet_spans) {
        SerializedPacket packet = deserialize_serialized_packet(packet_span);
        append_decoded_frames(decoded_frames, process_serialized_packet(impl, config, packet));
    }
    return decoded_frames;
}

std::vector<CustomByteBlockDecodedFrame> decode_wire_bytes_with_format(CustomByteBlockVideoDecoder::Impl &impl,
                                                                       const CustomByteBlockVideoDecoderConfig &config,
                                                                       const std::span<const std::uint8_t> wire_bytes,
                                                                       const WireFormat format) {
    switch (format) {
        case WireFormat::SerializedPacket:
            return decode_serialized_packet_bytes(impl, config, wire_bytes);
        case WireFormat::TransportBundle:
            return decode_transport_bundle_bytes(impl, config, wire_bytes);
        case WireFormat::Unknown:
        default:
            throw std::invalid_argument("unsupported wire format");
    }
}

bool should_try_transport_first(const WireFormat wire_format, const std::span<const std::uint8_t> wire_bytes) {
    if (wire_format == WireFormat::TransportBundle) {
        return true;
    }
    if (wire_format == WireFormat::SerializedPacket) {
        return false;
    }

    if (wire_bytes.size() == kMaxSupportedPacketBytes) {
        return true;
    }
    if (wire_bytes.size() < kTransportLengthPrefixBytes) {
        return false;
    }

    const std::uint16_t first_inner_size = read_u16_be(wire_bytes.data());
    if (first_inner_size != 0U &&
        first_inner_size <= kMaxInnerPacketBytes &&
        (kTransportLengthPrefixBytes + static_cast<std::size_t>(first_inner_size)) <= wire_bytes.size()) {
        return true;
    }

    return false;
}

}// namespace

CustomByteBlockVideoDecoder::CustomByteBlockVideoDecoder(CustomByteBlockVideoDecoderConfig config)
    : config_(std::move(config)),
      impl_(std::make_unique<Impl>()) {
    validate_config(config_);

    const AVCodec *codec = avcodec_find_decoder(AV_CODEC_ID_H264);
    if (codec == nullptr) {
        throw std::runtime_error("Unable to find an H.264 decoder");
    }

    impl_->codec_context = avcodec_alloc_context3(codec);
    if (impl_->codec_context == nullptr) {
        throw std::runtime_error("Failed to allocate decoder context");
    }

    const int ret_open = avcodec_open2(impl_->codec_context, codec, nullptr);
    if (ret_open < 0) {
        const std::string message = ffmpeg_error_to_string(ret_open);
        free_decoder_impl(*impl_);
        throw std::runtime_error("Failed to open decoder: " + message);
    }

    impl_->frame = av_frame_alloc();
    impl_->packet = av_packet_alloc();
    if (impl_->frame == nullptr || impl_->packet == nullptr) {
        free_decoder_impl(*impl_);
        throw std::runtime_error("Failed to allocate decoder frame or packet");
    }

    try {
        reset_parser(impl_.get());
    } catch (...) {
        free_decoder_impl(*impl_);
        throw;
    }
}

CustomByteBlockVideoDecoder::~CustomByteBlockVideoDecoder() {
    if (impl_ != nullptr) {
        free_decoder_impl(*impl_);
    }
}

CustomByteBlockVideoDecoder::CustomByteBlockVideoDecoder(CustomByteBlockVideoDecoder &&) noexcept = default;
CustomByteBlockVideoDecoder &CustomByteBlockVideoDecoder::operator=(CustomByteBlockVideoDecoder &&) noexcept = default;

std::vector<CustomByteBlockDecodedFrame> CustomByteBlockVideoDecoder::push_packet_bytes(std::span<const std::uint8_t> wire_bytes) {
    const bool transport_first = should_try_transport_first(impl_->wire_format, wire_bytes);
    const WireFormat primary_format = transport_first ? WireFormat::TransportBundle : WireFormat::SerializedPacket;
    const WireFormat fallback_format = transport_first ? WireFormat::SerializedPacket : WireFormat::TransportBundle;

    try {
        std::vector<CustomByteBlockDecodedFrame> decoded_frames =
                decode_wire_bytes_with_format(*impl_, config_, wire_bytes, primary_format);
        if (impl_->wire_format != primary_format) {
            SPDLOG_INFO("CustomByteBlock 输入包格式切换 old_format={} new_format={}",
                        wire_format_to_string(impl_->wire_format),
                        wire_format_to_string(primary_format));
            impl_->wire_format = primary_format;
        }
        return decoded_frames;
    } catch (const std::exception &primary_error) {
        try {
            std::vector<CustomByteBlockDecodedFrame> decoded_frames =
                    decode_wire_bytes_with_format(*impl_, config_, wire_bytes, fallback_format);
            if (impl_->wire_format != fallback_format) {
                SPDLOG_INFO("CustomByteBlock 输入包格式切换 old_format={} new_format={}",
                            wire_format_to_string(impl_->wire_format),
                            wire_format_to_string(fallback_format));
                impl_->wire_format = fallback_format;
            }
            SPDLOG_DEBUG("CustomByteBlock 输入包按 {} 解析失败，已自动回退到 {} bytes={} primary_error={}",
                         wire_format_to_string(primary_format),
                         wire_format_to_string(fallback_format),
                         wire_bytes.size(),
                         primary_error.what());
            return decoded_frames;
        } catch (const std::exception &fallback_error) {
            SPDLOG_WARN("丢弃无效的 CustomByteBlock 输入包 bytes={} primary_format={} primary_error={} fallback_format={} fallback_error={}",
                        wire_bytes.size(),
                        wire_format_to_string(primary_format),
                        primary_error.what(),
                        wire_format_to_string(fallback_format),
                        fallback_error.what());
            return {};
        }
    }
}

std::vector<CustomByteBlockDecodedFrame> CustomByteBlockVideoDecoder::flush() {
    std::vector<CustomByteBlockDecodedFrame> output;
    try {
        append_decoded_frames(output, flush_parser(impl_.get(), config_));

        const int ret_send = avcodec_send_packet(impl_->codec_context, nullptr);
        if (ret_send < 0 && ret_send != AVERROR_EOF) {
            reset_decoder_after_loss(impl_.get());
            throw std::runtime_error("Failed to flush decoder: " + ffmpeg_error_to_string(ret_send));
        }

        append_decoded_frames(output, drain_decoded_frames(*impl_, config_));
        return output;
    } catch (...) {
        reset_decoder_after_loss(impl_.get());
        throw;
    }
}

}// namespace RoboGo::video
