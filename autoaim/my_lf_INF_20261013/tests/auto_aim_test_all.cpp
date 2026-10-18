#include <fmt/core.h>
#include <chrono>
#include <nlohmann/json.hpp>
#include <opencv2/opencv.hpp>
#include "io/camera.hpp"
#include "io/gimbal/gimbal.hpp"
#include "tasks/auto_aim/aimer.hpp"
#include "tasks/auto_aim/solver.hpp"
#include "tasks/auto_aim/tracker.hpp"
#include "tasks/auto_aim/yolo.hpp"
#include "tools/exiter.hpp"
#include "tools/img_tools.hpp"
#include "tools/logger.hpp"
#include "tools/math_tools.hpp"
#include "tools/plotter.hpp"

const std::string keys =
  "{help h usage ? |                        | 输出命令行参数说明 }"
  "{@config-path   | configs/demo.yaml | yaml配置文件路径}";

int main(int argc, char * argv[])
{
  cv::CommandLineParser cli(argc, argv, keys);
  if (cli.has("help")) { cli.printMessage(); return 0; }
  auto config_path = cli.get<std::string>(0);

  tools::Exiter exiter;
  tools::Plotter plotter;
  io::Camera camera(config_path);
  io::Gimbal gimbal(config_path);
  auto_aim::YOLO yolo(config_path);
  auto_aim::Solver solver(config_path);
  auto_aim::Tracker tracker(config_path, solver);
  auto_aim::Aimer aimer(config_path);

  cv::Mat img;
  std::chrono::steady_clock::time_point frame_start;
  io::Command last_command{false, false, 0.f, 0.f};

  // 调试显示开关
  bool debug_show = true;

  while (!exiter.exit()) {
    camera.read(img, frame_start);
    if (img.empty()) continue;

    // 云台姿态 + 状态
    Eigen::Quaterniond gimbal_q = gimbal.q(frame_start);
    solver.set_R_gimbal2world(gimbal_q);
    io::GimbalState gs = gimbal.state();
    double bullet_speed = gs.bullet_speed;
    if (!std::isfinite(bullet_speed) || bullet_speed < 1.0) bullet_speed = 27.0;

    // 云台当前欧拉角(度)
    Eigen::Vector3d euler_now = tools::eulers(gimbal_q.toRotationMatrix(), 2, 1, 0) * 57.3;

    // 识别
    auto armors = yolo.detect(img, 0);
    auto targets = tracker.track(armors, frame_start);

    // 瞄准
    auto command = aimer.aim(targets, frame_start, bullet_speed, false);

    // ============ 可视化：把所有识别框/瞄准点叠在画面上 ============
    cv::Mat display = img.clone();

    // ① 所有识别到的装甲板：画框 + 角点 + 名字
    for (const auto & armor : armors) {
      // 4个角点
      for (const auto & p : armor.points) {
        cv::circle(display, p, 2, cv::Scalar(0, 255, 0), -1);
      }
      // 外框
      cv::rectangle(display, armor.box, cv::Scalar(0, 255, 0), 2);
      // 文字：颜色 + 类型
      std::string color_str = (armor.color == auto_aim::Color::red) ? "R" : "B";
      tools::draw_text(display, color_str, armor.center, {0, 255, 0});
    }

    // ② 当前跟踪目标(如果有)：画出预测装甲板 + 瞄准点
    if (!targets.empty()) {
      auto target = targets.front();
      // 画目标装甲板(绿)
      std::vector<Eigen::Vector4d> armor_xyza_list = target.armor_xyza_list();
      for (const Eigen::Vector4d & xyza : armor_xyza_list) {
        auto image_points = solver.reproject_armor(xyza.head(3), xyza[3], target.armor_type, target.name);
        tools::draw_points(display, image_points, {0, 255, 0});
      }
      // 画瞄准点 aim_point(红=有效, 蓝=无效)
      auto aim_point = aimer.debug_aim_point;
      Eigen::Vector4d aim_xyza = aim_point.xyza;
      if (aim_xyza != Eigen::Vector4d::Zero()) {
        auto image_points = solver.reproject_armor(aim_xyza.head(3), aim_xyza[3], target.armor_type, target.name);
        if (aim_point.valid)
          tools::draw_points(display, image_points, {0, 0, 255});
        else
          tools::draw_points(display, image_points, {255, 0, 0});
      }
    }

    // ③ 顶部调试信息(画在画面上，不刷屏)
    if (debug_show) {
      tools::draw_text(display,
        fmt::format("state:{} bullet:{:.1f}",
          tracker.state(), bullet_speed),
        {10, 30}, {255, 255, 255});
      tools::draw_text(display,
        fmt::format("now yaw:{:.1f} pitch:{:.1f}", euler_now[0], euler_now[1]),
        {10, 60}, {0, 255, 255});
      tools::draw_text(display,
        fmt::format("cmd yaw:{:.1f} pitch:{:.1f} fire:{} ctrl:{}",
          command.yaw * 57.3, command.pitch * 57.3, command.shoot ? 1 : 0, command.control ? 1 : 0),
        {10, 90}, {154, 50, 205});
      tools::draw_text(display, fmt::format("armors:{}", armors.size()), {10, 120}, {255, 255, 255});
    }

    // ④ 发送云台指令
    gimbal.send(command.control, command.shoot,
      static_cast<float>(command.yaw), 0.f, 0.f,
      static_cast<float>(command.pitch), 0.f, 0.f);

    // ⑤ 显示窗口
    cv::resize(display, display, {}, 0.7, 0.7);
    cv::imshow("auto_aim_test_all", display);
    int key = cv::waitKey(1);
    if (key == 'q') break;
  }

  gimbal.send(false, false, 0.f, 0.f, 0.f, 0.f, 0.f, 0.f);
  return 0;
}
