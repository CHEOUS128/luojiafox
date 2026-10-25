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

// 串口写超时必须显式设置。serial 库默认 Timeout 全是 0，而 open() 用的是
// O_NONBLOCK：写 309 字节时只要 USB CDC 缓冲暂时满，SerialImpl::write() 里的
// pselect 立刻返回 0 → break → 只写出去一部分就返回。调用方若不看返回值，
// 下位机就收到半帧，状态机错位后把后面的控制帧当视频吞掉（图传 0 帧的根因）。
void Gimbal::apply_timeout()
{
  // 单次 write() 内部最多等 kWriteWaitMs；整体预算由 write_some() 的截止时间控制，
  // 所以 multiplier 必须是 0（否则 5 + 1×309 = 314ms，一次 write 就把主循环拖死）。
  serial_.setTimeout(0u, 100u, 0u, kWriteWaitMs, 0u);  // inter, read_c, read_m, write_c, write_m
}

Gimbal::SerialStats Gimbal::serial_stats() const
{
  SerialStats s;
  s.ctrl_ok = st_ctrl_ok_.load();
  s.ctrl_short = st_ctrl_short_.load();
  s.ctrl_resync = st_ctrl_resync_.load();
  s.video_ok = st_video_ok_.load();
  s.video_short = st_video_short_.load();
  s.video_stall = st_video_stall_.load();
  s.video_fail = st_video_fail_.load();
  return s;
}

// 尽量把 len 字节写完，返回实际写出去的字节数。
// 调用方必须已经持有 write_mutex_（Serial 库内部有锁，这里是自己的业务锁）。
//
// 为什么要循环：serial 库的 write() 在 O_NONBLOCK 下只要缓冲瞬间满就返回，
// 可能只写出一部分甚至一个字节都没写出去（实测 32% 的 309B 帧会短写，含 0 字节）。
std::size_t Gimbal::write_some(
  const std::uint8_t * data, std::size_t len, std::uint32_t budget_ms)
{
  const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(budget_ms);
  std::size_t done = 0;
  while (done < len) {
    done += serial_.write(data + done, len - done);
    if (done >= len) return done;
    // 一个字节都没写出去（缓冲满/端口卡死）时空转会烧满一个核，歇 1ms 再来
    std::this_thread::sleep_for(std::chrono::milliseconds(1));
    if (std::chrono::steady_clock::now() >= deadline) return done;
  }
  return done;
}

// 把队列里的东西按固定优先级推出去。调用方必须已经持有 write_mutex_。
//
// 顺序（这条顺序就是正确性本身，别随意调）：
//   1) 上一轮没写完的视频帧剩余字节 —— 必须最先写完，
//      否则下一轮的控制帧会插进视频帧中间，下位机会把它当视频吞掉。
//   2) 控制帧 —— 自瞄的命脉。32 字节要么整条写出去，要么一个字节都先不写。
//      注意：控制帧「短写」(只出去半条) 是**不可恢复**的——下位机收到半条会字段错位、
//      把后面的视频帧当成控制帧的剩余字节吞掉，而 32 字节控制帧没有长度字段可以丢掉重找。
//      所以控制帧的策略是「要么全写、要么不写」：先试写，5ms 内写不完就保留进度，
//      下一轮接着从断点补完（中间绝不插入视频帧）。
//   3) 整帧视频 —— 只保留最新一帧，旧的直接丢（实时性优先）。
void Gimbal::pump_writes()
{
  // ---- 1) 续写上一轮没写完的视频帧 ----
  if (video_leftover_offset_ < video_leftover_.size()) {
    const std::size_t remain = video_leftover_.size() - video_leftover_offset_;
    const std::size_t n = write_some(
      video_leftover_.data() + video_leftover_offset_, remain, kVideoBudgetMs);
    video_leftover_offset_ += n;
    if (video_leftover_offset_ < video_leftover_.size()) {
      ++st_video_stall_;
      return;  // 还是没写完：本轮到此为止，控制帧留到下一轮（绝不插进视频帧中间）
    }
    ++st_video_ok_;
    video_leftover_.clear();
    video_leftover_offset_ = 0;
  }

  // ---- 2) 控制帧：从断点续写，写不完就保持排队、本轮不再写别的 ----
  if (ctrl_len_ > 0) {
    ctrl_off_ += write_some(
      ctrl_buf_.data() + ctrl_off_, ctrl_len_ - ctrl_off_, kControlBudgetMs);
    if (ctrl_off_ < ctrl_len_) {
      ++st_ctrl_short_;  // 本轮没写完，下一轮从断点续写
      return;            // 绝不把视频帧插进控制帧中间
    }
    ++st_ctrl_ok_;
    ctrl_len_ = 0;
    ctrl_off_ = 0;
  }

  // ---- 3) 视频帧 ----
  if (video_pending_) {
    video_pending_ = false;
    const std::size_t n = write_some(video_next_.data(), video_next_.size(), kVideoBudgetMs);
    if (n == video_next_.size()) {
      ++st_video_ok_;
    } else {
      ++st_video_short_;
      video_leftover_.assign(video_next_.begin() + static_cast<std::ptrdiff_t>(n),
                             video_next_.end());
      video_leftover_offset_ = 0;
    }
    video_next_.clear();
  }
}

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
    apply_timeout();  // 必须：串口是 O_NONBLOCK 打开的，不设超时就会短写
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

  if (g_three_order) {
    // 三阶指令帧 (角度用度, 速度用度/秒)
    VisionToGimbal3 tx3;
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
    queue_control(reinterpret_cast<const uint8_t *>(&tx3), sizeof(tx3));
  } else {
    queue_control(reinterpret_cast<const uint8_t *>(&tx_data_), sizeof(tx_data_));
  }
}

