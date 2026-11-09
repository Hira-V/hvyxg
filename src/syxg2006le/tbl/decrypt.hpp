// Copyright (C) 2026 Kevin H. Patterson
// SPDX-License-Identifier: BSD-3-Clause (see LICENSE)
#pragma once
// S-YXG2006LE wave table (SXGDAT6L.TBL) decryption and sample decoding.
//
// The format, as the S-YXG2006LE DLL reads it:
// - The loader loads SXGBNW6L.TBL (voice table, stored in the clear) and SXGDAT6L.TBL,
//   and passes only the wave table through the descrambler.
// - The descrambler runs with key 0x5B (the same descrambler as the S-YXG50 wave
//   table: x ^ key ^ i, nibble swap, key inverted every byte), then for every byte pair
//   (2k, 2k+1): swap the two bytes and XOR both with 0x5A. A trailing odd byte is left
//   as the descrambler produced it.
// - The engine keeps that 0x5A mask in memory and removes it whenever it reads a sample, so the
//   plain sample bytes are descramble(0x5B) + pair swap. `decrypt` produces those.
//
// Sample formats (which one a sample uses comes from its sample header in the voice table):
// - 16-bit signed little-endian.
// - 12-bit packed, 2 samples per 3 bytes: see `unpack12`.
// - 8-bit signed linear.
// - 8-bit companded DPCM: s = kCompand8[b] + (k1·s₁ >> 10) + (k2·s₂ >> 10),
//   clamped to per-voice limits, with per-voice predictor coefficients k1/k2.

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
#include <utility>

#include "bank_io.hpp"

namespace hvyxg::syxg2006le::tbl {

inline constexpr std::uint8_t kInitKey = 0x5B;

/// Decrypt an S-YXG2006LE wave table in place into plain sample bytes.
inline void decrypt(std::span<std::uint8_t> data) {
    hvyxg::descramble_wave(kInitKey, data);
    for (std::size_t i = 0; i + 1 < data.size(); i += 2) std::swap(data[i], data[i + 1]);
}

/// 12-bit packed samples: sample pair n lives in bytes [3n, 3n+3).
///   even: int16((p1 & 0x0F) << 12 | p0 << 4)    odd: int16(p2 << 8 | (p1 & 0xF0))
/// (left-justified in 16 bits, as the engine returns them).
inline std::int16_t unpack12(std::span<const std::uint8_t> data, std::size_t index) {
    const std::size_t p = (index >> 1) * 3;
    const auto v = (index & 1)
                       ? static_cast<std::uint16_t>((data[p + 2] << 8) | (data[p + 1] & 0xF0))
                       : static_cast<std::uint16_t>(((data[p + 1] & 0x0F) << 12) | (data[p] << 4));
    return static_cast<std::int16_t>(v);
}

/// 8-bit companding table of the DPCM format: sign-magnitude,
/// 4 linear segments of 32 codes with steps 1, 2, 4, 8 → magnitudes 0..472.
inline constexpr std::array<std::int32_t, 256> kCompand8 = [] {
    std::array<std::int32_t, 256> t{};
    constexpr std::array<std::int32_t, 4> base = {0, 32, 96, 224};
    for (int c = 0; c < 128; ++c) {
        const int seg = c >> 5;
        t[static_cast<std::size_t>(c)] = base[static_cast<std::size_t>(seg)] + ((c & 31) << seg);
        t[static_cast<std::size_t>(c + 128)] = -t[static_cast<std::size_t>(c)];
    }
    return t;
}();

} // namespace hvyxg::syxg2006le::tbl
