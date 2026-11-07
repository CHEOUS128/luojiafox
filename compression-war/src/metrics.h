// metrics.h —— 画质指标
//
// 两个口径同时给：
//   PSNR-Y / SSIM-Y：只比亮度平面。压片战争里 H.264 4:0:0（灰度）和 4:2:0（彩色）
//                    都能用这一项对比，不受色度采样方式影响。
//
// ⚠ SSIM 对「能不能认出远处装甲板」不敏感 —— 灰度实验里实测只值 +0.0012，
//   但人眼看着差不少。所以报表里必须同时给逐帧字节分布和人工判读的截图。
#pragma once

#include <cstdint>
#include <vector>

namespace cw {

// 两个 Y 平面逐像素比较。尺寸必须一致，否则返回 -1。
double psnr_y(const std::uint8_t * a, const std::uint8_t * b, int width, int height);

// 8×8 分块 SSIM（标准 C1=(0.01·L)² / C2=(0.03·L)²，L=255），逐块平均。
double ssim_y(const std::uint8_t * a, const std::uint8_t * b, int width, int height);

// 先估一条仿射色调曲线 b ≈ k*a + c（最小二乘），把 b 拉回去再算 SSIM。
//
// 为什么要这个数：实车链路有个**跟编码无关**的整体色偏——
//   VideoSender 用 sws 把 BGR24 转 GRAY8，输出是 0..255 满量程；
//   但码流不带 range 标记（color_range=unknown），客户端解码时 sws 又按
//   limited range 展开成 1.164*(x-16)，于是 <16 全被削成纯黑、整体压暗。
// 直接算 SSIM 会把这份「色调损失」和「压缩损失」混在一起，分不清谁该背锅。
//   ssim_y            = 客户端真实看到的画质（含色调损失）
//   ssim_y_tone_free  = 只衡量压缩本身
double ssim_y_tone_free(const std::uint8_t * a, const std::uint8_t * b, int width, int height);

// 单独估一次色调曲线（b ≈ k*a + c），给报表用：实车链路现在是 k≈1.16、c≈-16
void estimate_tone(const std::uint8_t * a, const std::uint8_t * b, int width, int height,
                   double & k, double & c);

// RGBA → Y（BT.601）。客户端解码器输出的是 RGBA。
void rgba_to_y(const std::uint8_t * rgba, std::uint8_t * y, int width, int height);

}  // namespace cw
