#include <fmt/core.h>

#include <algorithm>
#include <cmath>
#include <nlohmann/json.hpp>
#include <opencv2/opencv.hpp>
#include <fstream>
#include <random>
#include <vector>
#include <yaml-cpp/yaml.h>

#include "tasks/auto_aim/planner/planner.hpp"
#include "tasks/auto_aim/target.hpp"
#include "tools/logger.hpp"
#include "tools/math_tools.hpp"

const std::string keys =
  "{help h usage ? |      | 输出命令行参数说明 }"
  "{v              | 1.5  | 目标平移峰值速度 (m/s) }"
  "{d              | 3.0  | 目标距离 (m) }"
  "{delay          | 0.05 | 视觉延迟 (s) }"
  "{noise          | 0.01 | 观测噪声标准差 (m) }"
  "{tclose         | 0.032| 下位机闭环等效时间常数 (s) }"
  "{theta          | 20   | 目标摆动角半幅 (度) }"
  "{frames         | 3000 | 最大帧数 }"
  "{bullet         | 22   | 弹速 (m/s) }"
  "{delay-sweep ds | false| 扫描视觉延迟 }"
  "{scan           | false| 二维扫描 }"
  "{r              | 0.02 | EKF 位置观测噪声 R (=生产值) }"
  "{q1             | 100  | EKF 平动过程噪声 Q }"
  "{q2             | 400  | EKF 旋转过程噪声 Q }"
  "{grid g         | false| 扫描 R x Q }"
  "{ff             | 0.0  | 前馈强度 0~1 }"
  "{ff-sweep       | false| 扫描前馈强度 }"
  "{dc             | 0.0  | 延迟补偿量(s) }"
  "{dc-sweep       | false| 扫描延迟补偿量 }"
  "{nocomp         | false| 关闭延迟补偿(对照用) }"
  "{record         |      | 记录逐帧数据到CSV }"
  "{@config-path   |      | yaml 配置文件路径 }";

constexpr double ARMOR_HALF_WIDTH = 0.0675;
constexpr double SIM_DT = 0.01;

double wrap_pi(double a) { while (a > M_PI) a -= 2*M_PI; while (a < -M_PI) a += 2*M_PI; return a; }

auto_aim::Armor make_obs(double x, double y, double z, std::mt19937 & rng, double noise)
{
  std::normal_distribution<double> nd(0.0, noise);
  std::vector<cv::Point2f> kp = {{0,0},{1,0},{1,1},{0,1}};
  cv::Rect box(0,0,1,1);
  auto_aim::Armor a(10, 0.9f, box, kp);
  a.priority = auto_aim::ArmorPriority::first;
  a.xyz_in_world = {x + nd(rng), y + nd(rng), z + nd(rng)};
  a.ypr_in_world = {0.0, 0.0, 0.0};
  a.ypd_in_world = tools::xyz2ypd(a.xyz_in_world);
  return a;
}

struct SimResult {
  int n = 0, n_hit = 0;
  double mean_abs = 0.0, rms_dyn = 0.0, mean_err = 0.0;
  double hit_rate() const { return n > 0 ? 100.0*n_hit/n : 0.0; }
};

