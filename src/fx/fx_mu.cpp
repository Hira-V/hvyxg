// Copyright (C) 2026 Kevin H. Patterson
// SPDX-License-Identifier: BSD-3-Clause (see LICENSE)
#include "fx/fx_mu.hpp"

#include <algorithm>

namespace hvyxg::fx {

namespace {

constexpr std::uint8_t kAny = 0xFF;

struct TypeMap {
    std::uint8_t msb, lsb, to_msb, to_lsb;
};

// First match wins; kAny matches every LSB.
constexpr TypeMap kReverbMap[] = {{0x12, kAny, 0x11, 0x00}};
constexpr TypeMap kChorusMap[] = {
    {0x44, kAny, 0x42, 0x00}, {0x48, kAny, 0x43, 0x00}, {0x57, kAny, 0x41, 0x00}};
constexpr TypeMap kVariationMap[] = {
    {0x4E, 0x01, 0x49, 0x00}, {0x4E, 0x02, 0x4A, 0x00}, {0x52, 0x01, 0x49, 0x00},
    {0x52, 0x02, 0x4A, 0x00}, {0x52, kAny, 0x4E, 0x00}, {0x56, kAny, 0x45, 0x00},
    {0x57, kAny, 0x44, 0x00}, {0x5D, kAny, 0x4E, 0x00}, {0x5F, 0x00, 0x49, 0x00},
    {0x5F, kAny, 0x4A, 0x00}, {0x60, 0x00, 0x49, 0x00}, {0x60, kAny, 0x4A, 0x00},
    {0x61, 0x00, 0x49, 0x00}, {0x61, kAny, 0x4A, 0x00}};

struct Factor {
    std::uint8_t msb, lsb, factor;
};

// Hall +2.5 dB, Room 1 +3.5, Room 2 / 3 +2.0, Stage 1 +1.0, Stage 2 +2.5, Plate +1.5, White
// Room +3.5, Basement −1.0; the variation also Delay LCR +1.0, Delay LR +2.5, Echo +2.0,
// Cross Delay ×2, ER 2 +2.5, Gate / Reverse Gate +2.0, Karaoke 1 / 2 −1.5 / −2.5,
// Symphonic ×1.56, Rotary ×1.45, Tremolo / Auto Pan ×2.
constexpr Factor kReverbFactors[] = {
    {0x01, kAny, 0x55}, {0x02, 0x00, 0x60}, {0x02, kAny, 0x51}, {0x03, 0x00, 0x48},
    {0x03, kAny, 0x55}, {0x04, kAny, 0x4C}, {0x10, kAny, 0x60}, {0x13, kAny, 0x39}};
constexpr Factor kVariationFactors[] = {
    {0x01, kAny, 0x55}, {0x02, 0x00, 0x60}, {0x02, kAny, 0x51}, {0x03, 0x00, 0x48},
    {0x03, kAny, 0x55}, {0x04, kAny, 0x4C}, {0x10, kAny, 0x60}, {0x13, kAny, 0x39},
    {0x05, kAny, 0x48}, {0x06, kAny, 0x55}, {0x07, kAny, 0x51}, {0x08, kAny, 0x80},
    {0x09, 0x01, 0x55}, {0x0A, kAny, 0x51}, {0x0B, kAny, 0x51}, {0x14, 0x00, 0x36},
    {0x14, 0x01, 0x30}, {0x44, kAny, 0x64}, {0x45, kAny, 0x5D}, {0x46, kAny, 0x80},
    {0x47, kAny, 0x80}};

template <class T, std::size_t N>
const T* find(const T (&table)[N], std::uint8_t msb, std::uint8_t lsb) {
    for (const T& e : table)
        if (e.msb == msb && (e.lsb == kAny || e.lsb == lsb)) return &e;
    return nullptr;
}

} // namespace

std::pair<std::uint8_t, std::uint8_t> mu_effect_type(Block block, std::uint8_t msb, std::uint8_t lsb) {
    const TypeMap* m = nullptr;
    switch (block) {
    case Block::Reverb: m = find(kReverbMap, msb, lsb); break;
    case Block::Chorus: m = find(kChorusMap, msb, lsb); break;
    case Block::Variation: m = find(kVariationMap, msb, lsb); break;
    default: break;
    }
    return m ? std::pair{m->to_msb, m->to_lsb} : std::pair{msb, lsb};
}

std::uint8_t mu_return_level(Block block, std::uint8_t msb, std::uint8_t lsb, std::uint8_t level) {
    const Factor* f = block == Block::Reverb      ? find(kReverbFactors, msb, lsb)
                      : block == Block::Variation ? find(kVariationFactors, msb, lsb)
                                                  : nullptr;
    if (!f) return level;
    return static_cast<std::uint8_t>(std::min<unsigned>((level * f->factor + 32u) >> 6, 127u));
}

} // namespace hvyxg::fx