void Gimbal::send(io::VisionToGimbal VisionToGimbal)
{
  tx_data_ = VisionToGimbal;
  tx_data_.head = 0xFF;
  tx_data_.crc8 = 0;
  tx_data_.tail = 0x0D;
  queue_control(reinterpret_cast<const uint8_t *>(&tx_data_), sizeof(tx_data_));
}

void Gimbal::send_raw(const std::uint8_t * data, std::size_t len)
{
  try_send_raw(data, len);
}

// 排队一条控制帧并立刻推一次写队列。
// 返回值恒为 true：控制帧不会丢，写不完会从断点续写，
// 所以「写失败」不再需要惊动自瞄主循环。
bool Gimbal::queue_control(const std::uint8_t * data, std::size_t len)
{
  if (data == nullptr || len == 0 || len > ctrl_buf_.size()) return false;
  try {
    std::lock_guard<std::mutex> lock(write_mutex_);
    // 上一轮的控制帧还卡在半路（没写完）：先接着把它推完，否则线上会交错。
    // 正常不会发生（主循环 6ms、单条控制帧 32 字节 <1ms）；
    // 真发生了且本轮推不完，就丢掉这条新指令——下一轮主循环马上再送一条最新的。
    if (ctrl_len_ > 0) {
      pump_writes();
      if (ctrl_len_ > 0) {
        ++st_ctrl_resync_;
        return false;
      }
    }
    std::copy(data, data + len, ctrl_buf_.begin());
    ctrl_len_ = len;
    ctrl_off_ = 0;
    pump_writes();
    return true;
  } catch (const std::exception & e) {
    tools::logger()->warn("[Gimbal] Failed to write serial: {}", e.what());
    return false;
  }
}

// 排队一帧视频（只保留最新一帧：实时性优先，旧帧直接丢）并立刻推一次写队列。
// 返回值只表示「这一帧已经接管」；半帧会留到下一轮接着写完，绝不丢在线上。
bool Gimbal::try_send_raw(const std::uint8_t * data, std::size_t len)
{
  if (data == nullptr || len == 0) return false;
  try {
    std::lock_guard<std::mutex> lock(write_mutex_);
    // 上一帧还没在线上写完：这一帧直接丢（实时性优先），但继续推写队列。
    // 不能在这里再塞一帧进 video_next_ —— 否则 309 字节写不完时会一帧接一帧地
    // 占住写通道，控制帧永远排不上队（自瞄直接废掉）。同一时刻只允许一帧在途。
    if (video_leftover_offset_ < video_leftover_.size()) {
      ++st_video_fail_;
      pump_writes();
      return false;
    }
    if (video_pending_) ++st_video_fail_;  // 上一帧还没来得及发就被这一帧顶掉
    video_next_.assign(data, data + len);
    video_pending_ = true;
    pump_writes();
    return true;
  } catch (const std::exception & e) {
    // 视频帧发送失败不刷屏；返回 false 让上层丢包
    ++st_video_fail_;
    return false;
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
      apply_timeout();  // 重连后超时设置不会自动保留
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
