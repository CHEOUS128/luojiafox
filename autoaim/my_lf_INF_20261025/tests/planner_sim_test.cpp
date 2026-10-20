#include <fmt/core.h>

#include <algorithm>
#include <cmath>
#include <nlohmann/json.hpp>
#include <opencv2/opencv.hpp>
#include <vector>
#include <yaml-cpp/yaml.h>

#include "tasks/auto_aim/planner/planner.hpp"
#include "tools/logger.hpp"
#include "tools/plotter.hpp"

const std::string keys =
  "{help h usage ? |      | 输出命令行参数说明 }"
  "{v              | 1.0  | 目标平移峰值速度 (m/s) }"
  "{d              | 3.0  | 目标距离 (m) }"
  "{delay          | 0.0  | 模拟视觉延迟 (s) }"
  "{tau            | 0.05 | 云台一阶惯性时间常数 (s) }"
  "{theta          | 20   | 目标摆动角半幅 (度) }"
  "{frames         | 1500 | 仿真帧数 (每帧 10ms) }"
  "{bullet         | 22   | 弹速 (m/s) }"
  "{sweep s        | false| 扫描不同平移速度 }"
  "{delay-sweep ds | false| 扫描不同视觉延迟 }"
  "{tau-sweep ts   | false| 扫描不同云台响应时间常数 }"
  "{@config-path   |      | yaml 配置文件路径 }";

constexpr double ARMOR_HALF_WIDTH = 0.0675;
constexpr double SIM_DT = 0.01;

double wrap_pi(double a)
{
  while (a > M_PI) a -= 2 * M_PI;
  while (a < -M_PI) a += 2 * M_PI;
  return a;
}

struct SimResult
{
  int n = 0;
  int n_hit = 0;
  double mean_err = 0.0;
  double mean_abs = 0.0;
  double rms_dyn = 0.0;
  double max_abs = 0.0;
  double hit_rate() const { return n > 0 ? 100.0 * n_hit / n : 0.0; }
};

SimResult run_sim(
  const std::string & config_path, double v, double D, double delay, double tau, double theta_amp,
  int frames, double bullet_speed, tools::Plotter * plotter)
{
  auto_aim::Planner planner(config_path);

  double A = D * std::tan(theta_amp);
  double f = (A > 1e-6) ? v / (2 * M_PI * A) : 0.0;

  double yaw_offset_rad = 0.0;
  {
    auto yaml = YAML::LoadFile(config_path);
    if (yaml["yaw_offset"]) yaw_offset_rad = yaml["yaw_offset"].as<double>() / 57.3;
  }

  auto_aim::Target target(0.0, 0.0, 0.01, 0.0);
  double gimbal_yaw = 0.0;

  int n_frames = frames;
  if (f > 1e-6) n_frames = std::max(frames, static_cast<int>(5.0 / f / SIM_DT));
  n_frames = std::min(n_frames, 40000);

  std::vector<double> errs;
  double sum_err = 0.0;

  for (int k = 0; k < n_frames; ++k) {
    double t = k * SIM_DT;

    double t_in = t - delay;
    double cx_in = 0.0, vx_in = 0.0;
    if (t_in >= 0) {
      cx_in = A * std::sin(2 * M_PI * f * t_in);
      vx_in = A * 2 * M_PI * f * std::cos(2 * M_PI * f * t_in);
    }
    target.set_center(cx_in, D, vx_in, 0.0);

    auto plan = planner.plan(target, bullet_speed);
    if (!plan.control) continue;

    gimbal_yaw += (plan.yaw - gimbal_yaw) * (SIM_DT / tau);

    double fly_time = D / bullet_speed;
    double t_hit = t + fly_time;
    double cx_hit = A * std::sin(2 * M_PI * f * t_hit);
    double true_bearing = std::atan2(D, cx_hit);

    double gimbal_phys = gimbal_yaw - yaw_offset_rad;
    double err = wrap_pi(true_bearing - gimbal_phys);
    double impact = D * err;

    if (t > 0.5) {
      errs.push_back(impact);
      sum_err += impact;
    }

    if (plotter) {
      nlohmann::json data;
      data["t"] = t;
      data["true_bearing"] = true_bearing;
      data["gimbal_yaw"] = gimbal_yaw;
      data["impact"] = impact;
      plotter->plot(data);
    }
  }

  SimResult r;
  r.n = static_cast<int>(errs.size());
  if (r.n == 0) return r;
  r.mean_err = sum_err / r.n;
  double ss = 0.0;
  for (double e : errs) {
    r.mean_abs += std::abs(e);
    r.max_abs = std::max(r.max_abs, std::abs(e));
    double d = e - r.mean_err;
    ss += d * d;
    if (std::abs(e) < ARMOR_HALF_WIDTH) r.n_hit++;
  }
  r.mean_abs /= r.n;
  r.rms_dyn = std::sqrt(ss / r.n);
  return r;
}

