#pragma once

#include <complex>
#include <cstddef>
#include <vector>

namespace wm::core {

/// Iterative in-place radix-2 Cooley-Tukey FFT.
/// Size is fixed at construction and must be a power of two.
class Fft {
public:
    explicit Fft(std::size_t size);

    std::size_t size() const noexcept { return size_; }

    /// Forward transform, in-place. |data| must have exactly size() elements.
    void Transform(std::vector<std::complex<double>>& data) const;

private:
    std::size_t size_ = 0;
    std::vector<std::complex<double>> twiddle_;
    std::vector<std::size_t> reverse_;
};

/// Magnitude spectrum of a real signal (bins 0 .. fft.size()/2).
/// Input is zero-padded or truncated to fft.size(); Hann window is applied.
/// Output magnitude is normalized by (windowSum / 2) so a full-scale sinusoid
/// lands near 1.0.
std::vector<double> RealMagnitude(const Fft& fft, const std::vector<double>& real);

} // namespace wm::core
