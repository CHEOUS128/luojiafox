#include <fmt/core.h>
#include <chrono>
#include <optional>
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
  "{@config-path   | configs/demo.yaml | yaml配置文件路径}";

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
            video_sender.emplace(vs_cfg);
            tools::logger()->info("[VideoSender] 已启用（{}x{} @{:.0f}fps {}kbps preset={} keep_aspect={} sharpen={}）",
                                  vs_cfg.width, vs_cfg.height, vs_cfg.fps, vs_cfg.bitrate_kbps,
                                  vs_cfg.x264_preset, vs_cfg.keep_aspect, vs_cfg.sharpen);
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

    // -------------------- 主循环 --------------------
    while (!exiter.exit()) {
        auto frame_start = std::chrono::steady_clock::now();

        camera.read(img, frame_start);  // 读取图像和时间戳
        if (img.empty()) break;

        // -------------------- 云台状态 --------------------
        Eigen::Quaterniond gimbal_q = gimbal.q(frame_start);
        solver.set_R_gimbal2world(gimbal_q);
        auto gs = gimbal.state();

        // -------------------- 自瞄核心 --------------------
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

    return 0;
}