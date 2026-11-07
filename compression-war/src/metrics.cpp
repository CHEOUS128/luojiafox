#include "metrics.h"

#include <cmath>
#include <cstddef>
#include <vector>

namespace cw {

double psnr_y(const std::uint8_t * a, const std::uint8_t * b, int width, int height) {
    if (a == nullptr || b == nullptr || width <= 0 || height <= 0) return -1.0;
    const std::size_t n = static_cast<std::size_t>(width) * static_cast<std::size_t>(height);
    double sse = 0.0;
    for (std::size_t i = 0; i < n; ++i) {
        const double d = static_cast<double>(a[i]) - static_cast<double>(b[i]);
        sse += d * d;
    }
    if (sse <= 0.0) return 99.0;   // 完全一致，给一个有限的大数
    const double mse = sse / static_cast<double>(n);
    return 10.0 * std::log10((255.0 * 255.0) / mse);
}

double ssim_y(const std::uint8_t * a, const std::uint8_t * b, int width, int height) {
    if (a == nullptr || b == nullptr || width < 8 || height < 8) return -1.0;
    constexpr double kL = 255.0;
    constexpr double kC1 = (0.01 * kL) * (0.01 * kL);
    constexpr double kC2 = (0.03 * kL) * (0.03 * kL);

    double sum = 0.0;
    std::size_t blocks = 0;
    for (int by = 0; by + 8 <= height; by += 8) {
        for (int bx = 0; bx + 8 <= width; bx += 8) {
            double ma = 0, mb = 0;
            for (int y = 0; y < 8; ++y) {
                const std::uint8_t * ra = a + static_cast<std::size_t>(by + y) * width + bx;
                const std::uint8_t * rb = b + static_cast<std::size_t>(by + y) * width + bx;
                for (int x = 0; x < 8; ++x) {
                    ma += ra[x];
                    mb += rb[x];
                }
            }
            ma /= 64.0;
            mb /= 64.0;
            double va = 0, vb = 0, cov = 0;
            for (int y = 0; y < 8; ++y) {
                const std::uint8_t * ra = a + static_cast<std::size_t>(by + y) * width + bx;
                const std::uint8_t * rb = b + static_cast<std::size_t>(by + y) * width + bx;
                for (int x = 0; x < 8; ++x) {
                    const double da = static_cast<double>(ra[x]) - ma;
                    const double db = static_cast<double>(rb[x]) - mb;
                    va += da * da;
                    vb += db * db;
                    cov += da * db;
                }
            }
            va /= 63.0;
            vb /= 63.0;
            cov /= 63.0;
            const double num = (2.0 * ma * mb + kC1) * (2.0 * cov + kC2);
            const double den = (ma * ma + mb * mb + kC1) * (va + vb + kC2);
            if (den > 0.0) {
                sum += num / den;
                ++blocks;
            }
        }
    }
    return blocks > 0 ? sum / static_cast<double>(blocks) : -1.0;
}

void estimate_tone(const std::uint8_t * a, const std::uint8_t * b, int width, int height,
                   double & k, double & c) {
    k = 1.0;
    c = 0.0;
    if (a == nullptr || b == nullptr || width <= 0 || height <= 0) return;
    const std::size_t n = static_cast<std::size_t>(width) * static_cast<std::size_t>(height);
    double sa = 0, sb = 0, saa = 0, sab = 0;
    for (std::size_t i = 0; i < n; ++i) {
        const double x = a[i], y = b[i];
        sa += x; sb += y; saa += x * x; sab += x * y;
    }
    const double dn = static_cast<double>(n);
    const double den = dn * saa - sa * sa;
    if (std::abs(den) < 1e-9) return;
    k = (dn * sab - sa * sb) / den;
    c = (sb - k * sa) / dn;
    if (!(k > 0.2 && k < 5.0)) { k = 1.0; c = 0.0; }
}

double ssim_y_tone_free(const std::uint8_t * a, const std::uint8_t * b, int width, int height) {
    if (a == nullptr || b == nullptr || width <= 0 || height <= 0) return -1.0;
    const std::size_t n = static_cast<std::size_t>(width) * static_cast<std::size_t>(height);

    double k = 1.0, c = 0.0;
    estimate_tone(a, b, width, height, k, c);

    std::vector<std::uint8_t> fixed(n);
    for (std::size_t i = 0; i < n; ++i) {
        const double v = (static_cast<double>(b[i]) - c) / k;
        fixed[i] = static_cast<std::uint8_t>(v < 0.0 ? 0.0 : (v > 255.0 ? 255.0 : v));
    }
    return ssim_y(a, fixed.data(), width, height);
}

void rgba_to_y(const std::uint8_t * rgba, std::uint8_t * y, int width, int height) {    if (rgba == nullptr || y == nullptr) return;
    const std::size_t n = static_cast<std::size_t>(width) * static_cast<std::size_t>(height);
    for (std::size_t i = 0; i < n; ++i) {
        const std::uint8_t * p = rgba + i * 4;
        const int v = static_cast<int>(std::lround(0.299 * p[0] + 0.587 * p[1] + 0.114 * p[2]));
        y[i] = static_cast<std::uint8_t>(v < 0 ? 0 : (v > 255 ? 255 : v));
    }
}

}  // namespace cw
