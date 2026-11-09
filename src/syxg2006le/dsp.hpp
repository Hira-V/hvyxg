// Copyright (C) 2026 Kevin H. Patterson
// SPDX-License-Identifier: BSD-3-Clause (see LICENSE)
#pragma once
// Small per-sample building blocks of the S-YXG2006LE DSP voice: sample interpolation, the
// state-variable filter and the attenuation laws.

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>

#include "attenuation.hpp"

namespace hvyxg::syxg2006le {

/// Phase fraction bits of the sample position (the DLL's 15-bit fractional phase).
inline constexpr int kPhaseBits = 15;
inline constexpr std::int64_t kPhaseOne = std::int64_t{1} << kPhaseBits;
/// Render block = control tick (64 samples).
inline constexpr int kBlock = 64;

/// Attenuation (1/16-octave units) → gain; ≥ 255 units mutes.
inline float units_gain(int units) {
    return units >= 255 ? 0.0f : attenuation_gain(static_cast<std::uint32_t>(std::max(units, 0)) << 12);
}

/// 4-point, 3rd-order Hermite (Catmull-Rom) interpolation at fraction t of [y1, y2].
inline float hermite(float y0, float y1, float y2, float y3, float t) {
    const float c1 = 0.5f * (y2 - y0);
    const float c2 = y0 - 2.5f * y1 + 2.0f * y2 - 0.5f * y3;
    const float c3 = 0.5f * (y3 - y0) + 1.5f * (y1 - y2);
    return ((c3 * t + c2) * t + c1) * t + y1;
}

/// 6-tap Lanczos (a = 3) kernel, tabulated at 1/256-sample steps.
class Lanczos3 {
public:
    static constexpr int kSteps = 256;
    Lanczos3() {
        for (int s = 0; s <= kSteps; ++s) {
            const double t = static_cast<double>(s) / kSteps;
            double sum = 0.0;
            std::array<double, 6> w{};
            for (int k = 0; k < 6; ++k) {
                const double x = t - (k - 2); // taps at offsets -2..3
                w[static_cast<std::size_t>(k)] = sinc(x) * sinc(x / 3.0);
                sum += w[static_cast<std::size_t>(k)];
            }
            for (int k = 0; k < 6; ++k)
                table_[static_cast<std::size_t>(s)][static_cast<std::size_t>(k)] =
                    static_cast<float>(w[static_cast<std::size_t>(k)] / sum);
        }
    }
    const std::array<float, 6>& weights(float t) const {
        return table_[static_cast<std::size_t>(std::lround(t * kSteps))];
    }
    /// The table, kSteps + 1 rows of 6 weights end to end (the resample kernel's).
    const float* data() const { return table_[0].data(); }

private:
    static double sinc(double x) {
        if (std::abs(x) < 1e-12) return 1.0;
        const double px = 3.14159265358979323846 * x;
        return std::sin(px) / px;
    }
    std::array<std::array<float, 6>, kSteps + 1> table_{};
};

/// Chamberlin state-variable filter of the DSP voice (scalar kernel): low-pass
/// output, one sample late; `q` is clamped per sample to keep the loop stable.
struct Svf {
    float lp = 0.0f;
    float bp = 0.0f;

    float process(float x, float f, float q) {
        const float qq = std::min(q, std::max(3.0f - 2.0f * f, 2.0f - f));
        const float out = lp;
        bp += f * (x - qq * bp - lp);
        lp += f * bp;
        return out;
    }
};

} // namespace hvyxg::syxg2006le
