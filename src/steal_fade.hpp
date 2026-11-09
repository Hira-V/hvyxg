// Copyright (C) 2026 Kevin H. Patterson
// SPDX-License-Identifier: BSD-3-Clause (see LICENSE)
#pragma once
// Superset: a stolen voice's sound fades out linearly over a chosen time instead of the DLLs'
// fast steal release (Synth::Options::steal_fade_ms).

#include <cstddef>
#include <cstdint>

namespace hvyxg {

class StealFade {
public:
    /// Start a fade of `samples` samples (0 = none).
    void start(std::uint32_t samples) {
        len_ = samples;
        pos_ = 0;
    }
    bool active() const { return len_ != 0; }
    /// Scale the block `x[0..n)`; true once the fade has reached silence.
    bool apply(float* x, std::size_t n) {
        if (len_ == 0) return false;
        const float inv = 1.0f / static_cast<float>(len_);
        for (std::size_t i = 0; i < n; ++i) {
            const std::uint32_t p = pos_ + static_cast<std::uint32_t>(i);
            x[i] *= p >= len_ ? 0.0f : 1.0f - static_cast<float>(p) * inv;
        }
        pos_ += static_cast<std::uint32_t>(n);
        return pos_ >= len_;
    }

private:
    std::uint32_t len_ = 0;
    std::uint32_t pos_ = 0;
};

/// Fade length in samples for `ms` at `sample_rate` (0 = off).
inline std::uint32_t steal_fade_samples(float ms, float sample_rate) {
    return ms > 0.0f ? static_cast<std::uint32_t>(ms * sample_rate / 1000.0f + 0.5f) : 0u;
}

} // namespace hvyxg
