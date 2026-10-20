#ifndef IO__GIMBAL_HPP
#define IO__GIMBAL_HPP

#include <Eigen/Geometry>
#include <atomic>
#include <chrono>
#include <mutex>
#include <string>
#include <thread>
#include <tuple>
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

  // try_send_raw 同 send_raw，但串口写失败/阻塞抛异常时返回 false（不静默吞掉），
  // 方便上层在主循环里做视频包丢弃预算，保证自瞄控制不被图传拖慢。
  bool try_send_raw(const std::uint8_t * data, std::size_t len);

private:
  serial::Serial serial_;
  std::mutex write_mutex_;   // 保护 serial_.write（控制帧与视频帧共用同一串口）
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
