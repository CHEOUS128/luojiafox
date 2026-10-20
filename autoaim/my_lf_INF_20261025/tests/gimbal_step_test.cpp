#include <fmt/core.h>

#include <chrono>
#include <opencv2/opencv.hpp>
#include <thread>

#include "io/gimbal/gimbal.hpp"
#include "tools/exiter.hpp"
#include "tools/logger.hpp"
#include "tools/math_tools.hpp"

using namespace std::chrono_literals;

const std::string keys =
  "{help h usage ? |      | 输出命令行参数说明 }"
  "{step           | 10   | 阶跃幅度 (度) }"
  "{hold           | 1.5  | 每段保持时间 (s) }"
  "{axis           | pitch| 测试轴: pitch 或 yaw }"
  "{@config-path   |      | yaml 配置文件路径 }";

int main(int argc, char * argv[])
{
  cv::CommandLineParser cli(argc, argv, keys);
  if (cli.has("help")) { cli.printMessage(); return 0; }
  auto config_path = cli.get<std::string>(0);
  if (config_path.empty()) { cli.printMessage(); return 0; }
  double step_deg = cli.get<double>("step");
  double hold = cli.get<double>("hold");
  std::string axis = cli.get<std::string>("axis");

  tools::Exiter exiter;
  io::Gimbal gimbal(config_path);
  std::this_thread::sleep_for(1s);

  auto gs0 = gimbal.state();
  double base_yaw_deg = gs0.yaw * 57.3;
  double base_pitch_deg = gs0.pitch * 57.3;

  tools::logger()->info(
    "[StepTest] axis={} step={:.1f}deg hold={:.1f}s  base_yaw={:.2f} base_pitch={:.2f}", axis,
    step_deg, hold, base_yaw_deg, base_pitch_deg);
  if (axis == "yaw")
    tools::logger()->warn("[StepTest] yaw 测试会转动整个底盘, 请注意周围空间!");

  auto t0 = std::chrono::steady_clock::now();
  double last_log = -1.0;

  while (!exiter.exit()) {
    double t = tools::delta_time(std::chrono::steady_clock::now(), t0);

    double cmd_deg;
    if (t < hold) cmd_deg = 0.0;
    else if (t < 2 * hold) cmd_deg = step_deg;
    else if (t < 3 * hold) cmd_deg = -step_deg;
    else if (t < 4 * hold) cmd_deg = 0.0;
    else break;

    auto gs = gimbal.state();
    double cur_yaw = gs.yaw * 57.3;
    double cur_pitch = gs.pitch * 57.3;

    double out_yaw_deg, out_pitch_deg;
    if (axis == "yaw") { out_yaw_deg = base_yaw_deg + cmd_deg; out_pitch_deg = base_pitch_deg; }
    else { out_yaw_deg = base_yaw_deg; out_pitch_deg = base_pitch_deg + cmd_deg; }

    gimbal.send(true, false, static_cast<float>(out_yaw_deg / 57.3), 0.f, 0.f,
                static_cast<float>(out_pitch_deg / 57.3), 0.f, 0.f);

    if (t - last_log >= 0.01) {
      last_log = t;
      double target = (axis == "yaw") ? out_yaw_deg : out_pitch_deg;
      double actual = (axis == "yaw") ? cur_yaw : cur_pitch;
      tools::logger()->info("STEP t={:.3f} cmd={:.2f} actual={:.2f} err={:.2f}", t, target, actual, target - actual);
    }
    std::this_thread::sleep_for(2ms);
  }

  gimbal.send(false, false, 0.f, 0.f, 0.f, 0.f, 0.f, 0.f);
  tools::logger()->info("[StepTest] 结束");
  return 0;
}
