#include "wm/core/SpectrumAnalyzer.h"

#include <algorithm>
#include <cmath>
#include <stdexcept>

namespace wm::core {
namespace {
constexpr double kEpsilon = 1e-9;

double Clamp01(double v) {
    return v < 0.0 ? 0.0 : (v > 1.0 ? 1.0 : v);
}
} // namespace

SpectrumAnalyzer::SpectrumAnalyzer(SpectrumConfig cfg) : fft_(cfg.fftSize) {
    Configure(cfg);
}

void SpectrumAnalyzer::Configure(SpectrumConfig cfg) {
    if (cfg.barCount == 0) {
        cfg.barCount = 1;
    }
    if (cfg.maxFreq <= cfg.minFreq) {
        cfg.maxFreq = cfg.minFreq * 2.0;
    }
    if (cfg.maxDb <= cfg.minDb) {
        cfg.maxDb = cfg.minDb + 1.0;
    }
    cfg_ = cfg;
    Rebuild();
}

void SpectrumAnalyzer::Rebuild() {
    if (fft_.size() != cfg_.fftSize) {
        fft_ = Fft(cfg_.fftSize);
    }
    if (fft_.size() < 2) {
        throw std::invalid_argument("wm::core::SpectrumAnalyzer: fftSize too small");
    }

    real_.assign(cfg_.fftSize, 0.0);
    bars_.assign(cfg_.barCount, 0.0);

    // Logarithmically spaced bands, in (fractional) bin coordinates.
    const double binsPerHz = static_cast<double>(cfg_.fftSize) / static_cast<double>(cfg_.sampleRate);
    const double maxBin = static_cast<double>(cfg_.fftSize) / 2.0;
    const double ratio = cfg_.maxFreq / cfg_.minFreq;

    bandEdges_.resize(cfg_.barCount);
    for (std::size_t i = 0; i < cfg_.barCount; ++i) {
        const double f0 = cfg_.minFreq * std::pow(ratio, static_cast<double>(i) / static_cast<double>(cfg_.barCount));
        const double f1 = cfg_.minFreq * std::pow(ratio, static_cast<double>(i + 1) / static_cast<double>(cfg_.barCount));
        double b0 = f0 * binsPerHz;
        double b1 = f1 * binsPerHz;
        if (b1 <= b0) {
            b1 = b0 + 1.0;
        }
        b0 = std::min(b0, maxBin);
        b1 = std::min(std::max(b1, b0 + 1.0), maxBin);
        bandEdges_[i] = {b0, b1};
    }
}

std::vector<double> SpectrumAnalyzer::Process(const float* samples, std::size_t frameCount, int channels) {
    if (samples == nullptr || frameCount == 0 || channels <= 0) {
        Silence();
        return bars_;
    }

    const std::size_t n = cfg_.fftSize;
    const std::size_t frames = std::min(frameCount, n);
    const double invChannels = 1.0 / static_cast<double>(channels);

    real_.assign(n, 0.0);
    for (std::size_t i = 0; i < frames; ++i) {
        double sum = 0.0;
        for (int c = 0; c < channels; ++c) {
            sum += static_cast<double>(samples[i * static_cast<std::size_t>(channels) + static_cast<std::size_t>(c)]);
        }
        real_[i] = sum * invChannels;
    }

    const auto mag = RealMagnitude(fft_, real_);

    for (std::size_t i = 0; i < cfg_.barCount; ++i) {
        const auto [b0, b1] = bandEdges_[i];
        const std::size_t lo = static_cast<std::size_t>(std::floor(b0));
        const std::size_t hi = static_cast<std::size_t>(std::ceil(b1));

        double peak = 0.0;
        for (std::size_t k = lo; k <= hi && k < mag.size(); ++k) {
            peak = std::max(peak, mag[k]);
        }

        const double db = 20.0 * std::log10(peak + kEpsilon);
        const double normalized = (db - cfg_.minDb) / (cfg_.maxDb - cfg_.minDb);
        const double target = Clamp01(normalized * cfg_.gain);

        const double smoothing = target > bars_[i] ? cfg_.attack : cfg_.release;
        bars_[i] += (target - bars_[i]) * smoothing;
    }

    return bars_;
}

void SpectrumAnalyzer::Silence() {
    for (double& b : bars_) {
        b += (0.0 - b) * cfg_.release;
        if (b < 0.0005) {
            b = 0.0;
        }
    }
}

} // namespace wm::core
