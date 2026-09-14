#pragma once

#include "Fft.h"

#include <cstddef>
#include <cstdint>
#include <vector>

namespace wm::core {

struct SpectrumConfig {
    std::size_t fftSize = 2048;
    std::size_t barCount = 32;
    std::uint32_t sampleRate = 48000;

    // Frequency range covered by the bars (logarithmically distributed).
    double minFreq = 32.0;
    double maxFreq = 16000.0;

    // dB window mapped onto [0, 1].
    double minDb = -72.0;
    double maxDb = -8.0;

    // Smoothing: how much of the new value to take when rising / falling.
    double attack = 0.55;
    double release = 0.12;

    double gain = 1.0;
};

/// Turns raw PCM frames into a small number of smoothed, log-spaced bars.
class SpectrumAnalyzer {
public:
    explicit SpectrumAnalyzer(SpectrumConfig cfg = {});

    void Configure(SpectrumConfig cfg);
    const SpectrumConfig& Config() const noexcept { return cfg_; }

    std::size_t BarCount() const noexcept { return cfg_.barCount; }

    /// Feed interleaved samples. |channels| == 1 means mono.
    /// Returns bars in [0, 1]; size == BarCount().
    std::vector<double> Process(const float* samples, std::size_t frameCount, int channels = 2);

    /// Decay bars towards zero (used when playback is paused / no audio).
    void Silence();

private:
    void Rebuild();

    SpectrumConfig cfg_{};
    Fft fft_;
    std::vector<double> window_;
    std::vector<double> real_;
    std::vector<std::complex<double>> complex_;
    std::vector<double> bars_;
    std::vector<std::pair<double, double>> bandEdges_; // (binLo, binHi) fractional
};

} // namespace wm::core
