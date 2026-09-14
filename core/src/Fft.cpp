#include "wm/core/Fft.h"

#include <cmath>
#include <stdexcept>
#include <utility>

namespace wm::core {
namespace {
constexpr double kPi = 3.14159265358979323846;
} // namespace

Fft::Fft(std::size_t size) : size_(size) {
    if (size < 2 || (size & (size - 1)) != 0) {
        throw std::invalid_argument("wm::core::Fft: size must be a power of two >= 2");
    }

    twiddle_.resize(size / 2);
    for (std::size_t i = 0; i < size / 2; ++i) {
        twiddle_[i] = std::polar(1.0, -2.0 * kPi * static_cast<double>(i) / static_cast<double>(size));
    }

    std::size_t bits = 0;
    while ((static_cast<std::size_t>(1) << bits) < size) {
        ++bits;
    }
    reverse_.resize(size);
    for (std::size_t i = 0; i < size; ++i) {
        std::size_t r = 0;
        for (std::size_t b = 0; b < bits; ++b) {
            if (i & (static_cast<std::size_t>(1) << b)) {
                r |= static_cast<std::size_t>(1) << (bits - 1 - b);
            }
        }
        reverse_[i] = r;
    }
}

void Fft::Transform(std::vector<std::complex<double>>& data) const {
    if (data.size() != size_) {
        throw std::invalid_argument("wm::core::Fft::Transform: wrong buffer size");
    }

    for (std::size_t i = 0; i < size_; ++i) {
        const std::size_t r = reverse_[i];
        if (r > i) {
            std::swap(data[i], data[r]);
        }
    }

    for (std::size_t len = 2; len <= size_; len <<= 1) {
        const std::size_t half = len >> 1;
        const std::size_t step = size_ / len;
        for (std::size_t base = 0; base < size_; base += len) {
            for (std::size_t j = 0; j < half; ++j) {
                const auto w = twiddle_[j * step];
                auto& a = data[base + j];
                auto& b = data[base + j + half];
                const auto t = b * w;
                b = a - t;
                a = a + t;
            }
        }
    }
}

std::vector<double> RealMagnitude(const Fft& fft, const std::vector<double>& real) {
    const std::size_t n = fft.size();
    std::vector<std::complex<double>> data(n);

    double windowSum = 0.0;
    for (std::size_t i = 0; i < n; ++i) {
        // Hann window; input shorter than n is zero-padded.
        const double w = 0.5 * (1.0 - std::cos(2.0 * kPi * static_cast<double>(i) / static_cast<double>(n)));
        windowSum += w;
        data[i] = std::complex<double>(i < real.size() ? real[i] * w : 0.0, 0.0);
    }

    fft.Transform(data);

    std::vector<double> mag(n / 2 + 1);
    const double scale = 2.0 / (windowSum > 0.0 ? windowSum : 1.0);
    for (std::size_t k = 0; k <= n / 2; ++k) {
        mag[k] = std::abs(data[k]) * scale;
    }
    return mag;
}

} // namespace wm::core
