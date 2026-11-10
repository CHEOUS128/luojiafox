#pragma once
#include <cstdint>
#include <memory>
#include <string>
#include <vector>
#include <functional>
#include "strategy.h"
namespace cw::live {
struct Config {
    Strategy strategy;
    std::string input="素材/car_raw.bgr", ffmpeg="ffmpeg.exe", output;
    int raw_width=1280, raw_height=1024, width=192, height=152;
    double source_fps=20, fps=20, send_rate=50, duration=0;
    bool loop=false, headless=false;
    // Explicit test-only workload, never enabled by the launcher/default preset.
    int test_pre_ms=0, test_encode_ms=0, test_post_ms=0, drop_packet=-1;
    bool test_drop_all=false;
    int test_oversize_frame=0;
};
struct Image {
    int width=0,height=0;
    std::uint64_t id=0,decoded_ordinal=0;
    double source_ms=0;
    std::size_t coded_bytes=0,packets=0;
    std::vector<std::uint8_t> bgr;
};
struct View {
    std::shared_ptr<const Image> left,right;
    std::string status,output,error;
    bool running=false,finished=false;
    double now_ms=0, latency_ms=0, age_ms=0, update_fps=0, decode_fps=0, packets_sec=0;
    int stable_windows=0, failed_windows=0, minimum_stable_fps=0;
    std::uint64_t input=0,encoded=0,decoded=0,displayed=0,sent=0;
    std::uint64_t overwritten=0,oversize=0,backlog=0,network_drop=0,rx_drop=0,unknown=0;
    std::size_t queued_frames=0,rx_packets=0;
};
// Future processing stages execute synchronously inside their actual worker.
using Processor=std::function<void(Image &)>;
class Engine {
public:
    explicit Engine(Config config, Processor preprocess = {}, Processor reconstruct = {});
    ~Engine();
    bool start(std::string &error);
    void stop();
    View view();
    void presented(std::uint64_t frame_id);
private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};
bool validate(const Config &config,std::string &error);
int protocol_self_test();
} // namespace cw::live
