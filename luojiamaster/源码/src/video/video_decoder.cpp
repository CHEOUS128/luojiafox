#include "video_decoder.h"
#include "logging/logging.h"

namespace RoboGo::video {

    VideoDecoder::VideoDecoder(const std::function<void(const AVFrame *frame)> &frame_callback)
        : frame_callback(frame_callback) {
        // 查找HEVC解码器
        const AVCodec *codecs[] = {
                avcodec_find_decoder_by_name("hevc_cuvid"),// Nvidia 硬件加速
                avcodec_find_decoder_by_name("hevc"),      // CPU 软解码
        };
        assert(codecs[0] != nullptr);
        assert(codecs[1] != nullptr);
        const AVCodec *codec = nullptr;
        for (const auto &item: codecs) {
            codec = item;

            // 创建解码器上下文
            av_codec_context = avcodec_alloc_context3(codec);
            if (!av_codec_context) {
                codec = nullptr;
                continue;
            }

            // 配置低延迟参数
            av_codec_context->flags |= AV_CODEC_FLAG_LOW_DELAY;
            av_codec_context->flags2 |= AV_CODEC_FLAG2_FAST;

            if (avcodec_open2(av_codec_context, codec, nullptr) < 0) {
                SPDLOG_WARN("打开HEVC解码器失败 codec={}", codec->name);
                avcodec_free_context(&av_codec_context);
                codec = nullptr;
                av_codec_context = nullptr;
                continue;
            }

            SPDLOG_INFO("HEVC解码器已启用 codec={}", codec->name);
            break;
        }
        if (codec == nullptr) {
            SPDLOG_CRITICAL("Failed to find HEVC decoder");
            std::terminate();
        }

        // 创建解析器
        av_parser_context = av_parser_init(AV_CODEC_ID_HEVC);
        if (!av_parser_context) {
            SPDLOG_CRITICAL("Failed to find HEVC parser");
            std::terminate();
        }
        SPDLOG_INFO("HEVC解析器已初始化");

        // 缓存数据包
        packet = av_packet_alloc();
        if (!packet) {
            SPDLOG_ERROR("分配AVPacket失败");
            std::terminate();
        }

        // 缓存视频帧
        decoded_frame = av_frame_alloc();
        if (!decoded_frame) {
            SPDLOG_ERROR("分配AVFrame失败");
            std::terminate();
        }
    }

    VideoDecoder::~VideoDecoder() {
        SPDLOG_INFO("销毁视频解码器");
        if (av_parser_context) {
            av_parser_close(av_parser_context);
        }
        if (av_codec_context) {
            avcodec_free_context(&av_codec_context);
        }
        if (packet) {
            av_packet_free(&packet);
        }
        if (decoded_frame) {
            av_frame_free(&decoded_frame);
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
        // 每帧都打 TRACE 会把 spdlog 异步队列塞满；只在参数集/IDR 这种关键帧打一行。
        if (nal_type == 32 || nal_type == 33 || nal_type == 34 || nal_type == 19 || nal_type == 20 || nal_type == 21) {
            SPDLOG_DEBUG("关键NAL nal_type={} frame_bytes={}", nal_type, payload.size());
        }
        return nal_type;
    }

    void VideoDecoder::decode_frame(const std::vector<uint8_t> &frame) const {
        if (!av_codec_context || !av_parser_context) {
            SPDLOG_ERROR("视频解码器未完成初始化，无法解码 frame_bytes={}", frame.size());
            return;
        }

        const uint8_t *data = frame.data();
        int size = static_cast<int>(frame.size());

        while (size > 0) {
            int ret = av_parser_parse2(av_parser_context, av_codec_context,
                                       &packet->data, &packet->size,
                                       data, size,
                                       AV_NOPTS_VALUE, AV_NOPTS_VALUE, 0);
            if (ret < 0) {
                SPDLOG_ERROR("视频码流解析错误 ret={} remaining_bytes={}", ret, size);
                break;
            }
            data += ret;
            size -= ret;
            if (packet->size > 0) {
                // 发送到解码器
                ret = avcodec_send_packet(av_codec_context, packet);
                if (ret < 0 && ret != AVERROR(EAGAIN)) {
                    SPDLOG_ERROR("发送包到解码器失败: {}", ret);
                    continue;
                }
                // 接收解码后的帧
                while (ret >= 0) {
                    ret = avcodec_receive_frame(av_codec_context, decoded_frame);
                    if (ret == AVERROR(EAGAIN) || ret == AVERROR_EOF) {
                        break;
                    }
                    if (ret < 0) {
                        SPDLOG_ERROR("解码错误: {}", ret);
                        break;
                    }
                    frame_callback(decoded_frame);
                    av_frame_unref(decoded_frame);
                }
            }
        }
    }

}// namespace RoboGo::video
