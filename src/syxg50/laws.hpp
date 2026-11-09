// Copyright (C) 2026 Kevin H. Patterson
// SPDX-License-Identifier: BSD-3-Clause (see LICENSE)
#pragma once
// The S-YXG50 control layer's integer laws, as pure functions.

#include <algorithm>
#include <cstdint>
#include <cstdlib>

#include "syxg50/dll_tables.hpp"

namespace hvyxg::syxg50::law {

/// Floor division (the DLL's `>>` on signed values).
constexpr int floor_div(int a, int b) { return a >= 0 ? a / b : -((-a + b - 1) / b); }

/// Part velocity sense: depth/offset are XG `08 pp 0C/0D`.
constexpr int velocity_sense(int vel, int depth, int offset) {
    if (depth == 64 && offset == 64) return vel;
    return std::clamp(depth * vel / 64 + 2 * offset - 128, 1, 127);
}

/// Element velocity offset (byte 32) before the curve.
constexpr int velocity_offset(int vel, int b32) {
    return b32 ? std::min(127, vel + (((127 - vel) * b32 * 36) >> 8)) : vel;
}

/// Velocity attenuation `Tvel = [curve[v]]`, [i] = Tlev[i + 1]`.
constexpr int velocity_atten(const dll::Tables& tb, int curve, int vel) {
    const int c = tb.kVelocityCurves[static_cast<std::size_t>(std::clamp(curve, 0, 6) * 128 +
                                                                std::clamp(vel, 0, 127))];
    return tb.kLevelAtten[static_cast<std::size_t>(c + 1)];
}

/// Piecewise-linear key scaling between four break keys (`keys[0..3]`) with c64 offsets
/// (`offs[0..3]`), truncating division; returns `offset − 64`.
constexpr int key_scaling(const std::uint8_t* keys, const std::uint8_t* offs, int key) {
    if (key <= keys[0]) return offs[0] - 64;
    for (int i = 0; i < 3; ++i) {
        const int k0 = keys[i], k1 = keys[i + 1];
        if (key > k1) continue;
        const int a = offs[i], b = offs[i + 1];
        if (key == k1 || k1 == k0) return b - 64;
        int v = a + (key - k0) * (b - a) / (k1 - k0);
        v = b >= a ? std::min(v, 127) : std::max(v, 0);
        return v - 64;
    }
    return offs[3] - 64;
}

/// Key tracking: zone key and cents remainder for mode byte 15 / centre 16.
struct Tracked {
    int key;
    int cents;
};
constexpr Tracked key_track(const dll::Tables& tb, int key, int mode, int centre) {
    if (mode >= 5) return {centre, 0};
    if (mode == 0) return {key, 0};
    const int x = tb.kKeyTrack[static_cast<std::size_t>(mode)] * (key - centre);
    const int q = floor_div(x, 100);
    return {centre + q, x - 100 * q};
}

/// AEG rate key scaling: `trunc((key − centre)·(sens − 64)·16 / 256)`.
constexpr int aeg_key_scaling(int key, int centre, int sens) {
    return (key - centre) * ((sens - 64) * 16) / 256;
}

/// PEG / FEG rate key scaling: `(key − c)·(s − 64)·16 >> 8`.
constexpr int eg_key_scaling(int key, int centre, int sens) {
    return floor_div((key - centre) * (sens - 64) * 16, 256);
}

/// Part gain.
constexpr int part_gain(int volume, int expression, int master) {
    int g = (volume && master) ? std::max(1, ((volume + 1) * (master + 1)) >> 7) : 0;
    g = (g && expression) ? std::max(1, ((expression + 1) * g) >> 7) : 0;
    return g;
}

/// Level register 0x5c0: `2·min(127, Tlev[L] + zone + Tvel)`.
constexpr int level_register(const dll::Tables& tb, int level, int zone_atten, int tvel) {
    return 2 * std::min(127, tb.kLevelAtten[static_cast<std::size_t>(std::clamp(level, 0, 128))] +
                                 zone_atten + tvel);
}

/// Controller amplitude term `F(d, x) = sign(d − 64)·((3|d − 64|·x >> 8) & 0xff)`.
constexpr int amp_term(int depth, int x) {
    const int d = depth - 64;
    const int m = ((3 * std::abs(d) * x) >> 8) & 0xff;
    return d < 0 ? -m : m;
}

/// Controller pitch `(d − 64)·value·0xc947 >> 16` cents (0.786 cents per unit).
constexpr int ctrl_pitch(int depth, int value) {
    return ((depth - 64) * value * 0xc947) >> 16;
}

/// Pan nibbles: part pan 0..127 and element / drum pan 0..127.
struct PanAtten {
    int left;
    int right;
};
constexpr PanAtten pan_atten(const dll::Tables& tb, int part_pan, int pan) {
    const auto pp = static_cast<std::size_t>(std::clamp(part_pan, 0, 127));
    const auto p = static_cast<std::size_t>(std::clamp(pan, 0, 127));
    return {std::min(15, tb.kPartPan[pp] + tb.kElementPan[p]),
            std::min(15, tb.kPartPan[127 - pp] + tb.kElementPan[127 - p])};
}
/// The element / drum pan alone (for the variation insertion part: the part pan
/// is applied after the effect).
constexpr PanAtten element_pan_atten(const dll::Tables& tb, int pan) {
    const auto p = static_cast<std::size_t>(std::clamp(pan, 0, 127));
    return {std::min<int>(15, tb.kElementPan[p]), std::min<int>(15, tb.kElementPan[127 - p])};
}
/// Random pan: the part-pan law alone at a random position.
constexpr PanAtten random_pan(const dll::Tables& tb, int r) {
    const auto p = static_cast<std::size_t>(std::clamp(r, 0, 127));
    return {tb.kPartPan[p], tb.kPartPan[127 - p]};
}

/// 11-bit cutoff register from the control layer sum: `≤ 0x800 → 1`.
constexpr int cutoff_register(int c) {
    if (c <= 0x800) return 1;
    return std::min(c, 0xfff) & 0x7ff;
}

/// XG part resonance applied to an element resonance 0..63.
constexpr int part_resonance(int r, int part) {
    if (part < 64) return std::max(0, r + part - 64);
    return std::max(r, part - 64);
}

/// PEG level byte (c64) → ¼ cents for range byte 17.
constexpr int peg_level(int d, int vs, int range) {
    if (d == 0) return 0;
    const int a = std::abs(d) + (d > 0 ? 1 : 0);
    const int t = a - (((vs * a) >> 8) & 0xff);
    int m = t * 75;
    switch (range & 3) {
    case 0: m >>= 2; break;
    case 1: m >>= 1; break;
    case 2: break;
    default: m *= 2; break;
    }
    return d < 0 ? -m : m;
}

/// XG part PEG level (initial / release level) → ¼ cents: 9.375 cents per step.
constexpr int peg_part_level(int d, int vs) {
    if (d == 0) return 0;
    const int a = std::abs(d) + (d > 0 ? 1 : 0);
    const int t = a - (((vs * a) >> 8) & 0xff);
    const int m = (t * 75) >> 1;
    return d < 0 ? -m : m;
}

/// PEG / FEG level velocity sensitivity (without the curve).
constexpr int eg_level_vs(int sens, int vel) {
    const int s = sens - 64;
    if (s > 0) return ((128 - vel) * s * 36) >> 7;
    if (s < 0) return (vel * -s * 36) >> 7;
    return 0;
}

/// PEG / FEG rate velocity term: both signs make it faster.
constexpr int eg_rate_vs(int sens, int vel) {
    const int s = (sens - 64) * 16;
    if (s > 0) return (vel * s) >> 8;
    if (s < 0) return ((128 - vel) * -s) >> 8;
    return 0;
}

/// FEG level byte (c64) → ¼ cutoff units.
constexpr int feg_level(int level, int vs) {
    const int x = 2 * level - 128;
    return (x - ((x * vs) >> 8)) * 64;
}

/// Send level → attenuation register, 255 = off.
constexpr int send_atten(const dll::Tables& tb, int level) {
    return tb.kSendAtten[static_cast<std::size_t>(std::clamp(level, 0, 127))];
}

/// Part detune (XG `08 pp 09/0A` as one byte, centre 0x80) → cents.
constexpr int detune_cents(int detune) {
    const int e = (detune - 128) * 16;
    return e >= 0 ? (e * 0x63a) >> 16 : (e * 0x659) >> 16;
}

/// GS mode: part detune scaled by the key: a constant frequency offset rather
/// than a constant pitch offset.
constexpr int detune_cents_gs(const dll::Tables& tb, int detune, int key) {
    const int e = (detune - 128) * 16;
    const auto k = static_cast<std::size_t>(key & 0x7F);
    return e >= 0 ? (e * tb.kGsDetuneUp[k]) >> 16 : (e * static_cast<int>(tb.kGsDetuneDown[k])) >> 16;
}

/// Pitch bend → cents for XG bend range `08 pp 23` (c64).
constexpr int bend_cents(int range, int bend14) {
    int b = bend14 - 8192;
    if (b >= 0x1ffe) b = 0x2000;
    const auto r = static_cast<std::int16_t>((range - 64) * 800);
    return (r * b) >> 16;
}

} // namespace hvyxg::syxg50::law
