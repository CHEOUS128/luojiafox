#include "gimbal.hpp"

#include <algorithm>
#include <cmath>
#include <iomanip>
#include <sstream>
#include "tools/crc.hpp"
#include "tools/logger.hpp"
#include "tools/math_tools.hpp"
#include "tools/yaml.hpp"

namespace io
{
constexpr double DEG2RAD = M_PI / 180.0;
constexpr double RAD2DEG = 180.0 / M_PI;

constexpr bool DEBUG_UNWRAP = false;

// ============ 上位机整形参数 ============
// 1) 低通滤波: 滤掉视觉角度的"小幅高频抖动"
// 2) 指令限速: 把阶跃磨成斜坡, 减轻下位机超调
static bool g_lpf_enable = true;
static double g_lpf_alpha = 0.3;      // 0~1, 越小越平滑(但越滞后)
static bool g_slew_enable = true;
static double g_yaw_max_rate = 300.0;    // 度/秒
static double g_pitch_max_rate = 200.0;
static bool g_debug_send = true;
static bool g_three_order = false;   // 三阶指令发送开关 (默认关)

// =====================================================================
// 角度解缠
// =====================================================================
static double unwrap_angle(double raw_deg, double cur_deg)
{
  double diff = std::fmod(raw_deg - cur_deg + 180.0, 360.0);
  if (diff < 0.0) diff += 360.0;
  diff -= 180.0;
  return cur_deg + diff;
}

namespace
{
// ---------- 低通滤波状态 ----------
double g_lpf_yaw = 0.0;
double g_lpf_pitch = 0.0;
bool g_lpf_inited = false;

void lpf_reset() { g_lpf_inited = false; }

// 一阶低通: out = alpha*new + (1-alpha)*prev
// 输入输出单位: 度 (yaw 为解缠后的绝对角)
void lowpass(double & yaw_deg, double & pitch_deg)
{
  if (!g_lpf_inited) {
    g_lpf_yaw = yaw_deg;
    g_lpf_pitch = pitch_deg;
    g_lpf_inited = true;
    return;
  }
  if (!g_lpf_enable) {
    g_lpf_yaw = yaw_deg;
    g_lpf_pitch = pitch_deg;
    return;
  }
  g_lpf_yaw = g_lpf_alpha * yaw_deg + (1.0 - g_lpf_alpha) * g_lpf_yaw;
  g_lpf_pitch = g_lpf_alpha * pitch_deg + (1.0 - g_lpf_alpha) * g_lpf_pitch;
  yaw_deg = g_lpf_yaw;
  pitch_deg = g_lpf_pitch;
}

// ---------- 限速状态 ----------
double g_last_yaw_deg = 0.0;
double g_last_pitch_deg = 0.0;
bool g_slew_inited = false;
std::chrono::steady_clock::time_point g_last_slew_time = std::chrono::steady_clock::now();

void slew_reset() { g_slew_inited = false; g_lpf_inited = false; }

void slew_limit(double & yaw_deg, double & pitch_deg)
{
  auto now = std::chrono::steady_clock::now();
  if (!g_slew_inited) {
    g_last_yaw_deg = yaw_deg;
    g_last_pitch_deg = pitch_deg;
    g_last_slew_time = now;
    g_slew_inited = true;
    return;
  }

  double dt = std::chrono::duration<double>(now - g_last_slew_time).count();
  g_last_slew_time = now;
  if (dt < 1e-4) dt = 1e-4;
  if (dt > 0.1) dt = 0.1;

  const double yaw_raw = yaw_deg;
  const double pitch_raw = pitch_deg;
  double dyaw = yaw_deg - g_last_yaw_deg;
  double dpitch = pitch_deg - g_last_pitch_deg;

  double lim_yaw = g_yaw_max_rate * dt;
  double lim_pitch = g_pitch_max_rate * dt;

  if (g_slew_enable) {
    if (dyaw > lim_yaw) dyaw = lim_yaw;
    else if (dyaw < -lim_yaw) dyaw = -lim_yaw;
    if (dpitch > lim_pitch) dpitch = lim_pitch;
    else if (dpitch < -lim_pitch) dpitch = -lim_pitch;
  }

  g_last_yaw_deg += dyaw;
  g_last_pitch_deg += dpitch;
  yaw_deg = g_last_yaw_deg;
  pitch_deg = g_last_pitch_deg;

  if (g_debug_send) {
    static auto last_log = std::chrono::steady_clock::now();
    bool active = (std::abs(yaw_raw - yaw_deg) > 0.3) || (std::abs(pitch_raw - pitch_deg) > 0.3);
    if (active && std::chrono::duration<double>(now - last_log).count() >= 0.5) {
      tools::logger()->info(
        "[Gimbal][shape] in yaw={:.2f} pitch={:.2f} -> out yaw={:.2f} pitch={:.2f}", yaw_raw,
        pitch_raw, yaw_deg, pitch_deg);
      last_log = now;
    }
  }
}
}  // namespace

Gimbal::Gimbal(const std::string & config_path)
{
  auto yaml = tools::load(config_path);
  auto com_port = tools::read<std::string>(yaml, "com_port");

  if (yaml["lpf_enable"]) g_lpf_enable = yaml["lpf_enable"].as<bool>();
  if (yaml["lpf_alpha"]) g_lpf_alpha = yaml["lpf_alpha"].as<double>();
  if (yaml["slew_enable"]) g_slew_enable = yaml["slew_enable"].as<bool>();
  if (yaml["max_yaw_rate"]) g_yaw_max_rate = yaml["max_yaw_rate"].as<double>();
  if (yaml["max_pitch_rate"]) g_pitch_max_rate = yaml["max_pitch_rate"].as<double>();
  if (yaml["three_order_enable"]) g_three_order = yaml["three_order_enable"].as<bool>();
  tools::logger()->info(
    "[Gimbal] lpf_enable={} alpha={:.2f} | slew_enable={} yaw_rate={:.0f} pitch_rate={:.0f}",
    g_lpf_enable, g_lpf_alpha, g_slew_enable, g_yaw_max_rate, g_pitch_max_rate);

  try {
    serial_.setPort(com_port);
    serial_.open();
  } catch (const std::exception & e) {
    tools::logger()->error("[Gimbal] Failed to open serial: {}", e.what());
    exit(1);
  }
  thread_ = std::thread(&Gimbal::read_thread, this);
  queue_.pop();
  tools::logger()->info("[Gimbal] First q received.");
}

Gimbal::~Gimbal()
{
  quit_ = true;
  if (thread_.joinable()) thread_.join();
  serial_.close();
}

GimbalMode Gimbal::mode() const
{
  std::lock_guard<std::mutex> lock(mutex_);
  return mode_;
}

GimbalState Gimbal::state() const
{
  std::lock_guard<std::mutex> lock(mutex_);
  return state_;
}

std::string Gimbal::str(GimbalMode mode) const
{
  switch (mode) {
    case GimbalMode::IDLE: return "IDLE";
    case GimbalMode::AUTO_AIM: return "AUTO_AIM";
    case GimbalMode::SMALL_BUFF: return "SMALL_BUFF";
    case GimbalMode::BIG_BUFF: return "BIG_BUFF";
    default: return "INVALID";
  }
}

Eigen::Quaterniond Gimbal::q(std::chrono::steady_clock::time_point t)
{
  while (true) {
    auto [q_a, t_a] = queue_.pop();
    auto [q_b, t_b] = queue_.front();
    auto t_ab = tools::delta_time(t_a, t_b);
    auto t_ac = tools::delta_time(t_a, t);
    auto k = t_ac / t_ab;
    Eigen::Quaterniond q_c = q_a.slerp(k, q_b).normalized();
    if (t < t_a) return q_c;
    if (!(t_a < t && t <= t_b)) continue;
    return q_c;
  }
}

void Gimbal::send(
  bool control, bool fire, float yaw, float yaw_vel, float yaw_acc, float pitch, float pitch_vel,
  float pitch_acc)
{
  tx_data_.head = 0xFF;
  tx_data_.fire = fire ? 1 : 0;
  tx_data_.distance = 0.0f;
  tx_data_.crc8 = 0;
  tx_data_.tail = 0x0D;

  if (control) {
    double cur_deg = 0.0;
    {
      std::lock_guard<std::mutex> lock(mutex_);
      cur_deg = static_cast<double>(state_.yaw) * RAD2DEG;
    }
    double raw_deg = static_cast<double>(yaw) * RAD2DEG;
    double send_deg = unwrap_angle(raw_deg, cur_deg);
    double pitch_deg = static_cast<double>(pitch) * RAD2DEG;

    // ① 低通滤波: 磨掉小幅高频噪声
    lowpass(send_deg, pitch_deg);
    // ② 指令限速: 磨平大跳变
    slew_limit(send_deg, pitch_deg);

    tx_data_.yaw = static_cast<float>(send_deg);
    tx_data_.pitch = static_cast<float>(pitch_deg);

    if (DEBUG_UNWRAP && std::abs(send_deg - raw_deg) > 1.0) {
      tools::logger()->info(
        "[Gimbal] 解缠 raw={:.1f} cur={:.1f} -> send={:.1f}", raw_deg, cur_deg, send_deg);
    }
  } else {
    tx_data_.yaw = 0.0f;
    tx_data_.pitch = 0.0f;
    slew_reset();
  }

  try {
    std::lock_guard<std::mutex> lock(write_mutex_);
    if (g_three_order) {
      // 三阶指令帧 (角度用度, 速度用度/秒)
      static VisionToGimbal3 tx3;
      tx3.head = 0xFF;
      tx3.fire = tx_data_.fire;
      tx3.yaw = tx_data_.yaw;
      tx3.yaw_vel = yaw_vel * RAD2DEG;
      tx3.yaw_acc = yaw_acc * RAD2DEG;
      tx3.pitch = tx_data_.pitch;
      tx3.pitch_vel = pitch_vel * RAD2DEG;
      tx3.pitch_acc = pitch_acc * RAD2DEG;
      tx3.distance = tx_data_.distance;
      tx3.crc8 = 0;
      tx3.tail = 0x0D;
      serial_.write(reinterpret_cast<uint8_t *>(&tx3), sizeof(tx3));
    } else {
      serial_.write(reinterpret_cast<uint8_t *>(&tx_data_), sizeof(tx_data_));
    }
  } catch (const std::exception & e) {
    tools::logger()->warn("[Gimbal] Failed to write serial: {}", e.what());
  }
}

void Gimbal::send(io::VisionToGimbal VisionToGimbal)
{
  tx_data_ = VisionToGimbal;
  tx_data_.head = 0xFF;
  tx_data_.crc8 = 0;
  tx_data_.tail = 0x0D;
  try {
    std::lock_guard<std::mutex> lock(write_mutex_);
    serial_.write(reinterpret_cast<uint8_t *>(&tx_data_), sizeof(tx_data_));
  } catch (const std::exception & e) {
    tools::logger()->warn("[Gimbal] Failed to write serial: {}", e.what());
  }
}

void Gimbal::send_raw(const std::uint8_t * data, std::size_t len)
{
  if (data == nullptr || len == 0) return;
  try {
    std::lock_guard<std::mutex> lock(write_mutex_);
    serial_.write(data, len);
  } catch (const std::exception & e) {
    // 视频帧发送失败不刷屏，只静默计数（交给上层决定）
  }
}

bool Gimbal::read(uint8_t * buffer, size_t size)
{
  try {
    size_t total_read = 0;
    auto start = std::chrono::steady_clock::now();
    while (total_read < size) {
      size_t n = serial_.read(buffer + total_read, size - total_read);
      total_read += n;
      if (n == 0) {
        auto now = std::chrono::steady_clock::now();
        if (std::chrono::duration_cast<std::chrono::milliseconds>(now - start).count() > 100) {
          return false;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
      }
    }
    return true;
  } catch (const std::exception & e) {
    tools::logger()->warn("[Gimbal] Failed to read serial: {}", e.what());
    return false;
  }
}

void Gimbal::read_thread()
{
  tools::logger()->info("[Gimbal] read_thread started.");
  int error_count = 0;
  while (!quit_) {
    if (error_count > 20) {
      error_count = 0;
      tools::logger()->warn("[Gimbal] Link lost, reconnecting...");
      reconnect();
      continue;
    }
    uint8_t header_byte = 0;
    if (!read(&header_byte, 1)) {
      error_count++;
      continue;
    }
    if (header_byte != 0xFF) {
      continue;
    }
    rx_data_.head = 0xFF;
    if (!read(reinterpret_cast<uint8_t *>(&rx_data_) + 1, sizeof(rx_data_) - 1)) {
      error_count++;
      continue;
    }
    if (rx_data_.tail != 0x0D) {
      error_count++;
      continue;
    }
    error_count = 0;
    float pitch_deg = rx_data_.pitch;
    float yaw_deg = rx_data_.yaw;
    double pitch_rad = pitch_deg * DEG2RAD;
    double yaw_rad = yaw_deg * DEG2RAD;
    Eigen::Quaterniond q =
      Eigen::AngleAxisd(yaw_rad, Eigen::Vector3d::UnitZ()) *
      Eigen::AngleAxisd(pitch_rad, Eigen::Vector3d::UnitY()) *
      Eigen::AngleAxisd(0.0, Eigen::Vector3d::UnitX());
    q.normalize();
    auto t = std::chrono::steady_clock::now();
    queue_.push({q, t});

    std::lock_guard<std::mutex> lock(mutex_);
    state_.yaw = static_cast<float>(yaw_rad);
    state_.pitch = static_cast<float>(pitch_rad);
    state_.yaw_vel = 0.0f;
    state_.pitch_vel = 0.0f;
    state_.bullet_speed = 27.0f;
    state_.bullet_count = 0;
    mode_ = GimbalMode::AUTO_AIM;
  }
  tools::logger()->info("[Gimbal] read_thread stopped.");
}

void Gimbal::reconnect()
{
  int max_retry_count = 10;
  for (int i = 0; i < max_retry_count && !quit_; ++i) {
    try {
      serial_.close();
      std::this_thread::sleep_for(std::chrono::milliseconds(500));
    } catch (...) {
    }
    try {
      serial_.open();
      queue_.clear();
      tools::logger()->info("[Gimbal] Reconnected serial successfully.");
      break;
    } catch (const std::exception & e) {
      tools::logger()->warn("[Gimbal] Reconnect failed: {}", e.what());
      std::this_thread::sleep_for(std::chrono::seconds(1));
    }
  }
}
}  // namespace io
