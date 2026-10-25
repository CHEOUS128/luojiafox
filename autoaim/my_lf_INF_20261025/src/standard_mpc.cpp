#include <fmt/core.h>
#include <atomic>
#include <chrono>
#include <memory>
#include <optional>
#include <thread>
#include <nlohmann/json.hpp>
#include <opencv2/opencv.hpp>

#include "io/camera.hpp"
#include "io/gimbal/gimbal.hpp"
#include "io/video_sender/video_sender.hpp"
#include "tools/yaml.hpp"
#include "tasks/auto_aim/planner/planner.hpp"
#include "tasks/auto_aim/solver.hpp"
#include "tasks/auto_aim/tracker.hpp"
#include "tasks/auto_aim/yolo.hpp"
#include "tools/exiter.hpp"
#include "tools/img_tools.hpp"
#include "tools/logger.hpp"
#include "tools/math_tools.hpp"
#include "tools/plotter.hpp"

const std::string keys =
  "{help h usage ? |                   | 输出命令行参数说明 }"
  "{@config-path   | configs/demo.yaml | yaml配置文件路径}"
  "{video-fps vf   | -1                | 覆盖 video_fps（<=0 用 yaml 的值）}"
  "{video-width vw | -1                | 覆盖 video_width（<=0 用 yaml）}"
  "{video-height vh | -1               | 覆盖 video_height（<=0 用 yaml）}"
  "{video-crop vc  | -1                | 覆盖 video_crop_size（<=0 用 yaml）}";

