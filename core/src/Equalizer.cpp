#include "wm/core/Equalizer.h"

#include <algorithm>
#include <cmath>

namespace wm::core {
namespace {
constexpr double kPi = 3.14159265358979323846;
constexpr double kOctaveBandwidth = 1.0; // -3 dB points one octave apart

struct BiquadCoeffs {
    double b0 = 1.0, b1 = 0.0, b2 = 0.0, a1 = 0.0, a2 = 0.0;
};

double ClampGainDb(double v) {
    return std::clamp(v, Equalizer::MinGainDb, Equalizer::MaxGainDb);
}

// RBJ cookbook "peakingEQ": boost/cut around f0 without touching the endpoints.
// https://www.w3.org/2011/audio/audio-eq-cookbook
void DesignPeaking(BiquadCoeffs& c, double f0, double sampleRate, double gainDb) {
    const double a = std::pow(10.0, gainDb / 40.0);
    const double w0 = 2.0 * kPi * f0 / sampleRate;
    const double sinW0 = std::sin(w0);
    const double cosW0 = std::cos(w0);
    if (std::abs(sinW0) < 1e-12) { // f0 at/near Nyquist or DC: nothing to filter.
        c = {};
        return;
    }
    const double alpha = sinW0 * std::sinh(std::log(2.0) / 2.0 * kOctaveBandwidth * w0 / sinW0);
    const double a0 = 1.0 + alpha / a;
    c.b0 = (1.0 + alpha * a) / a0;
    c.b1 = -2.0 * cosW0 / a0;
    c.b2 = (1.0 - alpha * a) / a0;
    c.a1 = -2.0 * cosW0 / a0;
    c.a2 = (1.0 - alpha / a) / a0;
}
} // namespace

// Declared out-of-line so the array has exactly one definition.
const std::array<double, Equalizer::BandCount>& Equalizer::CenterFreqs() noexcept {
    static const std::array<double, BandCount> centers{ 31.5, 63.0, 125.0, 250.0, 500.0,
                                                        1000.0, 2000.0, 4000.0, 8000.0, 16000.0 };
    return centers;
}

void Equalizer::SetGains(const std::array<double, BandCount>& gainsDb, double preampDb) {
    std::lock_guard<std::mutex> lock(m_mutex);
    for (std::size_t i = 0; i < BandCount; ++i) {
        gainsDb_[i] = ClampGainDb(gainsDb[i]);
    }
    preampDb_ = std::clamp(preampDb, -PreampRangeDb, PreampRangeDb);
    Recompute();
}

void Equalizer::SetSampleRate(double sampleRate) {
    std::lock_guard<std::mutex> lock(m_mutex);
    if (sampleRate <= 0.0 || sampleRate_ == sampleRate) {
        return;
    }
    sampleRate_ = sampleRate;
    Recompute();
}

void Equalizer::Reset() noexcept {
    std::lock_guard<std::mutex> lock(m_mutex);
    for (auto& band : bands_) {
        band.Reset();
    }
}

std::array<double, Equalizer::BandCount> Equalizer::GainsDb() const {
    std::lock_guard<std::mutex> lock(m_mutex);
    return gainsDb_;
}

double Equalizer::PreampDb() const {
    std::lock_guard<std::mutex> lock(m_mutex);
    return preampDb_;
}

void Equalizer::Recompute() noexcept {
    preampLinear_ = std::pow(10.0, preampDb_ / 20.0);
    bool active = (preampLinear_ != 1.0);
    const bool rateKnown = sampleRate_ > 0.0;
    const double nyquist = sampleRate_ / 2.0;
    const auto& centers = CenterFreqs();
    for (std::size_t i = 0; i < BandCount; ++i) {
        double gain = gainsDb_[i];
        // A band above Nyquist (e.g. 48 kHz content with the 16 kHz band is
        // fine, but 8/16 kHz bands would be useless on a 22 kHz stream) is
        // forced to 0 dB instead of aliasing.
        if (!rateKnown || centers[i] >= nyquist) {
            gain = 0.0;
        }
        BiquadCoeffs coeffs;
        if (gain != 0.0) {
            DesignPeaking(coeffs, centers[i], sampleRate_, gain);
            active = true;
        }
        auto& band = bands_[i];
        band.b0 = coeffs.b0;
        band.b1 = coeffs.b1;
        band.b2 = coeffs.b2;
        band.a1 = coeffs.a1;
        band.a2 = coeffs.a2;
    }
    active_.store(active);
}

void Equalizer::Process(float* samples, std::size_t frameCount, int channels) {
    if (samples == nullptr || frameCount == 0 || channels <= 0) {
        return;
    }
    std::lock_guard<std::mutex> lock(m_mutex);
    if (!active_.load() || sampleRate_ <= 0.0) {
        return;
    }
    const int ch = channels > 2 ? 2 : channels; // >2 stays unfiltered per extra channel.

    for (std::size_t frame = 0; frame < frameCount; ++frame) {
        for (int c = 0; c < ch; ++c) {
            double x = static_cast<double>(samples[frame * channels + c]) * preampLinear_;
            for (auto& band : bands_) {
                const double y = band.b0 * x + band.b1 * band.x1[c] + band.b2 * band.x2[c]
                                 - band.a1 * band.y1[c] - band.a2 * band.y2[c];
                band.x2[c] = band.x1[c];
                band.x1[c] = x;
                band.y2[c] = band.y1[c];
                band.y1[c] = y;
                x = y;
            }
            samples[frame * channels + c] = static_cast<float>(std::clamp(x, -1.0, 1.0));
        }
    }
}

} // namespace wm::core
