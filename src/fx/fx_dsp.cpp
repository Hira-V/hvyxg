// Copyright (C) 2026 Kevin H. Patterson
// SPDX-License-Identifier: BSD-3-Clause (see LICENSE)
#include "fx/fx_dsp.hpp"

#include <algorithm>
#include <cmath>

#include "fx/fx_tables.hpp"

namespace hvyxg::fx {

namespace {

constexpr float kRefRate = 44100.0f;
constexpr std::size_t kReverbRing = 131072;
constexpr std::size_t kChorusRing = 32768;

std::size_t ring_size(std::size_t base, float rate) {
    std::size_t n = base;
    while (static_cast<float>(n) < static_cast<float>(base) * rate / kRefRate) n <<= 1;
    return n;
}

std::int32_t scale_addr(std::int16_t a, float rate) {
    if (rate == kRefRate) return a;
    return static_cast<std::int32_t>(std::lround(static_cast<double>(a) * rate / kRefRate));
}

// Triangle of the 23-bit phase: phase XOR (bit 22 ? 0x7FFFFF : 0), 0 .. 2^22 − 1.
std::uint32_t tri(std::uint32_t ph) { return ph ^ ((ph & 0x400000u) ? 0x7FFFFFu : 0u); }

constexpr float kFrac = 1.0f / 4194304.0f;  // 2^-22

} // namespace

float decode_word(std::uint16_t word, std::uint8_t format) {
    const unsigned w = word & 0x3FFu;
    if (format == 2) {
        const unsigned e = w >> 7;
        const auto m = static_cast<float>(w & 0x7Fu);
        if (e == 7) return m * (1.0f / 128.0f);
        return static_cast<float>(1u << e) * (1.0f / 4194304.0f) * m;
    }
    const int s = w > 0x200 ? static_cast<int>(w) - 0x400 : static_cast<int>(w);
    float v = static_cast<float>(s) * (1.0f / 512.0f);
    if (format == 1) v *= 4.0f;
    return v;
}

// --- reverb --------------------------------------------------------------------------------

ReverbUnit::ReverbUnit(float sample_rate)
    : rate_(sample_rate), ring_(ring_size(kReverbRing, sample_rate)), mask_(ring_.size() - 1) {}

void ReverbUnit::load(const EffectControl& ctrl) {
    for (std::size_t i = 0; i < EffectControl::kWords; ++i)
        coef_[i] = ctrl.tables().kCoefFormat[i] == 0xFF ? 0.0f : decode_word(ctrl.dsp_word(i), ctrl.tables().kCoefFormat[i]);
    for (std::size_t i = 0; i < EffectControl::kAddrs; ++i) addr_[i] = scale_addr(ctrl.dsp_addr(i), rate_);
}

void ReverbUnit::clear() {
    std::fill(ring_.begin(), ring_.end(), 0.0f);
    pos_ = 0;
    x1_ = y1_ = y2_ = 0;
    comb_prev_.fill(0);
}

void ReverbUnit::process(float* io_l, float* out_r, std::size_t frames) {
    // Input HPF then LPF (regs 45..50).
    for (std::size_t n = 0; n < frames; ++n) {
        const float x = io_l[n];
        const float y1 = c(45) * x1_ + c(46) * x + c(47) * y1_;
        const float y2 = c(48) * y1_ + c(50) * y2_ + c(49) * y1;
        x1_ = x;
        y1_ = y1;
        y2_ = y2;
        io_l[n] = y2;
    }
    float* b = ring_.data();
    const std::size_t m = mask_;
    for (std::size_t n = 0; n < frames; ++n) {
        const std::size_t p = pos_;
        const auto at = [&](std::size_t i) -> float& { return b[(d(i) + p) & m]; };
        const float u8 = at(8);
        const float u11 = at(11);
        at(17) = u8 * c(51) + io_l[n];                    // pre-delay line + feedback
        at(16) = u8 * c(42) + u11 * c(41);                // all-pass 1
        const float u13 = at(13);
        float f = u8 * c(44) + u11 * c(43);
        at(21) = f * c(57) + u13 * c(56);                 // all-pass 2
        f = f * c(59) + u13 * c(58);
        float u = at(12);                                  // four damped combs
        at(31) = c(89) * comb_prev_[0] + f * c(91) + u * c(90);
        comb_prev_[0] = u;
        u = at(28);
        at(33) = c(92) * comb_prev_[1] + f * c(94) + u * c(93);
        comb_prev_[1] = u;
        u = at(30);
        at(34) = c(98) * comb_prev_[2] + f * c(100) + u * c(99);
        comb_prev_[2] = u;
        u = at(32);
        at(35) = c(101) * comb_prev_[3] + f * c(103) + u * c(102);
        comb_prev_[3] = u;
        const float er = at(63);
        const float el = er * c(27) + (at(3) + at(2) + at(1) + at(0)) * c(28);
        const float hl = at(18);
        at(24) = el * c(70) + hl * c(69);                 // output all-pass L
        io_l[n] = el * c(72) + hl * c(71);
        const float e_r = er * c(32) + (at(7) + at(6) + at(5) + at(4)) * c(33);
        const float hr = at(23);
        at(29) = e_r * c(82) + hr * c(81);                // output all-pass R
        out_r[n] = e_r * c(84) + hr * c(83);
        pos_ = (pos_ - 1) & m;
    }
}

// --- chorus --------------------------------------------------------------------------------

ChorusUnit::ChorusUnit(float sample_rate)
    : rate_(sample_rate), ring_(ring_size(kChorusRing, sample_rate)), mask_(ring_.size() - 1) {}

void ChorusUnit::load(const EffectControl& ctrl) {
    for (std::size_t i = 0; i < EffectControl::kWords; ++i)
        coef_[i] = ctrl.tables().kCoefFormat[i] == 0xFF ? 0.0f : decode_word(ctrl.dsp_word(i), ctrl.tables().kCoefFormat[i]);
    for (std::size_t i = 0; i < EffectControl::kAddrs; ++i) addr_[i] = scale_addr(ctrl.dsp_addr(i), rate_);
    celeste_ = is_celeste(ctrl.chorus_algorithm());
    const double r = static_cast<double>(rate_) / kRefRate;
    // LFO increment per 4 samples, f = c9 · 22050 Hz.
    inc_ = static_cast<std::uint32_t>(std::llround(coef_[9] * 16777216.0 / r)) & 0x7FFFFFu;
    const auto depth = [&](std::size_t reg) {
        return static_cast<std::int64_t>(std::llround(coef_[reg] * 2048.0 * r));
    };
    depth_l_ = depth(26);
    depth_r_ = depth(40);
    depth_3_ = depth(55);
    off_r_ = static_cast<std::uint32_t>(std::lround(coef_[37] * 4194304.0));
    off_3_ = static_cast<std::uint32_t>(std::lround(coef_[52] * 4194304.0));
}

void ChorusUnit::clear() {
    std::fill(ring_.begin(), ring_.end(), 0.0f);
    pos_ = 0;
    phase_ = 0;
    lfo_count_ = 0;
    mod_l_ = mod_r_ = mod_3_ = 0;
    eq_.fill(0);
}

void ChorusUnit::process(float* l, float* r, std::size_t frames) {
    const auto& k = coef_;
    // Input EQ in place: low shelf then high shelf per channel.
    for (std::size_t n = 0; n < frames; ++n) {
        float x = l[n];
        float y1 = k[73] * eq_[0] + k[74] * x + k[75] * eq_[1];
        float y2 = y1 * k[77] + k[76] * eq_[1] + k[78] * eq_[2];
        eq_[0] = x;
        eq_[1] = y1;
        eq_[2] = y2;
        l[n] = y2;
        x = r[n];
        y1 = k[61] * x + k[60] * eq_[3] + k[62] * eq_[4];
        y2 = y1 * k[64] + k[63] * eq_[4] + k[65] * eq_[5];
        eq_[3] = x;
        eq_[4] = y1;
        eq_[5] = y2;
        r[n] = y2;
    }
    float* b = ring_.data();
    const std::size_t m = mask_;
    const auto tap = [&](std::int64_t base, std::int64_t mod) {
        const auto i = static_cast<std::size_t>(base + (mod >> 22)) & m;
        const float frac = static_cast<float>(static_cast<std::uint32_t>(mod) & 0x3FFFFFu) * kFrac;
        const float x0 = b[i];
        return (b[(i + 1) & m] - x0) * frac + x0;
    };
    for (std::size_t n = 0; n < frames; ++n) {
        if ((lfo_count_++ & 3) == 0) {
            phase_ = (phase_ + inc_) & 0x7FFFFFu;
            mod_l_ = static_cast<std::int64_t>(tri(phase_)) * depth_l_;
            mod_r_ = static_cast<std::int64_t>(tri((phase_ - off_r_) & 0x7FFFFFu)) * depth_r_;
            if (celeste_)
                mod_3_ = static_cast<std::int64_t>(tri((phase_ - off_3_) & 0x7FFFFFu)) * depth_3_;
        }
        const auto p = static_cast<std::int64_t>(pos_);
        const float vl = tap(addr_[9] + p, mod_l_);
        const float vr = tap(addr_[14] + p, mod_r_);
        float out_l = vl * k[96];
        float out_r = vr * k[105];
        if (celeste_) {
            out_l += tap(addr_[19] + p, mod_3_) * k[97];
            out_r += tap(addr_[25] + p, mod_3_) * k[106];
        }
        const float in_l = l[n];
        const float in_r = r[n];
        // L line first, R line second: in mono mode (A22 = 0, A27 = 1) the next sample's L
        // write lands on this R write, so both taps read the L + R line.
        b[static_cast<std::size_t>(addr_[27] + p) & m] = k[80] * in_r + vl * k[79] + in_l;
        b[static_cast<std::size_t>(addr_[22] + p) & m] = vr * k[66] + in_r;
        l[n] = out_l;
        r[n] = out_r;
        pos_ = (pos_ - 1) & m;
    }
}

} // namespace hvyxg::fx