int main(int argc, char * argv[])
{
    // -------------------- 参数读取 --------------------
    cv::CommandLineParser cli(argc, argv, keys);
    if (cli.has("help")) {
        cli.printMessage();
        return 0;
    }
    auto config_path = cli.get<std::string>(0);

    tools::Plotter plotter;
    tools::Exiter exiter;

    // -------------------- 模块初始化 --------------------
    io::Camera camera(config_path);  // 工业相机
    auto_aim::YOLO yolo(config_path);
    auto_aim::Solver solver(config_path);
    auto_aim::Tracker tracker(config_path, solver);
    auto_aim::Planner planner(config_path);
    io::Gimbal gimbal(config_path);  // 串口通信对象

    // ---- 视频发送（第二视角：工业相机画面 → 0x0310 → 下位机 → 图传）----
    std::optional<io::VideoSender> video_sender;
    {
        auto cfg_yaml = tools::load(config_path);
        const bool video_enable = cfg_yaml["video_send_enable"]
                                    ? cfg_yaml["video_send_enable"].as<bool>() : false;
        if (video_enable) {
            io::VideoSenderConfig vs_cfg;
            if (cfg_yaml["video_width"])      vs_cfg.width = cfg_yaml["video_width"].as<int>();
            if (cfg_yaml["video_height"])     vs_cfg.height = cfg_yaml["video_height"].as<int>();
            if (cfg_yaml["video_fps"])        vs_cfg.fps = cfg_yaml["video_fps"].as<double>();
            if (cfg_yaml["video_bitrate_kbps"]) vs_cfg.bitrate_kbps = cfg_yaml["video_bitrate_kbps"].as<int>();
            if (cfg_yaml["video_crop_size"])  vs_cfg.crop_size = cfg_yaml["video_crop_size"].as<int>();
            if (cfg_yaml["video_x264_preset"]) vs_cfg.x264_preset = cfg_yaml["video_x264_preset"].as<std::string>();
            if (cfg_yaml["video_keep_aspect"]) vs_cfg.keep_aspect = cfg_yaml["video_keep_aspect"].as<bool>();
            if (cfg_yaml["video_sharpen"])     vs_cfg.sharpen = cfg_yaml["video_sharpen"].as<double>();
            if (cfg_yaml["video_send_rate_hz"]) vs_cfg.send_rate_hz = cfg_yaml["video_send_rate_hz"].as<double>();
            if (cfg_yaml["video_single_packet"]) vs_cfg.single_packet = cfg_yaml["video_single_packet"].as<bool>();
            if (cfg_yaml["video_max_frame_bytes"]) vs_cfg.max_frame_bytes = cfg_yaml["video_max_frame_bytes"].as<int>();
            // 命令行覆盖（做 A/B 时不用改 yaml）
            const auto cli_fps = cli.get<double>("video-fps");
            const auto cli_w = cli.get<int>("video-width");
            const auto cli_h = cli.get<int>("video-height");
            const auto cli_c = cli.get<int>("video-crop");
            if (cli_fps > 0.0) vs_cfg.fps = cli_fps;
            if (cli_w > 0)     vs_cfg.width = cli_w;
            if (cli_h > 0)     vs_cfg.height = cli_h;
            if (cli_c > 0)     vs_cfg.crop_size = cli_c;
            video_sender.emplace(vs_cfg);
            tools::logger()->info("[VideoSender] 已启用（{}x{} @{:.0f}fps {}kbps preset={} keep_aspect={} sharpen={} 单包={} crop={}）",
                                  vs_cfg.width, vs_cfg.height, vs_cfg.fps, vs_cfg.bitrate_kbps,
                                  vs_cfg.x264_preset, vs_cfg.keep_aspect, vs_cfg.sharpen,
                                  vs_cfg.single_packet ? "是" : "否", vs_cfg.crop_size);
        } else {
            tools::logger()->info("[VideoSender] 未启用（配置 video_send_enable: false）");
        }
    }

    cv::Mat img;
    double fps = 0.0;
    auto last_frame_time = std::chrono::steady_clock::now();
    int frame_count = 0;
    constexpr double fps_update_interval = 1.0;
    constexpr int kVideoPacketsPerCycle = 16;  // 每主循环最多发的视频包数（兜底）

    // ---- [诊断 2026-10-23] 主循环看门狗 ----
    // 日志是按 system_clock 打的，而帧率是按 steady_clock 算的，两者一旦对不上
    // （例如机器从休眠恢复），日志会出现“好几秒一跳”甚至“时间倒着走”的假象。
    // 这里每秒记录一次主循环的单调时钟心跳，并在卡住时明确说出卡在哪个环节。
    // 它只读 atomic，自身开销可以忽略；排查完可以整段删掉。
    struct LoopWatchdog
    {
      std::atomic<std::chrono::steady_clock::time_point> beat{std::chrono::steady_clock::now()};
      std::atomic<int> stage{0};  // 1=等相机图 2=等云台数据 3=自瞄检测/跟踪
      std::atomic<long long> count{0};
      std::atomic<bool> stop{false};
    };
    auto wd = std::make_shared<LoopWatchdog>();
    std::thread watchdog([wd] {
      auto last_beat = wd->beat.load();
      int stuck_seconds = 0;
      while (!wd->stop.load()) {
        for (int i = 0; i < 10 && !wd->stop.load(); ++i)
          std::this_thread::sleep_for(std::chrono::milliseconds(100));
        if (wd->stop.load()) break;
        const auto beat = wd->beat.load();
        if (beat != last_beat) {
          last_beat = beat;
          stuck_seconds = 0;
          continue;
        }
        ++stuck_seconds;
        if (stuck_seconds < 3) continue;
        const int stage = wd->stage.load();
        const char * where = "未知位置";
        if (stage == 1) where = "等相机出图（camera.read）";
        else if (stage == 2) where = "等云台数据（gimbal.q：下位机串口没数据/在重连）";
        else if (stage == 3) where = "自瞄检测/跟踪（yolo.detect / tracker.track）";
        tools::logger()->warn(
          "[看门狗] 主循环已连续 {} 秒没有走完一轮，当前卡在：{}。已完成 {} 轮。"
          "（视频发送发不出帧就是这个原因，不是图传的问题）",
          stuck_seconds, where, wd->count.load());
      }
    });

    // -------------------- 主循环 --------------------
    while (!exiter.exit()) {
        auto frame_start = std::chrono::steady_clock::now();
        wd->stage = 1;
        camera.read(img, frame_start);  // 读取图像和时间戳
        if (img.empty()) break;

        // -------------------- 云台状态 --------------------
        wd->stage = 2;
        Eigen::Quaterniond gimbal_q = gimbal.q(frame_start);
        solver.set_R_gimbal2world(gimbal_q);
        auto gs = gimbal.state();

        // -------------------- 自瞄核心 --------------------
        wd->stage = 3;
        auto armors = yolo.detect(img, 0);
        auto targets = tracker.track(armors, frame_start);

        std::optional<auto_aim::Target> target = std::nullopt;
        if (!targets.empty()) {
            target = targets.front();
        }

        // 使用下位机发送的弹速作为最终弹速，并启用 Planner(MPC) 输出
        auto plan = planner.plan(target, gs.bullet_speed);

        // -------------------- 串口发送控制命令 --------------------
        gimbal.send(
            plan.control,
            plan.fire,
            plan.yaw,
            plan.yaw_vel,
            plan.yaw_acc,
            plan.pitch,
            plan.pitch_vel,
            plan.pitch_acc);

        // ---- 视频帧：工业相机画面 → 0x0310 → 同一条串口 ----
        // 发送节奏：VideoSender 内部按 40Hz 节拍出包（平均每轮主循环约 0.5 个包）。
        //   每轮最多发 kVideoPacketsPerCycle 个包作为兜底，串口写失败就丢弃剩余包，
        //   保证自瞄主循环（云台控制）永远不被图传拖慢。
        if (video_sender.has_value()) {
            const auto ts_ns = std::chrono::duration_cast<std::chrono::nanoseconds>(
              frame_start.time_since_epoch()).count();
            int budget = kVideoPacketsPerCycle;
            for (auto & frame : video_sender->feed(img, ts_ns)) {
                if (budget-- <= 0) {
                    tools::logger()->warn("[VideoSender] 单周期包数超预算，丢弃剩余视频包");
                    break;
                }
                if (!gimbal.try_send_raw(frame.data(), frame.size())) {
                    tools::logger()->warn("[VideoSender] 串口写失败，丢弃剩余视频包");
                    break;
                }
            }
        }

        // -------------------- 帧率计算 --------------------
        auto frame_end = std::chrono::steady_clock::now();
        wd->stage = 0;
        wd->beat = frame_end;
        ++wd->count;
        frame_count++;
        double elapsed_time = tools::delta_time(frame_end, last_frame_time);

        if (elapsed_time >= fps_update_interval) {
            fps = frame_count / elapsed_time;
            frame_count = 0;
            last_frame_time = frame_end;
            tools::logger()->info(
              "Current FPS: {:.1f}, Bullet Speed: {:.2f}, Tracker State: {}",
              fps,
              gs.bullet_speed,
              tracker.state());
            // 串口写统计：视频帧只有真的整帧写进 USB 才算成功。
            // short 在涨 = USB CDC 缓冲来不及排空（以前这里被静默当成成功）。
            static io::Gimbal::SerialStats last_ss;
            const auto ss = gimbal.serial_stats();
            tools::logger()->info(
              "[Serial] 控制帧 完整 {} 短写 {} 重发 {} | 视频帧 完整 {} 短写 {} 跨轮 {} 失败 {}",
              ss.ctrl_ok - last_ss.ctrl_ok,
              ss.ctrl_short - last_ss.ctrl_short,
              ss.ctrl_resync - last_ss.ctrl_resync,
              ss.video_ok - last_ss.video_ok,
              ss.video_short - last_ss.video_short,
              ss.video_stall - last_ss.video_stall,
              ss.video_fail - last_ss.video_fail);
            last_ss = ss;
        }

    //     // -------------------- 可视化 --------------------
    //     Eigen::Vector3d euler_deg = tools::eulers(gimbal_q.toRotationMatrix(), 2, 1, 0) * 57.3;

    //     cv::Scalar state_color;
    //     std::string tracker_state = tracker.state();
    //     if (tracker_state == "tracking") state_color = {0, 255, 0};
    //     else if (tracker_state == "temp_lost") state_color = {0, 255, 255};
    //     else if (tracker_state == "detecting") state_color = {255, 255, 0};
    //     else state_color = {0, 0, 255};

    //     tools::draw_text(img, fmt::format("State: {}", tracker_state), {10, 30}, state_color);
    //     tools::draw_text(
    //       img,
    //       fmt::format("gimbal yaw:{:.2f}, pitch:{:.2f}", euler_deg[0], euler_deg[2]),
    //       {10, 60},
    //       {255, 255, 255});
    //     tools::draw_text(
    //       img,
    //       fmt::format("bullet speed:{:.2f} m/s", gs.bullet_speed),
    //       {10, 90},
    //       {255, 255, 255});
    //     tools::draw_text(
    //       img,
    //       fmt::format(
    //         "plan yaw:{:.2f}, pitch:{:.2f}, fire:{}, control:{}",
    //         plan.yaw * 57.3,
    //         plan.pitch * 57.3,
    //         plan.fire,
    //         plan.control),
    //       {10, 120},
    //       {154, 50, 205});
    //     tools::draw_text(
    //       img,
    //       fmt::format(
    //         "plan yaw_vel:{:.2f}, yaw_acc:{:.2f}, pitch_vel:{:.2f}, pitch_acc:{:.2f}",
    //         plan.yaw_vel,
    //         plan.yaw_acc,
    //         plan.pitch_vel,
    //         plan.pitch_acc),
    //       {10, 150},
    //       {154, 50, 205});

    //     // -------------------- 绘图 --------------------
    //     nlohmann::json data;
    //     data["gimbal_yaw"] = euler_deg[0];
    //     data["plan_yaw"] = plan.yaw * 57.3;
    //     data["plan_yaw_vel"] = plan.yaw_vel;
    //     data["plan_yaw_acc"] = plan.yaw_acc;
    //     data["fire"] = plan.fire;
    //     data["gimbal_pitch"] = euler_deg[2];
    //     data["plan_pitch"] = plan.pitch * 57.3;
    //     data["plan_pitch_vel"] = plan.pitch_vel;
    //     data["plan_pitch_acc"] = plan.pitch_acc;
    //     data["track_state"] = tracker_state;
    //     data["control"] = plan.control;
    //     data["bullet_speed"] = gs.bullet_speed;

    //     plotter.plot(data);

    //     cv::resize(img, img, {}, 0.5, 0.5);
    //     cv::imshow("reprojection", img);
    //     if (cv::waitKey(1) == 'q') break;
    }

    wd->stop = true;
    if (watchdog.joinable()) watchdog.join();
    return 0;
}