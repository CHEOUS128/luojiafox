#include <fmt/core.h>
#include <chrono>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>
#include <opencv2/opencv.hpp>
#include "tasks/auto_aim/planner/planner.hpp"
#include "tasks/auto_aim/target.hpp"

const std::string keys =
  "{help h usage ? |      | 输出命令行参数说明 }"
  "{obs            | obs.csv | 输入: 观测文件 }"
  "{out            | cmd.csv | 输出: 指令文件 }"
  "{bullet         | 22   | 弹速 m/s }"
  "{@config-path   | configs/standard3.yaml | 配置 }";

auto_aim::Armor make_armor(double x, double y, double yaw_rad)
{
  std::vector<cv::Point2f> kp = {{0,0},{1,0},{1,1},{0,1}};
  cv::Rect box(0,0,1,1);
  auto_aim::Armor a(10, 0.9f, box, kp);
  a.priority = auto_aim::ArmorPriority::first;
  a.xyz_in_world = {x, y, 0.0};
  a.ypr_in_world = {yaw_rad, 0.0, 0.0};
  a.ypd_in_world = {std::atan2(y,x), 0.0, std::sqrt(x*x+y*y)};
  return a;
}

int main(int argc, char * argv[])
{
  cv::CommandLineParser cli(argc, argv, keys);
  if (cli.has("help")) { cli.printMessage(); return 0; }
  auto config_path = cli.get<std::string>(0);
  auto obs_path = cli.get<std::string>("obs");
  auto out_path = cli.get<std::string>("out");
  double bullet = cli.get<double>("bullet");

  auto_aim::Planner planner(config_path);
  Eigen::VectorXd P0_dig{{1,64,1,64,1,64,0.4,100,1,1,1}};

  std::ifstream fin(obs_path);
  if (!fin) { fmt::print("找不到 {}\n", obs_path); return 1; }
  std::string line; std::getline(fin, line);
  std::ofstream fout(out_path);
  fout << "t_cmd,fire,yaw,pitch,yaw_vel\n";

  auto t0 = std::chrono::steady_clock::now();
  auto_aim::Target tgt; bool inited=false; int n=0;
  while (std::getline(fin, line)) {
    if (line.empty()) continue;
    std::stringstream ss(line); std::string tok; std::vector<double> v;
    while (std::getline(ss, tok, ',')) v.push_back(std::stod(tok));
    if (v.size() < 4) continue;
    double t=v[0], x=v[1], y=v[2], yaw_obs=v[3];
    auto a = make_armor(x, y, yaw_obs);
    auto tp = t0 + std::chrono::microseconds(static_cast<long>(t*1e6));
    if (!inited) { tgt = auto_aim::Target(a, tp, 0.2, 4, P0_dig); inited=true; }
    else { tgt.predict(0.01); tgt.update(a); }
    double fire=0, yaw=0, pitch=0, yaw_vel=0;
    auto plan = planner.plan(std::optional<auto_aim::Target>(tgt), bullet);
    if (plan.control) { yaw=plan.yaw; pitch=plan.pitch; yaw_vel=plan.yaw_vel; }
    fire = plan.fire ? 1 : 0;
    fout << fmt::format("{:.4f},{},{:.6f},{:.6f},{:.6f}\n", t, fire?1:0, yaw, pitch, yaw_vel);
    n++;
  }
  fmt::print("[发弹端] 处理 {} 帧 -> {}\n", n, out_path);
  return 0;
}
