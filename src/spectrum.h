#pragma once

#include <algorithm>
#include <array>
#include <cmath>
#include <complex>
#include <cstddef>
#include <cstdint>
#include <numbers>
#include <span>

namespace talktext {

// Single-threaded, allocation-free analysis of continuous mono signed PCM16 at 16 kHz.
// Include this header alone for synthetic-tone smoke checks; no Windows/ASR linkage.
class SpectrumAnalyzer {
public:
    static constexpr int sampleRate = 16000;
    static constexpr std::size_t windowSize = 1024;
    static constexpr std::size_t hopSize = 256;
    static constexpr std::array<int, 9> bandEdgesHz{80, 160, 320, 640, 1000, 1600, 2500, 4000, 8000};

    SpectrumAnalyzer() noexcept {
        for (std::size_t i = 0; i < windowSize; ++i) {
            const double angle = 2.0 * std::numbers::pi * static_cast<double>(i) / windowSize;
            window_[i] = static_cast<float>(0.5 - 0.5 * std::cos(angle));
            if (i < twiddles_.size()) {
                twiddles_[i] = {static_cast<float>(std::cos(angle)), static_cast<float>(-std::sin(angle))};
            }
        }
    }

    // Arbitrary block sizes retain sample history and a fixed 16 ms analysis cadence.
    // First result needs 1024 samples (64 ms); bands are zero until then.
    void push(std::span<const std::int16_t> samples) noexcept {
        for (const auto sample : samples) {
            history_[next_] = static_cast<float>(sample) / 32768.0f;
            next_ = (next_ + 1) & (windowSize - 1);
            if (--untilFrame_ == 0) {
                analyze();
                untilFrame_ = hopSize;
            }
        }
    }

    // Latest frame: bins assigned by center Hz to [low, high), with 8 kHz included.
    // One-sided power / (N * sum(Hann^2)); periodic Hann has sum(Hann^2) = 3N/8.
    // Each band = clamp(4 * sqrt(sum(power)), 0, 1), matching the RMS level gain.
    // A contained sine of peak A gives about 4*A/sqrt(2), not a relative band share.
    // No time smoothing/noise floor; zero input clears history within N+hop samples.
    const std::array<float, 8>& bands() const noexcept { return bands_; }

private:
    void analyze() noexcept {
        for (std::size_t i = 0; i < windowSize; ++i) {
            fft_[i] = {history_[(next_ + i) & (windowSize - 1)] * window_[i], 0.0f};
        }
        for (std::size_t i = 1, reversed = 0; i < windowSize; ++i) {
            std::size_t bit = windowSize >> 1;
            while (reversed & bit) {
                reversed ^= bit;
                bit >>= 1;
            }
            reversed ^= bit;
            if (i < reversed) std::swap(fft_[i], fft_[reversed]);
        }
        for (std::size_t length = 2; length <= windowSize; length <<= 1) {
            const std::size_t half = length / 2;
            const std::size_t stride = windowSize / length;
            for (std::size_t start = 0; start < windowSize; start += length) {
                for (std::size_t i = 0; i < half; ++i) {
                    const auto even = fft_[start + i];
                    const auto odd = fft_[start + i + half] * twiddles_[i * stride];
                    fft_[start + i] = even + odd;
                    fft_[start + i + half] = even - odd;
                }
            }
        }

        constexpr float powerScale = 8.0f / (3.0f * windowSize * windowSize);
        bands_.fill(0.0f);
        std::size_t band = 0;
        for (std::size_t bin = 1; bin <= windowSize / 2; ++bin) {
            const auto scaledHz = bin * sampleRate;
            if (scaledHz < static_cast<std::size_t>(bandEdgesHz.front()) * windowSize) continue;
            while (band + 1 < bands_.size() &&
                   scaledHz >= static_cast<std::size_t>(bandEdgesHz[band + 1]) * windowSize) {
                ++band;
            }
            const float sidedness = bin == windowSize / 2 ? 1.0f : 2.0f;
            bands_[band] += std::norm(fft_[bin]) * sidedness * powerScale;
        }
        for (auto& bandLevel : bands_) bandLevel = std::clamp(4.0f * std::sqrt(bandLevel), 0.0f, 1.0f);
    }

    std::array<float, windowSize> history_{};
    std::array<float, windowSize> window_{};
    std::array<std::complex<float>, windowSize> fft_{};
    std::array<std::complex<float>, windowSize / 2> twiddles_{};
    std::array<float, 8> bands_{};
    std::size_t next_ = 0;
    std::size_t untilFrame_ = windowSize;
};

}  // namespace talktext