void print_header(const char * title)
{
  tools::logger()->info("");
  tools::logger()->info("===== {} =====", title);
  tools::logger()->info(
    "{:>9} | {:>8} | {:>10} | {:>9} | {:>9}", "参数", "命中率%", "平均|偏差|", "动态RMS", "最大偏差");
}

int main(int argc, char * argv[])
{
  cv::CommandLineParser cli(argc, argv, keys);
  if (cli.has("help")) { cli.printMessage(); return 0; }
  auto config_path = cli.get<std::string>(0);
  if (config_path.empty()) { cli.printMessage(); return 0; }

  double v = cli.get<double>("v");
  double D = cli.get<double>("d");
  double delay = cli.get<double>("delay");
  double tau = cli.get<double>("tau");
  double theta_amp = cli.get<double>("theta") * M_PI / 180.0;
  int frames = cli.get<int>("frames");
  double bullet_speed = cli.get<double>("bullet");
  bool sweep = cli.get<bool>("sweep");
  bool delay_sweep = cli.get<bool>("delay-sweep");
  bool tau_sweep = cli.get<bool>("tau-sweep");

  tools::Plotter plotter;

  if (sweep) {
    print_header(fmt::format("平动速度扫描  (D={:.1f}m, 延迟={:.0f}ms)", D, delay * 1e3).c_str());
    for (double vv : std::vector<double>{0.5, 1.0, 1.5, 2.0, 2.5}) {
      auto r = run_sim(config_path, vv, D, delay, tau, theta_amp, frames, bullet_speed, nullptr);
      tools::logger()->info(
        "{:>7.1f} m/s | {:>8.1f} | {:>10.3f} | {:>9.3f} | {:>9.3f}", vv, r.hit_rate(), r.mean_abs,
        r.rms_dyn, r.max_abs);
    }
    return 0;
  }

  if (delay_sweep) {
    print_header(fmt::format("视觉延迟扫描  (v={:.1f}m/s, D={:.1f}m)", v, D).c_str());
    for (double L : std::vector<double>{0.0, 0.02, 0.05, 0.08, 0.10, 0.15}) {
      auto r = run_sim(config_path, v, D, L, tau, theta_amp, frames, bullet_speed, nullptr);
      tools::logger()->info(
        "{:>6.0f} ms | {:>8.1f} | {:>10.3f} | {:>9.3f} | {:>9.3f}", L * 1e3, r.hit_rate(),
        r.mean_abs, r.rms_dyn, r.max_abs);
    }
    return 0;
  }

  if (tau_sweep) {
    print_header(fmt::format("云台响应扫描  (v={:.1f}m/s, D={:.1f}m)", v, D).c_str());
    for (double T : std::vector<double>{0.02, 0.05, 0.08, 0.12, 0.20}) {
      auto r = run_sim(config_path, v, D, delay, T, theta_amp, frames, bullet_speed, nullptr);
      tools::logger()->info(
        "{:>6.0f} ms | {:>8.1f} | {:>10.3f} | {:>9.3f} | {:>9.3f}", T * 1e3, r.hit_rate(),
        r.mean_abs, r.rms_dyn, r.max_abs);
    }
    return 0;
  }

  auto r = run_sim(config_path, v, D, delay, tau, theta_amp, frames, bullet_speed, &plotter);

  tools::logger()->info("");
  tools::logger()->info("=============== 仿真结果 ===============");
  tools::logger()->info("  目标平移峰值速度 v   = {:.2f} m/s", v);
  tools::logger()->info("  目标距离          D  = {:.2f} m", D);
  tools::logger()->info("  模拟视觉延迟         = {:.0f} ms", delay * 1e3);
  tools::logger()->info("  云台时间常数         = {:.0f} ms", tau * 1e3);
  tools::logger()->info("  弹速                 = {:.1f} m/s", bullet_speed);
  tools::logger()->info("  有效帧数             = {}", r.n);
  tools::logger()->info("  ---------------------------------------");
  tools::logger()->info("  命中率               = {:.1f} %  ({}/{})", r.hit_rate(), r.n_hit, r.n);
  tools::logger()->info("  平均落点偏差(绝对值) = {:.3f} m", r.mean_abs);
  tools::logger()->info("  平均落点偏差(有符号) = {:.3f} m", r.mean_err);
  tools::logger()->info("  动态误差 RMS         = {:.3f} m", r.rms_dyn);
  tools::logger()->info("  最大偏差             = {:.3f} m", r.max_abs);
  tools::logger()->info("========================================");

  return 0;
}