SimResult run_sim(const std::string & config_path, double v, double D, double delay,
                  double noise, double tau, double theta_amp, int frames, double bullet_speed,
                  double r_pos = 2e-2, double q_v1 = 100.0, double q_v2 = 400.0,
                  const std::string & record_file = "", double ff = 0.0, double dc = 0.0, bool nocomp = false)
{
  auto_aim::set_ekf_tuning(r_pos, q_v1, q_v2);
  std::ofstream rec;
  if (!record_file.empty()) rec.open(record_file);
  auto_aim::Planner planner(config_path);
  double A = D*std::tan(theta_amp);
  double f = (A > 1e-6) ? v/(2*M_PI*A) : 0.0;
  double yaw_offset_rad = 0.0;
  { auto yaml = YAML::LoadFile(config_path);
    if (yaml["yaw_offset"]) yaw_offset_rad = yaml["yaw_offset"].as<double>()/57.3; }
  std::mt19937 rng(12345);

  auto obs0 = make_obs(D - 0.2, 0.0, 0.0, rng, noise);
  Eigen::VectorXd P0_dig{{1,64,1,64,1,64,0.4,100,1,1,1}};
  auto_aim::Target tgt(obs0, std::chrono::steady_clock::now(), 0.2, 4, P0_dig);

  double yaw_actual = 0.0;
  int n_frames = frames;
  if (f > 1e-6) n_frames = std::max(frames, static_cast<int>(5.0/f/SIM_DT));
  n_frames = std::min(n_frames, 40000);

  std::vector<double> errs; double sum = 0.0; int n_hit = 0;
  double fly = D/bullet_speed; double last_fire_t = -1.0;

  for (int k=0;k<n_frames;++k) {
    double t = k*SIM_DT;
    double xc_true = A*std::sin(2*M_PI*f*t);
    double t_obs = t - delay;
    double xc_obs = (t_obs>=0) ? A*std::sin(2*M_PI*f*t_obs) : 0.0;
    auto obs = make_obs(D - 0.2, xc_obs, 0.0, rng, noise);

    tgt.predict(SIM_DT);
    tgt.update(obs);

    // 上科大式: 算法用【时间戳自己测】延迟, 实时补偿 (不是外部给的值)
    double measured_latency = nocomp ? 0.0 : (t - t_obs);   // 采集时刻→现在
    auto_aim::Target tc = tgt;
    tc.predict(measured_latency + dc);                      // dc=可选额外微调
    auto_aim::Plan plan = planner.plan(tc, bullet_speed);   // 用纯值重载, 避开墙上时钟
    if (plan.control) yaw_actual += (plan.yaw - yaw_actual)*(SIM_DT/tau) + ff*plan.yaw_vel*SIM_DT;
    if (rec.is_open() && (k % 4 == 0)) {
      double tb_now = std::atan2(A*std::sin(2*M_PI*f*t), D - 0.2) * 57.3;
      double ab_now = (yaw_actual - yaw_offset_rad) * 57.3;
      rec << "F " << t << " " << tb_now << " " << ab_now << "\n";
    }

    if (k % 50 == 0) {
      auto ex = tgt.ekf_x();
      double ekf_brg = std::atan2(ex[2], ex[0]) * 57.3;
      double tbrg = std::atan2(A*std::sin(2*M_PI*f*(t+fly)), D) * 57.3;
      double gp = (yaw_actual - yaw_offset_rad) * 57.3;
      tools::logger()->info(
        "DBG t={:.1f} true_brg={:.1f} ekf_brg={:.1f} plan={:.1f} gimbal={:.1f} hitErr={:.2f} r={:.3f} vy={:.2f}",
        t, tbrg, ekf_brg, plan.yaw*57.3, gp, tbrg - gp, ex[8], ex[3]);
    }

    if (t - last_fire_t >= 0.34) {
      last_fire_t = t;
      double xhit = A*std::sin(2*M_PI*f*(t+fly));
      double true_bearing = std::atan2(xhit, D - 0.2);
      double gimbal_phys = yaw_actual - yaw_offset_rad;
      double impact = D*wrap_pi(true_bearing - gimbal_phys);
      if (t > 0.6) { errs.push_back(impact); sum += impact; if (std::abs(impact)<ARMOR_HALF_WIDTH) ++n_hit; }
      if (rec.is_open()) rec << "S " << t << " " << (true_bearing - impact/D)*57.3 << " " << (std::abs(impact)<ARMOR_HALF_WIDTH?1:0) << "\n";
    }
  }

  SimResult r;
  r.n = static_cast<int>(errs.size());
  if (r.n==0) return r;
  r.n_hit = n_hit; r.mean_err = sum/r.n;
  double ss=0.0;
  for (double e : errs) { r.mean_abs += std::abs(e); double d=e-r.mean_err; ss+=d*d; }
  r.mean_abs /= r.n; r.rms_dyn = std::sqrt(ss/r.n);
  return r;
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
  double noise = cli.get<double>("noise");
  double tau = cli.get<double>("tclose");
  double theta_amp = cli.get<double>("theta")*M_PI/180.0;
  int frames = cli.get<int>("frames");
  double bullet = cli.get<double>("bullet");
  bool dsweep = cli.get<bool>("delay-sweep");
  bool scan = cli.get<bool>("scan");
  double r_pos = cli.get<double>("r");
  std::string record_file = cli.get<std::string>("record");
  double ff = cli.get<double>("ff");
  bool ffsweep = cli.get<bool>("ff-sweep");
  double dc = cli.get<double>("dc");
  bool dcsweep = cli.get<bool>("dc-sweep");
  bool nocomp = cli.get<bool>("nocomp");
  double q_v1 = cli.get<double>("q1");
  double q_v2 = cli.get<double>("q2");
  bool grid = cli.get<bool>("grid");

  if (grid) {
    std::vector<double> rs = {2e-4, 5e-4, 1e-3, 2e-3, 4e-3};
    std::vector<double> qs = {400, 800, 1600, 3200, 6400};
    tools::logger()->info("");
    tools::logger()->info("===== R x Q 网格扫描  v={:.2f} D={:.1f} 延迟={:.0f}ms =====", v, D, delay*1e3);
    fmt::print("   R \\ Q  |");
    for (double Q : qs) fmt::print(" {:>6.0f}", Q);
    fmt::print("\n");
    for (double R : rs) {
      fmt::print("  {:>6.4f} |", R);
      for (double Q : qs) {
        auto rr = run_sim(config_path, v, D, delay, noise, tau, theta_amp, frames, bullet, R, Q, q_v2);
        fmt::print(" {:>5.1f}%", rr.hit_rate());
      }
      fmt::print("\n");
    }
    return 0;
  }

  if (dsweep) {
    tools::logger()->info("");
    tools::logger()->info("===== 延迟扫描(含真实EKF)  v={:.1f} D={:.1f} =====", v, D);
    tools::logger()->info("{:>8} | {:>8} | {:>10} | {:>9}", "延迟ms", "命中率%", "平均|偏差|", "RMS");
    for (double L : std::vector<double>{0.0,0.02,0.05,0.08,0.10,0.15}) {
      auto r = run_sim(config_path, v, D, L, noise, tau, theta_amp, frames, bullet);
      tools::logger()->info("{:>8.0f} | {:>8.1f} | {:>10.3f} | {:>9.3f}", L*1e3, r.hit_rate(), r.mean_abs, r.rms_dyn);
    }
    return 0;
  }

  if (scan) {
    tools::logger()->info("");
    tools::logger()->info("===== 二维扫描  v={:.1f} D={:.1f} =====", v, D);
    std::vector<double> taus = {0.02,0.032,0.05,0.08,0.12};
    fmt::print("          |");
    for (double T : taus) fmt::print(" {:>7.0f}ms", T*1e3);
    fmt::print("\n");
    for (double L : std::vector<double>{0.0,0.03,0.05,0.08,0.12}) {
      fmt::print("{:>6.0f}ms  |", L*1e3);
      for (double T : taus) {
        auto r = run_sim(config_path, v, D, L, noise, T, theta_amp, frames, bullet);
        fmt::print(" {:>7.1f} ", r.hit_rate());
      }
      fmt::print("\n");
    }
    return 0;
  }

  if (ffsweep) {
    tools::logger()->info("");
    tools::logger()->info("===== 前馈强度扫描  v={:.2f} D={:.1f} R={:.4f} =====", v, D, r_pos);
    tools::logger()->info("{:>8} | {:>8} | {:>10}", "前馈ff", "命中率%", "平均|偏差|");
    for (double fv : std::vector<double>{0.0, 0.25, 0.5, 0.75, 1.0}) {
      auto rr = run_sim(config_path, v, D, delay, noise, tau, theta_amp, frames, bullet, r_pos, q_v1, q_v2, "", fv);
      tools::logger()->info("{:>8.2f} | {:>8.1f} | {:>10.3f}", fv, rr.hit_rate(), rr.mean_abs);
    }
    return 0;
  }

  if (dcsweep) {
    tools::logger()->info("");
    tools::logger()->info("===== 延迟补偿扫描  v={:.2f} D={:.1f} 延迟={:.0f}ms R={:.4f} =====", v, D, delay*1e3, r_pos);
    tools::logger()->info("{:>10} | {:>8} | {:>10}", "补偿ms", "命中率%", "平均|偏差|");
    for (double dv : std::vector<double>{0.0, 0.02, 0.04, 0.05, 0.06, 0.08, 0.10}) {
      auto rr = run_sim(config_path, v, D, delay, noise, tau, theta_amp, frames, bullet, r_pos, q_v1, q_v2, "", ff, dv);
      tools::logger()->info("{:>10.0f} | {:>8.1f} | {:>10.3f}", dv*1e3, rr.hit_rate(), rr.mean_abs);
    }
    return 0;
  }

  auto r = run_sim(config_path, v, D, delay, noise, tau, theta_amp, frames, bullet, r_pos, q_v1, q_v2, record_file, ff, dc, nocomp);
  tools::logger()->info("");
  tools::logger()->info("===== 忠实仿真结果 (2D, 含真实EKF) =====");
  tools::logger()->info("  v={:.2f} D={:.2f} 延迟={:.0f}ms 噪声σ={:.0f}mm tau={:.0f}ms",
                        v, D, delay*1e3, noise*1e3, tau*1e3);
  tools::logger()->info("  命中率 = {:.1f}%  ({}/{})", r.hit_rate(), r.n_hit, r.n);
  tools::logger()->info("  平均|偏差| = {:.3f} m   动态RMS = {:.3f} m", r.mean_abs, r.rms_dyn);
  return 0;
}
