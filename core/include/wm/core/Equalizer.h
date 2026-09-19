#pragma once

#include <array>
#include <atomic>
#include <cstddef>
#include <mutex>

namespace wm::core {

/// A 10-band parametric equaliser built from cascaded RBJ "peaking" biquads.
/// Works on interleaved float PCM in [-1, 1]; gains are stored in dB so the
/// UI can round-trip them without re-quantisation.
///
/// All methods are safe to call from the UI thread while an audio thread is
/// inside Process (coefficient updates are serialised by a mutex).
class Equalizer {
public:
    static constexpr std::size_t BandCount = 10;
    static constexpr double MinGainDb = -12.0;
    static constexpr double MaxGainDb = 12.0;
    static constexpr double PreampRangeDb = 12.0;

    // ISO 266 十段频点，31.5 Hz 至 16 kHz。
    static const std::array<double, BandCount>& CenterFreqs() noexcept;

    Equalizer() = default;

    /// (Re)computes the filter coefficients for the given per-band gains (dB)
    /// and preamp (dB). Gains are clamped to [MinGainDb, MaxGainDb].
    /// Safe before SetSampleRate: the maths happens as soon as a rate exists.
    void SetGains(const std::array<double, BandCount>& gainsDb, double preampDb = 0.0);

    /// Sample rate of the stream that will be fed to Process. Recalculates the
    /// coefficients; call before the first Process.
    void SetSampleRate(double sampleRate);

    /// Clears the filter memory (seek / track change) so stale samples from
    /// the previous position do not smear into the new one.
    void Reset() noexcept;

    /// Filters |frameCount| interleaved frames in place.
    void Process(float* samples, std::size_t frameCount, int channels);

    /// True when any coefficient deviates from unity (callers may then bypass
    /// Process entirely).
    bool IsActive() const noexcept { return active_.load(); }

    std::array<double, BandCount> GainsDb() const;
    double PreampDb() const;

private:
    struct Biquad {
        double b0 = 1.0, b1 = 0.0, b2 = 0.0, a1 = 0.0, a2 = 0.0;
        // Per-channel transposed direct-form II state.
        double x1[2] = {}, x2[2] = {}, y1[2] = {}, y2[2] = {};

        void Reset() noexcept { x1[0] = x1[1] = x2[0] = x2[1] = y1[0] = y1[1] = y2[0] = y2[1] = 0.0; }
    };

    void Recompute() noexcept; // callers hold m_mutex

    mutable std::mutex m_mutex;
    std::array<double, BandCount> gainsDb_{};
    double preampDb_ = 0.0;
    double sampleRate_ = 0.0;
    std::array<Biquad, BandCount> bands_{};
    double preampLinear_ = 1.0;
    std::atomic<bool> active_{ false };
};

} // namespace wm::core
