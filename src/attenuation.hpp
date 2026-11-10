// Copyright (C) 2026 Kevin H. Patterson
// SPDX-License-Identifier: BSD-3-Clause (see LICENSE)
#pragma once
// The attenuation → gain law both DLLs use: `x` is an attenuation with 0x10000 per octave (6.02 dB), so one
// 1/16-octave level unit is 0x1000. Within each octave the gain is linear in `x`, which
// is up to +0.51 dB above the exact exponential halfway between octaves; both engines'
// measured levels follow this curve.

#include <bit>
#include <cstdint>

namespace hvyxg {

/// Gain for attenuation `x` (0x10000 per octave); 1.0 at 0. Values ≥ 32 octaves are silent.
inline float attenuation_gain(std::uint32_t x) {
    const std::uint32_t octave = x >> 16;
    if (octave >= 32) return 0.0f;
    const std::uint32_t mant = (~x & 0xFFFFu) | 0x10000u;
    // mant / (131071 · 2^octave), rounded as a float division, without the division (17 cycles,
    // unpipelined, on the A25): the 2^-octave commutes with the rounding (the results stay normal),
    // and mant / 131071 is a double product rounded to float, which is the correctly rounded quotient:
    // p / 131071 (2^16 <= p < 2^17, 131071 odd) lies at least 2^-42 (relative) from any float rounding
    // midpoint, and the product is within 2^-51.
    const auto q = static_cast<float>(static_cast<double>(mant) * (1.0 / 131071.0));
    return q * std::bit_cast<float>((127u - octave) << 23);   // · 2^-octave
}

/// Gain for an attenuation in 1/16-octave level units (0.376 dB each).
inline float level_gain(int units) {
    return units <= 0 ? 1.0f : attenuation_gain(static_cast<std::uint32_t>(units) << 12);
}

} // namespace hvyxg
