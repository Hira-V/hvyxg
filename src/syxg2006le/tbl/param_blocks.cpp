// Copyright (C) 2026 Kevin H. Patterson
// SPDX-License-Identifier: BSD-3-Clause (see LICENSE)
#include "syxg2006le/tbl/param_blocks.hpp"

#include "syxg2006le/tbl/dll_tables.hpp"

#include <algorithm>
#include <cmath>

namespace hvyxg::syxg2006le::tbl {

namespace {

std::uint16_t u16(std::span<const std::uint8_t> b, std::size_t at) {
    return static_cast<std::uint16_t>(b[at] | (b[at + 1] << 8));
}
std::int16_t i16(std::span<const std::uint8_t> b, std::size_t at) {
    return static_cast<std::int16_t>(u16(b, at));
}
std::int8_t sext(std::uint8_t v, int bits) {
    const int m = (1 << bits) - 1, x = v & m;
    return static_cast<std::int8_t>(x >= (1 << (bits - 1)) ? x - (1 << bits) : x);
}

} // namespace

LfoBlock LfoBlock::from_bytes(std::span<const std::uint8_t, 16> b) {
    LfoBlock a;
    a.wave = b[0] & 0x0F;
    a.key_sync = b[0] & 0x40;
    a.speed = b[1];
    a.phase = b[2];
    a.amp_depth = b[3];
    a.pitch_depth = i16(b, 4);
    a.filter_depth = u16(b, 6);
    a.delay = u16(b, 8) & 0x7FF;
    a.fade_flags = b[9] >> 5;
    a.fade_in = u16(b, 10) & 0x7FF;
    a.unknown12 = u16(b, 12);
    a.source_gates = b[14] & 7;
    a.invert = b[15] >> 5;
    return a;
}

PitchEgBlock PitchEgBlock::from_bytes(std::span<const std::uint8_t, 28> b) {
    PitchEgBlock p;
    p.rate_key_center = b[0] & 0x7F;
    p.depth = static_cast<std::int8_t>(b[1]);
    p.delay = u16(b, 2) & 0x7FF;
    p.rate = {b[4], b[5], b[6]};
    p.release_rate = b[7] & 0x7F;
    p.initial_level = i16(b, 8);
    p.level = {i16(b, 10), i16(b, 12), i16(b, 14)};
    p.release_level = i16(b, 16);
    p.depth_velocity_curve = u16(b, 18);
    p.depth_curve_scale = b[22] & 0x0F;
    p.rate_key_sens = sext(b[23], 6);
    p.rate_velocity_sens = sext(b[24], 6);
    p.coarse = sext(b[26], 7);
    p.fine = sext(b[27], 7);
    return p;
}

AmpEgBlock AmpEgBlock::from_bytes(std::span<const std::uint8_t, 36> b) {
    AmpEgBlock c;
    c.attack_rate = b[1];
    c.decay1_rate = b[2];
    c.decay2_rate = b[3];
    c.decay3_rate = b[4];
    c.release_rate = b[5];
    c.damper_release_rate = b[6];
    c.level_0 = b[7];
    c.level_1 = b[8];
    c.level_2 = b[9];
    c.level_3 = b[10];
    c.level_key_curve = u16(b, 12);
    c.rate_key_curve = u16(b, 14);
    c.level_velocity_curve = u16(b, 16);
    c.attack_velocity_curve = u16(b, 18);
    c.decay1_velocity_curve = u16(b, 20);
    c.rate_velocity_curve = u16(b, 22);
    c.soft_key_curve = u16(b, 24);
    c.soft_velocity_curve = u16(b, 26);
    c.held_time_curve = u16(b, 28);
    c.scales = {b[30], b[31], b[32], b[33], b[34]};
    return c;
}

FilterEntry FilterEntry::from_bytes(std::span<const std::uint8_t, 20> b) {
    FilterEntry f;
    f.type = b[0] & 0x0F;
    f.flags = b[0] >> 6;
    f.resonance = b[1];
    f.cutoff = u16(b, 2);
    f.byte4 = b[4];
    f.gain_a = b[5] & 7;
    f.gain_b = u16(b, 6) & 0x1FF;
    f.cutoff_key_curve = u16(b, 10);
    f.cutoff_velocity_curve = u16(b, 12);
    f.cutoff_soft_curve = u16(b, 14);
    f.resonance_velocity_curve = u16(b, 16);
    f.scale_a = b[18];
    f.scale_b = b[19];
    return f;
}

FilterBlock FilterBlock::from_bytes(std::span<const std::uint8_t, 84> b) {
    FilterBlock d;
    d.depth = static_cast<std::int8_t>(b[1]);
    d.delay = u16(b, 2) & 0x7FF;
    d.rate = {b[4], b[5], b[6]};
    d.release_rate = b[7] & 0x7F;
    d.damper_release_rate = b[8] & 0x7F;
    d.level = {i16(b, 10), i16(b, 12), i16(b, 14), i16(b, 16)};
    d.release_level = i16(b, 18);
    d.damper_release_level = i16(b, 20);
    d.rate_key_curve = u16(b, 22);
    d.depth_velocity_curve = u16(b, 24);
    d.delay_velocity_curve = u16(b, 26);
    d.rate1_velocity_curve = u16(b, 28);
    d.rate_velocity_curve = u16(b, 30);
    d.scales = {b[32], b[33], b[34]};
    d.cutoff_key_curve = u16(b, 38);
    d.filter = {FilterEntry::from_bytes(b.subspan<44, 20>()), FilterEntry::from_bytes(b.subspan<64, 20>())};
    return d;
}

namespace {

constexpr double kTwo26 = 67108864.0;

} // namespace

int eg_increment(const dll::Tables& tb, int rate, double fs) {
    const int r = std::clamp(rate, 0, 127);
    return static_cast<int>(44100.0 / fs * kTwo26 / tb.kDecayTime[static_cast<std::size_t>(r)]);
}

double attenuation_gain(int units) {
    const std::uint32_t x = static_cast<std::uint32_t>(std::clamp(units, 0, 255)) << 12;
    return static_cast<double>(0x10000u | (~x & 0xFFFFu)) / (131071.0 * std::ldexp(1.0, static_cast<int>(x >> 16)));
}

std::uint8_t pan_attenuation(const dll::Tables& tb, int pan) { return tb.kPan[static_cast<std::size_t>(std::clamp(pan, 0, 128))]; }

int lfo_speed_value(const dll::Tables& tb, int index) { return tb.kLfoSpeed[static_cast<std::size_t>(std::clamp(index, 2, 93))]; }

int lfo_increment(int v, double fs) {
    // v → 6-bit mantissa code (e = 0 below 535, e = 1 up to 1072; the speed is
    // clamped to 6..1016 before it gets here).
    const auto mant = [](std::int64_t shifted) {
        return static_cast<int>(((shifted + 0x21A548) * 0x79BD4BAB) >> 53);
    };
    double x = 0.0;
    if (v > 0 && v <= 0x216) {
        x = std::clamp(mant(std::int64_t{v} << 19), 1, 63) / 128.0;
    } else if (v > 0x216) {
        x = 0.5 + std::clamp(mant(std::int64_t{std::min(v, 0x430)} << 19) - 64, 0, 63) / 128.0;
    }
    return static_cast<int>(x * 10.7666015625 / fs * kTwo26);
}

double lfo_hz(int v, double fs) { return lfo_increment(v, fs) * fs / (64.0 * 1048576.0); }

int filter_log_cutoff(int cents, double fs) {
    const float ratio = static_cast<float>(44100.0 / static_cast<float>(fs));
    const int base = static_cast<int>(65536.0 * std::log2(static_cast<double>(ratio)));
    return std::clamp(cents * 4096 / 75 + base, 0x80000, 0x100000);
}

double filter_coefficient(int log_cutoff) {
    const int e = log_cutoff >> 16;
    const int m = (log_cutoff >> 8) & 0xFF;
    return std::min(1.0, (256 + m) / std::ldexp(1.0, 24 - e));
}

double filter_damping(int resonance) {
    const int r = std::clamp(resonance, 16, 140);
    return (64 - (r & 31)) / std::ldexp(1.0, (r >> 5) + 5);
}

} // namespace hvyxg::syxg2006le::tbl
