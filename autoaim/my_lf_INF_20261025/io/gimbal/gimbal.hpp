#ifndef IO__GIMBAL_HPP
#define IO__GIMBAL_HPP

#include <Eigen/Geometry>
#include <array>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <mutex>
#include <string>
#include <thread>
#include <tuple>
#include <vector>
#include "serial/serial.h"
#include "tools/thread_safe_queue.hpp"

namespace io
{
// ===== 这辆车实际使用的自定义协议 =====
// 上行帧（下位机→上位机）16字节：
//   FF | 04 | roll(f32,恒0) | pitch(f32,rad) | yaw(f32,rad) | crc(0) | 0D
// 下行帧（上位机→下位机）16字节：
//   FF | fire(0/1) | pitch(f32,rad) | yaw(f32,rad) | distance(f32) | crc(0) | 0D
struct __attribute__((packed)) GimbalToVision
{
  uint8_t head = 0xFF;      // 帧头
  uint8_t mode = 0x04;      // 上行 mode(固定)
  float roll = 0.0f;        // roll，恒0
  float pitch = 0.0f;       // 云台 pitch 绝对角度 (rad)
  float yaw = 0.0f;         // 云台 yaw 绝对角度 (rad)
  uint8_t crc8 = 0;         // 校验(未启用)
  uint8_t tail = 0x0D;      // 帧尾
};
static_assert(sizeof(GimbalToVision) == 16);

struct __attribute__((packed)) VisionToGimbal
{
  uint8_t head = 0xFF;      // 帧头
  uint8_t fire = 0;         // 0=不发射 1=请求发射
  float pitch = 0.0f;       // 目标 pitch (rad)
  float yaw = 0.0f;         // 目标 yaw (rad)
  float distance = 0.0f;    // 目标距离
  uint8_t crc8 = 0;         // 校验(发0)
  uint8_t tail = 0x0D;      // 帧尾
};
static_assert(sizeof(VisionToGimbal) == 16);

// 三阶指令帧 (32字节) —— 与《下位机改动方案》一致; 电控改好后开 flag 启用
struct __attribute__((packed)) VisionToGimbal3
{
  uint8_t head = 0xFF;
  uint8_t fire = 0;
  float yaw = 0.0f;        // 度
  float yaw_vel = 0.0f;    // 度/秒
  float yaw_acc = 0.0f;    // 度/秒^2
  float pitch = 0.0f;      // 度
  float pitch_vel = 0.0f;  // 度/秒
  float pitch_acc = 0.0f;  // 度/秒^2
  float distance = 0.0f;
  uint8_t crc8 = 0;
  uint8_t tail = 0x0D;
};
static_assert(sizeof(VisionToGimbal3) == 32);

enum class GimbalMode
{
  IDLE,
  AUTO_AIM,
  SMALL_BUFF,
  BIG_BUFF
};

struct GimbalState
{
  float yaw;
  float yaw_vel;
  float pitch;
  float pitch_vel;
  float bullet_speed;
  uint16_t bullet_count;
};

class Gimbal
{
public:
  Gimbal(const std::string & config_path);
  ~Gimbal();
  GimbalMode mode() const;
  GimbalState state() const;
  std::string str(GimbalMode mode) const;
  Eigen::Quaterniond q(std::chrono::steady_clock::time_point t);
  void send(
    bool control, bool fire, float yaw, float yaw_vel, float yaw_acc, float pitch, float pitch_vel,
    float pitch_acc);
  void send(io::VisionToGimbal VisionToGimbal);

  // send_raw 直接把一段字节写入同一条串口（用于视频帧 0x0310，帧头 0xA5 与控制帧 0xFF 区分）
  // 线程安全：内部加锁，与 send() 互斥
  void send_raw(const std::uint8_t * data, std::size_t len);

  // try_send_raw：排队一帧视频并立刻推一次写队列。返回 false 只表示入队失败（异常）。
  // ★ 2026-10-24 修复（图传 0 帧的根因）：
  //   串口是 O_NONBLOCK 打开的、库默认写超时是 0，以前 try_send_raw 直接裸 write()
  //   还不看返回值 —— USB CDC 缓冲一满 write() 就只写出去半帧（实测 32% 的 309B 帧
  //   短写，最坏一个字节都没写），下位机的 309 状态机从此错位，把后续控制帧
  //   一起当视频吞掉。现在：非零写超时 + 循环补齐 + 未写完的字节跨轮续写。
  bool try_send_raw(const std::uint8_t * data, std::size_t len);

  // 串口写超时（毫秒）。串口以 O_NONBLOCK 打开，库默认写超时是 0：
  // 缓冲一满就立刻放弃、短写。单次 write() 内部等 kWriteWaitMs。
  std::uint32_t write_timeout_ms() const { return kWriteWaitMs; }

