#include "target.hpp"

#include <algorithm>
#include <limits>
#include <numeric>
#include <set>

#include "tools/logger.hpp"
#include "tools/math_tools.hpp"

namespace auto_aim
{
namespace
{
// ===== Physical constraint on rotation radius (ref: SHTech 2026 radium_limit) =====
// When the vehicle does not rotate, r is unobservable and will drift,
// even becoming negative -> diverged -> target lost -> jitter / head-swing.
constexpr double R_MIN = 0.12;
constexpr double R_MAX = 0.40;
constexpr double FREEZE_W_THRESHOLD = 0.7;
// =====================================================================
// ===== 双模型可调参数 (换车 / 重新安装后, 需上车实测再填) =====
// =====================================================================
// [ARMOR_KF_DT] 装甲板KF的观测周期(秒) = 1 / 实际帧率
//   怎么测: 上车跑, 看日志 "Current FPS: XX" → 填 1.0/XX
//   例: 83FPS -> 0.0120 ; 100FPS -> 0.0100 ; 50FPS -> 0.0200
constexpr double ARMOR_KF_DT = 0.012;

// [ARMOR_KF_Q] 过程噪声: 越大越信观测(跟得紧, 但可能抖一点)
// [ARMOR_KF_R] 观测噪声: 越大越平滑(但迟钝)
//   先按默认; 若平动速度估计"跟得太慢"→ 调大Q或调小R; 若抖 → 反过来
constexpr double ARMOR_KF_Q = 0.01;
constexpr double ARMOR_KF_R = 0.01;

// [DUAL_MODEL_W_THRESHOLD] 平动/旋转分界(rad/s)
//   |omega| 小于它 → 视为平动, 用装甲板KF的速度; 大于它 → 用整车EKF
//   怎么定: 看日志 "[DualModel] ... w=XX", 挑一个"平动时明显小、自转时明显大"的数
constexpr double DUAL_MODEL_W_THRESHOLD = 0.7;

// 可调 EKF 参数 (默认 = 生产值, 供离线仿真扫描)
double ekf_r_pos = 2e-2;
double ekf_q_v1 = 100.0;
double ekf_q_v2 = 400.0;
}  // namespace

void set_ekf_tuning(double r_pos, double q_v1, double q_v2)
{
  ekf_r_pos = r_pos;
  ekf_q_v1 = q_v1;
  ekf_q_v2 = q_v2;
}

Target::Target(
  const Armor & armor, std::chrono::steady_clock::time_point t, double radius, int armor_num,
  Eigen::VectorXd P0_dig)
: name(armor.name),
  armor_type(armor.type),
  jumped(false),
  last_id(0),
  armor_num_(armor_num),
  t_(t),
  last_switch_time_(t)
{
  auto r = radius;
  priority = armor.priority;
  const Eigen::VectorXd & xyz = armor.xyz_in_world;
  const Eigen::VectorXd & ypr = armor.ypr_in_world;

  auto center_x = xyz[0] + r * std::cos(ypr[0]);
  auto center_y = xyz[1] + r * std::sin(ypr[0]);
  auto center_z = xyz[2];

  // x vx y vy z vz a w r l h
  // a: angle
  // w: angular velocity
  // l: r2 - r1
  // h: z2 - z1
  Eigen::VectorXd x0{{center_x, 0, center_y, 0, center_z, 0, ypr[0], 0, r, 0, 0}};
  Eigen::MatrixXd P0 = P0_dig.asDiagonal();

  auto x_add = [](const Eigen::VectorXd & a, const Eigen::VectorXd & b) -> Eigen::VectorXd {
    Eigen::VectorXd c = a + b;
    c[6] = tools::limit_rad(c[6]);
    return c;
  };

  ekf_ = tools::ExtendedKalmanFilter(x0, P0, x_add);
}

Target::Target(double x, double vyaw, double radius, double h) : armor_num_(4)
{
  Eigen::VectorXd x0{{x, 0, 0, 0, 0, 0, 0, vyaw, radius, 0, h}};
  Eigen::VectorXd P0_dig{{0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0}};
  Eigen::MatrixXd P0 = P0_dig.asDiagonal();

  auto x_add = [](const Eigen::VectorXd & a, const Eigen::VectorXd & b) -> Eigen::VectorXd {
    Eigen::VectorXd c = a + b;
    c[6] = tools::limit_rad(c[6]);
    return c;
  };

  ekf_ = tools::ExtendedKalmanFilter(x0, P0, x_add);
  t_ = std::chrono::steady_clock::now();
  last_switch_time_ = t_;
}

void Target::configure_phase_jump_guard(
  bool enable, double score_thresh, double guard_time_s, double dist_R_scale,
  double angle_R_scale)
{
  enable_phase_jump_guard_ = enable;
  phase_jump_score_thresh_ = score_thresh;
  phase_jump_guard_time_ = guard_time_s;
  phase_jump_dist_R_scale_ = dist_R_scale;
  phase_jump_angle_R_scale_ = angle_R_scale;
}

bool Target::recent_phase_jump() const
{
  if (!enable_phase_jump_guard_) return false;
  return time_since_last_switch() < phase_jump_guard_time_;
}

double Target::time_since_last_switch() const
{
  return tools::delta_time(t_, last_switch_time_);
}

void Target::predict(std::chrono::steady_clock::time_point t)
{
  auto dt = tools::delta_time(t, t_);
  predict(dt);
  t_ = t;
}

void Target::predict(double dt)
{
  Eigen::MatrixXd F{
    {1, dt, 0, 0, 0, 0, 0, 0, 0, 0, 0}, {0, 1, 0, 0, 0, 0, 0, 0, 0, 0, 0},
    {0, 0, 1, dt, 0, 0, 0, 0, 0, 0, 0}, {0, 0, 0, 1, 0, 0, 0, 0, 0, 0, 0},
    {0, 0, 0, 0, 1, dt, 0, 0, 0, 0, 0}, {0, 0, 0, 0, 0, 1, 0, 0, 0, 0, 0},
    {0, 0, 0, 0, 0, 0, 1, dt, 0, 0, 0}, {0, 0, 0, 0, 0, 0, 0, 1, 0, 0, 0},
    {0, 0, 0, 0, 0, 0, 0, 0, 1, 0, 0},  {0, 0, 0, 0, 0, 0, 0, 0, 0, 1, 0},
    {0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 1}};

  double v1, v2;
  if (name == ArmorName::outpost) {
    v1 = 10;
    v2 = 0.1;
  } else {
    v1 = ekf_q_v1;
    v2 = ekf_q_v2;
  }

  auto a = dt * dt * dt * dt / 4;
  auto b = dt * dt * dt / 2;
  auto c = dt * dt;
  Eigen::MatrixXd Q{
    {a * v1, b * v1, 0, 0, 0, 0, 0, 0, 0, 0, 0},
    {b * v1, c * v1, 0, 0, 0, 0, 0, 0, 0, 0, 0},
    {0, 0, a * v1, b * v1, 0, 0, 0, 0, 0, 0, 0},
    {0, 0, b * v1, c * v1, 0, 0, 0, 0, 0, 0, 0},
    {0, 0, 0, 0, a * v1, b * v1, 0, 0, 0, 0, 0},
    {0, 0, 0, 0, b * v1, c * v1, 0, 0, 0, 0, 0},
    {0, 0, 0, 0, 0, 0, a * v2, b * v2, 0, 0, 0},
    {0, 0, 0, 0, 0, 0, b * v2, c * v2, 0, 0, 0},
    {0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0},
    {0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0},
    {0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0}};

  auto f = [&](const Eigen::VectorXd & x) -> Eigen::VectorXd {
    Eigen::VectorXd x_prior = F * x;
    x_prior[6] = tools::limit_rad(x_prior[6]);
    return x_prior;
  };

  if (this->convergened() && this->name == ArmorName::outpost && std::abs(this->ekf_.x[7]) > 2) {
    this->ekf_.x[7] = this->ekf_.x[7] > 0 ? 2.51 : -2.51;
  }

  ekf_.predict(F, Q, f);
}

MatchResult Target::match(const Armor & armor) const
{
  MatchResult result;
  const std::vector<Eigen::Vector4d> & xyza_list = armor_xyza_list();
  if (xyza_list.empty()) return result;

  std::vector<int> candidate_ids;
  candidate_ids.reserve(armor_num_);

  const auto push_unique = [&](int id) {
    if (id < 0 || id >= armor_num_) return;
    if (std::find(candidate_ids.begin(), candidate_ids.end(), id) == candidate_ids.end()) {
      candidate_ids.push_back(id);
    }
  };

  const double w = ekf_.x[7];
  if (
    !enable_phase_jump_guard_ || update_count_ < 2 || !is_converged_ ||
    std::abs(w) < 1.0) {
    for (int i = 0; i < armor_num_; ++i) push_unique(i);
  } else {
    push_unique(last_id);
    push_unique((last_id + 1) % armor_num_);
    push_unique((last_id - 1 + armor_num_) % armor_num_);
    if (candidate_ids.empty()) {
      for (int i = 0; i < armor_num_; ++i) push_unique(i);
    }
  }

  double best_score = std::numeric_limits<double>::infinity();
  int best_id = -1;

  for (int id : candidate_ids) {
    const auto & xyza = xyza_list[id];
    Eigen::Vector3d ypd = tools::xyz2ypd(xyza.head(3));

    double armor_yaw_err = std::abs(tools::limit_rad(armor.ypr_in_world[0] - xyza[3]));
    double aim_yaw_err = std::abs(tools::limit_rad(armor.ypd_in_world[0] - ypd[0]));
    double pitch_err = std::abs(tools::limit_rad(armor.ypd_in_world[1] - ypd[1]));
    double dist_err = std::abs(armor.ypd_in_world[2] - ypd[2]);

    double score = armor_yaw_err + aim_yaw_err + 0.35 * pitch_err + 0.10 * dist_err;

    if (score < best_score) {
      best_score = score;
      best_id = id;
    }
  }

  if (best_id < 0) return result;

  result.valid = true;
  result.id = best_id;
  result.score = best_score;

  if (
    enable_phase_jump_guard_ && update_count_ >= 2 && best_id != last_id &&
    best_score < phase_jump_score_thresh_) {
    int delta = (best_id - last_id + armor_num_) % armor_num_;
    int signed_step = 0;
    if (delta == 1) signed_step = 1;
    else if (delta == armor_num_ - 1) signed_step = -1;

    bool is_neighbor_jump = (signed_step != 0);
    bool direction_ok =
      std::abs(w) < 1.0 ||
      (w > 0 ? (signed_step == 1) : (signed_step == -1));

    result.phase_jump = is_neighbor_jump && direction_ok;
  }

  return result;
}

void Target::update(const Armor & armor)
{
  auto match_result = match(armor);
  if (!match_result.valid) return;
  update(armor, match_result);
}

void Target::update(const Armor & armor, const MatchResult & match_result)
{
  if (!match_result.valid) return;

  int id = match_result.id;

  if (id != 0) jumped = true;

  bool switched = (id != last_id);
  is_switch_ = switched;

  if (switched) {
    switch_count_++;
    last_switch_time_ = t_;
  }

  recent_phase_jump_flag_ = match_result.phase_jump;
  last_id = id;
  update_count_++;

  update_ypda(armor, id, match_result.phase_jump);
}

void Target::update_ypda(const Armor & armor, int id, bool phase_jump)
{
  Eigen::MatrixXd H = h_jacobian(ekf_.x, id);

  auto center_yaw = std::atan2(armor.xyz_in_world[1], armor.xyz_in_world[0]);
  auto delta_angle = tools::limit_rad(armor.ypr_in_world[0] - center_yaw);

  Eigen::VectorXd R_dig{
    {ekf_r_pos, ekf_r_pos, log(std::abs(delta_angle) + 1) + 1,
     log(std::abs(armor.ypd_in_world[2]) + 1) / 200 + 9e-2}};

  if (phase_jump) {
    R_dig[2] *= phase_jump_dist_R_scale_;
    R_dig[3] *= phase_jump_angle_R_scale_;
  }

  Eigen::MatrixXd R = R_dig.asDiagonal();

  auto h = [&](const Eigen::VectorXd & x) -> Eigen::Vector4d {
    Eigen::VectorXd xyz = h_armor_xyz(x, id);
    Eigen::VectorXd ypd = tools::xyz2ypd(xyz);
    auto angle = tools::limit_rad(x[6] + id * 2 * CV_PI / armor_num_);
    return {ypd[0], ypd[1], ypd[2], angle};
  };

  auto z_subtract = [](const Eigen::VectorXd & a, const Eigen::VectorXd & b) -> Eigen::VectorXd {
    Eigen::VectorXd c = a - b;
    c[0] = tools::limit_rad(c[0]);
    c[1] = tools::limit_rad(c[1]);
    c[3] = tools::limit_rad(c[3]);
    return c;
  };

  const Eigen::VectorXd & ypd = armor.ypd_in_world;
  const Eigen::VectorXd & ypr = armor.ypr_in_world;
  Eigen::VectorXd z{{ypd[0], ypd[1], ypd[2], ypr[0]}};

  ekf_.update(z, H, R, h, z_subtract);

  // ===== 双模型(阶段1): 并列运行装甲板模型 (线性KF, 不改行为) =====
  {
    const double dt = ARMOR_KF_DT;  // 观测周期(见上方参数块)
    if (!armor_kf_inited_) {
      Eigen::VectorXd ax0{{armor.xyz_in_world[0], 0, armor.xyz_in_world[1], 0, armor.xyz_in_world[2], 0}};
      Eigen::VectorXd aP0{{1, 64, 1, 64, 1, 64}};
      armor_kf_ = tools::ExtendedKalmanFilter(ax0, aP0.asDiagonal());
      armor_kf_inited_ = true;
    } else {
      Eigen::MatrixXd F6 = Eigen::MatrixXd::Identity(6, 6);
      for (int i = 0; i < 6; i += 2) F6(i, i + 1) = dt;
      // Q 按 dt 缩放: 位置 ~dt^4/4, 位置-速度 ~dt^3/2, 速度 ~dt^2 (三个轴各一组)
      Eigen::Matrix2d q1;
      q1 << dt*dt*dt*dt/4, dt*dt*dt/2,
            dt*dt*dt/2,   dt*dt;
      Eigen::MatrixXd Q6 = Eigen::MatrixXd::Zero(6, 6);
      Q6.block<2,2>(0,0) = q1 * ARMOR_KF_Q;
      Q6.block<2,2>(2,2) = q1 * ARMOR_KF_Q;
      Q6.block<2,2>(4,4) = q1 * ARMOR_KF_Q;
      armor_kf_.predict(F6, Q6);
      Eigen::MatrixXd H6 = Eigen::MatrixXd::Zero(3, 6);
      H6(0, 0) = H6(1, 2) = H6(2, 4) = 1;
      Eigen::VectorXd R6 = Eigen::VectorXd::Ones(3) * ARMOR_KF_R;
      Eigen::VectorXd z6{{armor.xyz_in_world[0], armor.xyz_in_world[1], armor.xyz_in_world[2]}};
      armor_kf_.update(z6, H6, R6.asDiagonal());
    }
    // ===== 双模型核心: 平动时用装甲板KF的干净速度 =====
    if (std::abs(ekf_.x[7]) < DUAL_MODEL_W_THRESHOLD && armor_kf_inited_) {
      ekf_.x[1] = armor_kf_.x[1];   // vx
      ekf_.x[3] = armor_kf_.x[3];   // vy
    }
    // 每 0.5s 打印两套模型的速度估计, 供真车对比
    static auto last_dm_log = std::chrono::steady_clock::now();
    if (tools::delta_time(std::chrono::steady_clock::now(), last_dm_log) >= 0.5) {
      last_dm_log = std::chrono::steady_clock::now();
      tools::logger()->info(
        "[DualModel] armor_vy={:.2f} vehicle_vy={:.2f} w={:.2f} r={:.3f}",
        armor_kf_inited_ ? armor_kf_.x[3] : 0.0, ekf_.x[3], ekf_.x[7], ekf_.x[8]);
    }
  }

  // 半径处理: 不冻结, 仅钳位 (对齐上科大 radium_limit 做法, 2026-10-13 评审)
  // Hard clamp to physically reasonable range
  ekf_.x[8] = std::clamp(ekf_.x[8], R_MIN, R_MAX);
  const double r_plus_l = ekf_.x[8] + ekf_.x[9];
  if (r_plus_l < R_MIN) {
    ekf_.x[9] = R_MIN - ekf_.x[8];
  }
  else if (r_plus_l > R_MAX) {
    ekf_.x[9] = R_MAX - ekf_.x[8];
  }
}

Eigen::VectorXd Target::ekf_x() const { return ekf_.x; }

Eigen::Vector3d Target::armor_kf_xyz() const
{
  if (!armor_kf_inited_) return Eigen::Vector3d::Zero();
  return {armor_kf_.x[0], armor_kf_.x[2], armor_kf_.x[4]};
}
Eigen::Vector3d Target::armor_kf_vel() const
{
  if (!armor_kf_inited_) return Eigen::Vector3d::Zero();
  return {armor_kf_.x[1], armor_kf_.x[3], armor_kf_.x[5]};
}

const tools::ExtendedKalmanFilter & Target::ekf() const { return ekf_; }

std::vector<Eigen::Vector4d> Target::armor_xyza_list() const
{
  std::vector<Eigen::Vector4d> _armor_xyza_list;

  for (int i = 0; i < armor_num_; i++) {
    auto angle = tools::limit_rad(ekf_.x[6] + i * 2 * CV_PI / armor_num_);
    Eigen::Vector3d xyz = h_armor_xyz(ekf_.x, i);
    _armor_xyza_list.push_back({xyz[0], xyz[1], xyz[2], angle});
  }
  return _armor_xyza_list;
}

bool Target::diverged() const
{
  auto r_ok = ekf_.x[8] > 0.05 && ekf_.x[8] < 0.5;
  auto l_ok = ekf_.x[8] + ekf_.x[9] > 0.05 && ekf_.x[8] + ekf_.x[9] < 0.5;

  if (r_ok && l_ok) return false;

  tools::logger()->debug("[Target] r={:.3f}, l={:.3f}", ekf_.x[8], ekf_.x[9]);
  return true;
}

bool Target::convergened()
{
  if (this->name != ArmorName::outpost && update_count_ > 3 && !this->diverged()) {
    is_converged_ = true;
  }

  if (this->name == ArmorName::outpost && update_count_ > 10 && !this->diverged()) {
    is_converged_ = true;
  }

  return is_converged_;
}

Eigen::Vector3d Target::h_armor_xyz(const Eigen::VectorXd & x, int id) const
{
  auto angle = tools::limit_rad(x[6] + id * 2 * CV_PI / armor_num_);
  auto use_l_h = (armor_num_ == 4) && (id == 1 || id == 3);

  auto r = (use_l_h) ? x[8] + x[9] : x[8];
  auto armor_x = x[0] - r * std::cos(angle);
  auto armor_y = x[2] - r * std::sin(angle);
  auto armor_z = (use_l_h) ? x[4] + x[10] : x[4];

  return {armor_x, armor_y, armor_z};
}

Eigen::MatrixXd Target::h_jacobian(const Eigen::VectorXd & x, int id) const
{
  auto angle = tools::limit_rad(x[6] + id * 2 * CV_PI / armor_num_);
  auto use_l_h = (armor_num_ == 4) && (id == 1 || id == 3);

  auto r = (use_l_h) ? x[8] + x[9] : x[8];
  auto dx_da = r * std::sin(angle);
  auto dy_da = -r * std::cos(angle);

  auto dx_dr = -std::cos(angle);
  auto dy_dr = -std::sin(angle);
  auto dx_dl = (use_l_h) ? -std::cos(angle) : 0.0;
  auto dy_dl = (use_l_h) ? -std::sin(angle) : 0.0;

  auto dz_dh = (use_l_h) ? 1.0 : 0.0;

  Eigen::MatrixXd H_armor_xyza{
    {1, 0, 0, 0, 0, 0, dx_da, 0, dx_dr, dx_dl, 0},
    {0, 0, 1, 0, 0, 0, dy_da, 0, dy_dr, dy_dl, 0},
    {0, 0, 0, 0, 1, 0, 0, 0, 0, 0, dz_dh},
    {0, 0, 0, 0, 0, 0, 1, 0, 0, 0, 0}};

  Eigen::VectorXd armor_xyz = h_armor_xyz(x, id);
  Eigen::MatrixXd H_armor_ypd = tools::xyz2ypd_jacobian(armor_xyz);
  Eigen::MatrixXd H_armor_ypda{
    {H_armor_ypd(0, 0), H_armor_ypd(0, 1), H_armor_ypd(0, 2), 0},
    {H_armor_ypd(1, 0), H_armor_ypd(1, 1), H_armor_ypd(1, 2), 0},
    {H_armor_ypd(2, 0), H_armor_ypd(2, 1), H_armor_ypd(2, 2), 0},
    {0, 0, 0, 1}};

  return H_armor_ypda * H_armor_xyza;
}

bool Target::checkinit() { return isinit; }

void Target::set_center(double x, double y, double vx, double vy)
{
  // 状态: [x, vx, y, vy, z, vz, a, w, r, l, h]
  ekf_.x[0] = x;
  ekf_.x[1] = vx;
  ekf_.x[2] = y;
  ekf_.x[3] = vy;
}

}  // namespace auto_aim