  // 串口发送统计（只给日志用，主循环每秒打一次）
  struct SerialStats
  {
    std::uint64_t ctrl_ok = 0;      // 控制帧完整写出的次数
    std::uint64_t ctrl_short = 0;   // 控制帧一轮没写完、下一轮从断点续写的轮数（不是错误）
    std::uint64_t ctrl_resync = 0;  // 上一条控制帧还没补完时又来新指令，新指令被丢弃的次数（应为 0）
    std::uint64_t video_ok = 0;     // 视频帧完整写出的次数
    std::uint64_t video_short = 0;  // 视频帧一次没写完、需要下一轮接着写的次数
    std::uint64_t video_stall = 0;  // 视频帧跨主循环续写的累计轮数
    std::uint64_t video_fail = 0;   // 真正失败的次数（端口掉线/抛异常/旧帧被新帧顶掉）
  };
  SerialStats serial_stats() const;

private:
  // 单次 write() 的内部等待上限（serial 库的 write_timeout_constant）。
  // 这个值就是「主循环最坏被串口拖住多久」——一次 write() 内部最多阻塞这么久。
  // 不能给 100ms：下位机 USB 侧一旦暂时不取数，80Hz 的自瞄会直接掉到 10Hz。
  static constexpr std::uint32_t kWriteWaitMs = 5u;
  // 控制帧补齐预算：32 字节在 USB FS 上不到 1 个令牌（<1ms），5ms 只为兜住偶发重传。
  static constexpr std::uint32_t kControlBudgetMs = 5u;
  // 视频帧一轮最多占用主循环的时间。309 字节健康时 <1ms；
  // 真堵的时候宁可跨轮分几次写，也不让主循环一口气卡 100ms（自瞄优先）。
  static constexpr std::uint32_t kVideoBudgetMs = 8u;

  serial::Serial serial_;
  mutable std::mutex write_mutex_;   // 保护 serial_.write（控制帧与视频帧共用同一串口）
  std::atomic<std::uint64_t> st_ctrl_ok_{0};
  std::atomic<std::uint64_t> st_ctrl_short_{0};
  std::atomic<std::uint64_t> st_ctrl_resync_{0};
  std::atomic<std::uint64_t> st_video_ok_{0};
  std::atomic<std::uint64_t> st_video_short_{0};
  std::atomic<std::uint64_t> st_video_stall_{0};
  std::atomic<std::uint64_t> st_video_fail_{0};

  // ---- 写队列（只在持有 write_mutex_ 时访问）----
  // 之所以要排队而不是"谁调谁直接 write"：USB CDC 缓冲满时 309 字节的一帧
  // 可能只写出去一半，剩下的必须在下一轮接着写完；这期间**绝不能**插入控制帧，
  // 否则下位机把控制帧的 32 字节当成视频帧的一部分吞掉，之后再无同步。
  // 控制帧同理：只写出去半条 32 字节指令时，剩下的必须紧接着补完，
  // 补不完就宁可这一轮什么都不再写（新指令下一轮主循环马上会再送一条）。
  std::array<std::uint8_t, 64> ctrl_buf_{};  // 当前控制帧（最长 32 字节）
  std::size_t ctrl_len_ = 0;                 // 0 = 没有待发控制帧
  std::size_t ctrl_off_ = 0;                 // 已经写出去的字节数
  std::vector<std::uint8_t> video_next_;         // 待发视频帧（只保留最新一帧）
  bool video_pending_ = false;
  std::vector<std::uint8_t> video_leftover_;     // 上一帧没写完的剩余字节
  std::size_t video_leftover_offset_ = 0;

  // 严格顺序：先写完上一帧的剩余字节 → 再发控制帧 → 最后发整帧视频。
  std::size_t write_some(const std::uint8_t * data, std::size_t len, std::uint32_t budget_ms);
  void pump_writes();
  bool queue_control(const std::uint8_t * data, std::size_t len);
  void apply_timeout();
  std::thread thread_;
  std::atomic<bool> quit_ = false;
  mutable std::mutex mutex_;
  GimbalToVision rx_data_;
  VisionToGimbal tx_data_;
  GimbalMode mode_ = GimbalMode::IDLE;
  GimbalState state_;
  tools::ThreadSafeQueue<std::tuple<Eigen::Quaterniond, std::chrono::steady_clock::time_point>>
    queue_{1000};
  bool read(uint8_t * buffer, size_t size);
  void read_thread();
  void reconnect();
};
}  // namespace io
#endif  // IO__GIMBAL_HPP
