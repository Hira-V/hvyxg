// Copyright (C) 2026 Kevin H. Patterson
// SPDX-License-Identifier: BSD-3-Clause (see LICENSE)
// The variation control shared by syxg50.dll and S-YXG2006LE.DLL: type map, defaults, the
// common helpers (filter designer, delay damping, connection) and the delay family / 2-band EQ
// handlers. The other families live in fx_control_var_*.cpp. Reimplements the behaviour of the 2006LE
// controller (block 2 and the per-type functions it dispatches to); syxg50.dll
// has the same logic over the same tables.
//
// Handlers address registers by index (`wr`, `ar`) and lists by their offset in the lists'
// block (`var_list`).

#include <algorithm>
#include <cmath>

#include "fx/fx_control.hpp"
#include "fx/fx_tables.hpp"

namespace hvyxg::fx {

namespace {

constexpr std::uint8_t kParamInit = 0xFF;

// (p − 64) · 125 >> 4: feedback ±0.97.
std::uint16_t fb_word(int p) { return static_cast<std::uint16_t>(((p - 64) * 125 >> 4) & 0x3FF); }

// v·scale rounded half away from zero by the x87 `± 0.5, _ftol` idiom; `r` = trunc − v·scale.
struct Q {
    float x;
    float r;
};
Q quantise(float v, float scale) {
    const float s = v * scale;
    const float x = v < 0.0f ? s - 0.5f : s + 0.5f;
    return {x, static_cast<float>(static_cast<int>(x)) - s};
}
std::uint16_t to_word(float x) { return static_cast<std::uint16_t>(static_cast<int>(x) & 0x3FF); }

float shelf_gain(const Tables& tb, unsigned g) { return tb.kShelfGain[std::clamp(g, 52u, 76u) - 52u]; }

} // namespace

VariationType variation_type(const Tables& tb, std::uint8_t msb, std::uint8_t lsb, bool system) {
    if (lsb > 9) lsb = 0;
    if (msb > 0x4E) return {0, 1};
    if (msb >= 0x40) {
        const std::size_t i = (msb - 0x40u) * 10u + lsb;
        return {tb.kVarTypeCat40[i], tb.kVarTypeSub40[i]};
    }
    if (msb == 0x14) return {tb.kVarTypeCat14[lsb], tb.kVarTypeSub14[lsb]};
    if (msb < 0x0C) {
        const std::size_t i = msb * 10u + lsb;
        return {tb.kVarTypeCat[i], tb.kVarTypeSub[i]};
    }
    return {0, static_cast<std::uint8_t>(system ? 0 : 1)};
}

std::array<std::uint16_t, 16> variation_defaults(const Tables& tb, VariationType t) {
    const std::uint16_t* row = nullptr;
    switch (t.category) {
    case 0: row = t.type < 23 ? &tb.kVarDefaults0[t.type * 16u] : nullptr; break;
    case 1: row = t.type < 3 ? &tb.kVarDefaults1[t.type * 16u] : nullptr; break;
    case 2: row = t.type < 9 ? &tb.kVarDefaults2[t.type * 16u] : nullptr; break;
    case 3: row = t.type < 13 ? &tb.kVarDefaults3[t.type * 16u] : nullptr; break;
    default: break;
    }
    std::array<std::uint16_t, 16> d{};
    if (row) std::copy(row, row + 16, d.begin());
    return d;
}

std::uint8_t variation_out_gain(const Tables& tb, VariationType t) {
    return t.category < 4 && t.type < 23 ? tb.kVarOutGain[t.category * 23u + t.type] : 0;
}

void EffectControl::set_variation_type(VariationType type) {
    var_cat_ = type.category;
    var_type_ = type.type;
    load_program(Block::Variation);
    load_defaults(Block::Variation);
    if (system_connection()) param_[2][9] = 127;
    var_param(kParamInit);
    push_block(Block::Variation);
}

void EffectControl::push_wl(std::uint32_t list, std::size_t n) { push_words(tb_.var_list(list), n); }
void EffectControl::push_al(std::uint32_t list, std::size_t n) { push_addrs(tb_.var_list(list), n); }

std::int16_t EffectControl::ms(std::uint16_t v) {
    return static_cast<std::int16_t>((static_cast<std::uint32_t>(v) * 0x468Fu) >> 12);
}

int EffectControl::out_gain(int v) const { return (variation_out_gain(tb_, {var_cat_, var_type_}) * v) >> 6; }

void EffectControl::filter(std::uint8_t order, std::uint8_t kind, std::uint8_t freq, std::uint8_t gain,
                      std::uint8_t q, std::uint32_t list) {
    const std::size_t fi = std::min<std::size_t>(freq, tb_.kTan44100.size() - 1);
    const float k = rate_ == 44100.0f
                        ? tb_.kTan44100[fi]
                        : static_cast<float>(std::tan(3.14159265358979 *
                                                      std::min(double(kEqFrequency[fi]), 0.45 * rate_) / rate_));
    const float g = shelf_gain(tb_, gain);
    const float g_cut = shelf_gain(tb_, 128u - gain);  // gain < 64: the inverse gain
    float b0 = 0, b1 = 0, b2 = 0, a1 = 0, a2 = 0;
    const auto first_order_pole = [&] { a1 = (1.0f - k) / (k + 1.0f); };
    switch (kind) {
    case 0:  // LPF
        b0 = b1 = (g * k) / (k + 1.0f);
        first_order_pole();
        break;
    case 1:  // HPF
        b0 = g / (k + 1.0f);
        b1 = -b0;
        first_order_pole();
        break;
    case 2: case 0x12: case 0x13: {  // low shelf
        float n = k, d = k;
        if (gain < 0x40) d = k * g_cut;
        else n = k * g;
        const float den = d + 1.0f;
        b0 = (1.0f + n) / den;
        b1 = (n - 1.0f) / den;
        a1 = (1.0f - d) / den;
        if (kind != 2) {
            b0 *= 1.414199948f;
            b1 *= 1.414199948f;
        }
        break;
    }
    case 3: case 0x0D: case 0x0E: case 0x0F: case 0x10: case 0x11: {  // high shelf
        float n = 1.0f, d = 1.0f;
        if (gain < 0x40) d = g_cut;
        else n = g;
        const float den = d + k;
        b0 = (k + n) / den;
        b1 = (k - n) / den;
        a1 = (d - k) / den;
        break;
    }
    case 4: case 5: case 8: case 9: case 10: {  // second order, Q = q / 10
        const float kk = k * k;
        const float kq = (k * 10.0f) / static_cast<float>(q);
        float damping = kq;  // the denominator's K/Q term
        if (kind == 10 && gain < 0x40) damping = kq * g_cut;
        const float n = 1.0f / (kk + damping + 1.0f);
        a1 = ((2.0f - kk) - kk) * n;
        a2 = ((damping - 1.0f) - kk) * n;
        switch (kind) {
        case 4: b0 = b2 = g * k * k * n; b1 = b0 + b0; break;
        case 5: b0 = b2 = g * n; b1 = -(b0 + b0); break;
        case 8: b0 = n * kq; b2 = -b0; break;
        case 9: b0 = b2 = (kk + 1.0f) * n; b1 = -a1; break;
        default:  // 10: peaking
            if (gain >= 0x40) {
                b0 = (kk + kq * g + 1.0f) * n;
                b2 = ((1.0f - kq * g) + kk) * n;
            } else {
                b0 = (kk + kq + 1.0f) * n;
                b2 = ((1.0f - kq) + kk) * n;
            }
            b1 = -a1;
            break;
        }
        break;
    }
    case 11: b0 = g; break;  // pure gain
    default: break;
    }

    const std::uint8_t* l = tb_.var_list(list);
    if (order == 1) {
        Q q0 = quantise(b0, 128.0f), q1 = quantise(b1, 128.0f);
        const Q qa = quantise(a1, 512.0f);
        const float sum = q0.r + q1.r;
        if (sum < -0.5f) {
            if (q0.r <= q1.r) q0.x += 1.0f; else q1.x += 1.0f;
        } else if (sum > 0.5f) {
            if (q0.r > q1.r) q0.x -= 1.0f; else q1.x -= 1.0f;
        }
        w_[l[0]] = to_word(q0.x);
        w_[l[1]] = to_word(q1.x);
        w_[l[2]] = to_word(qa.x);
        push_words(l, 3);
        return;
    }
    // Second order: all five ×128; the b and a groups each keep their summed rounding error
    // within ½ LSB by moving the word with the smallest (largest) residual.
    Q z[5] = {quantise(b0, 128.0f), quantise(b1, 128.0f), quantise(b2, 128.0f),
              quantise(a1, 128.0f), quantise(a2, 128.0f)};
    const float sb = z[0].r + z[1].r + z[2].r;
    if (sb < -0.5f) {
        const int i = z[1].r <= z[2].r ? (z[0].r <= z[1].r ? 0 : 1) : (z[0].r <= z[2].r ? 0 : 2);
        z[i].x += 1.0f;
    } else if (sb > 0.5f) {
        const int i = z[1].r > z[2].r ? (z[0].r > z[1].r ? 0 : 1) : (z[0].r > z[2].r ? 0 : 2);
        z[i].x -= 1.0f;
    }
    const float sa = z[3].r + z[4].r;
    if (sa < -0.5f) z[z[3].r <= z[4].r ? 3 : 4].x += 1.0f;
    else if (sa > 0.5f) z[z[3].r <= z[4].r ? 4 : 3].x -= 1.0f;
    for (std::size_t i = 0; i < 5; ++i) w_[l[i]] = to_word(z[i].x);
    push_words(l, 5);
}

void EffectControl::damp(std::uint8_t mode, std::uint8_t value, std::uint8_t gain, std::uint32_t list) {
    // h = (d + 8) / 18. The DLL disables the damping at rates other than 44.1 kHz; here it
    // applies at every rate (the effect control works in 44.1 kHz terms).
    const float g = shelf_gain(tb_, gain);
    const double h = (static_cast<double>(value) + 8.0) * static_cast<double>(0.0555555559694767f);
    double first = 0, second = 0;
    if (mode == 0) {
        first = h * g;
        second = static_cast<double>((1.0f - static_cast<float>(h)) * g);
    } else if (mode == 1) {
        first = h * g;
        second = static_cast<double>((static_cast<float>(h) - 1.0f) * g);
    } else if (mode == 2) {
        first = g;
    }
    const auto q = [](double v) {
        const double s = v * 512.0;
        return static_cast<std::uint16_t>(static_cast<int>(v < 0.0 ? s - 0.5 : s + 0.5) & 0x3FF);
    };
    const std::uint8_t* l = tb_.var_list(list);
    w_[l[0]] = q(first);
    w_[l[1]] = q(static_cast<double>(static_cast<float>(second)));
    push_words(l, 2);
}

void EffectControl::connection_changed() {
    // the variation input gain words 4 / 7 (0x296 = −2.83 system,
    // 0x380 = −1.0 insertion; the engines drive these from their own ramp) and the dry/wet
    // of the current type. The effect control's own return / send words are
    // likewise replaced by the engines' mixer, so they are not modelled.
    const std::uint16_t g = system_connection() ? 0x296 : 0x380;
    w_[4] = w_[7] = g;
    var_param(9);
}

void EffectControl::var_param(std::uint8_t index) {
    const std::uint8_t t = var_type_;
    switch (var_cat_) {
    case 0:
        switch (t) {
        case 2: case 3: case 4: case 5: case 6: case 7: case 0x14: case 0x15: var_chorus(index); break;
        case 8: case 9: case 0x16: var_flanger(index); break;
        case 10: var_symphonic(index); break;
        case 11: var_rotary(index); break;
        case 12: var_tremolo(index); break;
        case 13: var_auto_pan(index); break;
        case 14: var_delay_lcr(index); break;
        case 15: var_delay_lr(index); break;
        case 16: var_echo(index); break;
        case 17: var_cross_delay(index); break;
        case 18: var_type18(index); break;
        case 19: var_eq2(index); break;
        default: break;
        }
        break;
    case 1:
        if (t == 1) var_phaser1(index);
        else if (t == 2) var_phaser2(index);
        break;
    case 2:
        switch (t) {
        case 1: case 2: case 3: case 4: var_er(index); break;
        case 5: case 6: var_distortion(index); break;
        case 7: var_amp_sim(index); break;
        case 8: var_eq3(index); break;
        default: break;
        }
        break;
    case 3:
        if (t >= 1 && t <= 8) var_reverb(index);
        else if (t == 9) var_auto_wah(index);
        else if (t >= 10 && t <= 12) var_karaoke(index);
        break;
    default: break;
    }
}

// --- delay family (kernel) ----------------------------------------------------
// Taps: A28 (t1), A2d (t2), A33 (t3), A38 (t4); lines A3a (1) and A3b (16384).

namespace {
// (0xFFFF − 0x8000) / 2 + 1: the second delay line.
constexpr std::int16_t kLine2 = 16384;
} // namespace

void EffectControl::var_delay_lcr(std::uint8_t index) {
    const std::uint16_t dw = dry_wet();
    if (index == kParamInit) {
        for (std::uint8_t i : {0, 1, 2, 3, 4, 5, 6, 12, 14}) var_delay_lcr(i);
        return;
    }
    switch (index) {
    case 0: ar(0x33) = static_cast<std::int16_t>(ms(vp(0)) + ar(0x3a)); push_al(0x3e3, 1); break;
    case 1: ar(0x2d) = static_cast<std::int16_t>(ms(vp(1)) + ar(0x3a)); push_al(0x3e6, 1); break;
    case 2: ar(0x38) = static_cast<std::int16_t>(ms(vp(2)) + ar(0x3a)); push_al(0x3e8, 1); break;
    case 3: ar(0x28) = static_cast<std::int16_t>(ms(vp(3)) + ar(0x3a)); push_al(0x3e1, 1); break;
    case 4: wr(0xab) = fb_word(vp(4)); push_wl(0x3dc, 1); break;
    case 5: case 9: {
        wr(0xb8) = wr(0xb2) = static_cast<std::uint16_t>(tb_.kLevelCurve[128 - dw]);
        auto s = static_cast<std::int16_t>(tb_.kLevelCurve[dw]);
        if (!system_connection()) s = static_cast<std::int16_t>(s >> 1);
        wr(0xb9) = wr(0xb5) = static_cast<std::uint16_t>(s);
        const auto c = static_cast<std::uint16_t>(
            static_cast<int>(static_cast<float>(s) * tb_.kLevelSquare[std::min<std::uint16_t>(vp(5), 127)]));
        wr(0xbb) = wr(0xb6) = c;
        push_wl(0x3ec, 1);
        push_wl(0x3ee, 4);
        push_wl(0x3f3, 1);
        break;
    }
    case 6: damp(0, static_cast<std::uint8_t>(vp(6)), 0x40, 0x3b0); break;
    case 12: case 13:
        filter(1, 2, static_cast<std::uint8_t>(vp(12)), static_cast<std::uint8_t>(vp(13)), 0, 0x3a0);
        filter(1, 2, static_cast<std::uint8_t>(vp(12)), static_cast<std::uint8_t>(vp(13)), 0, 0x3a8);
        break;
    case 14: case 15:
        filter(1, 3, static_cast<std::uint8_t>(vp(14)), static_cast<std::uint8_t>(vp(15)), 0, 0x3a4);
        filter(1, 3, static_cast<std::uint8_t>(vp(14)), static_cast<std::uint8_t>(vp(15)), 0, 0x3ac);
        break;
    default: break;
    }
}

void EffectControl::var_delay_lr(std::uint8_t index) {
    const std::uint16_t dw = dry_wet();
    if (index == kParamInit) {
        for (std::uint8_t i : {0, 1, 2, 3, 4, 5, 9, 12, 14}) var_delay_lr(i);
        return;
    }
    switch (index) {
    case 0: ar(0x33) = static_cast<std::int16_t>(ms(vp(0)) + ar(0x3a)); push_al(0x3e3, 1); break;
    case 1: ar(0x38) = static_cast<std::int16_t>(ms(vp(1)) + ar(0x3a)); push_al(0x3e8, 1); break;
    case 2: ar(0x28) = static_cast<std::int16_t>(ms(vp(2)) + ar(0x3a)); push_al(0x3e1, 1); break;
    case 3: ar(0x2d) = static_cast<std::int16_t>(ms(vp(3)) + ar(0x3a)); push_al(0x3e6, 1); break;
    case 4: {
        // Two feedback taps: half the feedback each ((p − 64) · 125 >> 5).
        const auto f = static_cast<std::uint16_t>(((vp(4) - 0x40) * 0x7D >> 5) & 0x3FF);
        wr(0xac) = wr(0xab) = f;
        push_wl(0x3dc, 1);
        push_wl(0x3de, 1);
        break;
    }
    case 5:
        damp(0, static_cast<std::uint8_t>(vp(5)), 0x40, 0x3b0);
        damp(0, static_cast<std::uint8_t>(vp(5)), 0x40, 0x3b2);
        break;
    case 9: {
        wr(0xb8) = wr(0xb2) = static_cast<std::uint16_t>(tb_.kLevelCurve[128 - dw]);
        auto s = static_cast<std::int16_t>(tb_.kLevelCurve[dw]);
        if (!system_connection()) s = static_cast<std::int16_t>(s >> 1);
        wr(0xbb) = wr(0xb5) = static_cast<std::uint16_t>(s);
        push_wl(0x3ec, 1);
        push_wl(0x3ee, 2);
        push_wl(0x3f1, 1);
        break;
    }
    case 12: case 13:
        filter(1, 2, static_cast<std::uint8_t>(vp(12)), static_cast<std::uint8_t>(vp(13)), 0, 0x3a0);
        filter(1, 2, static_cast<std::uint8_t>(vp(12)), static_cast<std::uint8_t>(vp(13)), 0, 0x3a8);
        break;
    case 14: case 15:
        filter(1, 3, static_cast<std::uint8_t>(vp(14)), static_cast<std::uint8_t>(vp(15)), 0, 0x3a4);
        filter(1, 3, static_cast<std::uint8_t>(vp(14)), static_cast<std::uint8_t>(vp(15)), 0, 0x3ac);
        break;
    default: break;
    }
}

void EffectControl::var_echo(std::uint8_t index) {
    const std::uint16_t dw = dry_wet();
    if (index == kParamInit) {
        ar(0x3b) = kLine2;
        for (std::uint8_t i : {0, 1, 2, 3, 4, 5, 6, 7, 12, 14}) var_echo(i);
        return;
    }
    switch (index) {
    case 0: ar(0x28) = static_cast<std::int16_t>(ms(vp(0)) + ar(0x3a)); push_al(0x3e1, 1); break;
    case 1: wr(0xab) = fb_word(vp(1)); push_wl(0x3dc, 1); break;
    case 2: ar(0x2d) = static_cast<std::int16_t>(ms(vp(2)) + ar(0x3b)); push_al(0x3e6, 1); break;
    case 3: wr(0xaf) = fb_word(vp(3)); push_wl(0x3dd, 1); break;
    case 4:
        damp(0, static_cast<std::uint8_t>(vp(4)), 0x40, 0x3b0);
        damp(0, static_cast<std::uint8_t>(vp(4)), 0x40, 0x3b2);
        break;
    case 5: ar(0x33) = static_cast<std::int16_t>(ms(vp(5)) + ar(0x3a)); push_al(0x3e3, 1); break;
    case 6: ar(0x38) = static_cast<std::int16_t>(ms(vp(6)) + ar(0x3b)); push_al(0x3e8, 1); break;
    case 7: case 9: {
        // No insertion halving here (unlike the other delays).
        wr(0xb8) = wr(0xb2) = static_cast<std::uint16_t>(tb_.kLevelCurve[128 - dw]);
        const std::int16_t s = tb_.kLevelCurve[dw];
        wr(0xb9) = wr(0xb4) = static_cast<std::uint16_t>(s);
        const auto c = static_cast<std::uint16_t>(
            static_cast<int>(static_cast<float>(s) * tb_.kLevelSquare[std::min<std::uint16_t>(vp(7), 127)]));
        wr(0xbb) = wr(0xb5) = c;
        push_wl(0x3ec, 6);
        break;
    }
    case 12: case 13:
        filter(1, 2, static_cast<std::uint8_t>(vp(12)), static_cast<std::uint8_t>(vp(13)), 0, 0x3a0);
        filter(1, 2, static_cast<std::uint8_t>(vp(12)), static_cast<std::uint8_t>(vp(13)), 0, 0x3a8);
        break;
    case 14: case 15:
        filter(1, 3, static_cast<std::uint8_t>(vp(14)), static_cast<std::uint8_t>(vp(15)), 0, 0x3a4);
        filter(1, 3, static_cast<std::uint8_t>(vp(14)), static_cast<std::uint8_t>(vp(15)), 0, 0x3ac);
        break;
    default: break;
    }
}

void EffectControl::var_cross_delay(std::uint8_t index) {
    const std::uint16_t dw = dry_wet();
    const bool swap = vp(3) == 1;  // input select L&R swapped
    switch (index) {
    case kParamInit:
        for (std::uint8_t i : {3, 0, 1, 4, 12, 14}) var_cross_delay(i);
        break;
    case 0:
        ar(0x2d) = static_cast<std::int16_t>(ms(vp(0)) + (swap ? ar(0x3b) : ar(0x3a)));
        push_al(0x3e6, 1);
        break;
    case 1:
        ar(0x28) = static_cast<std::int16_t>(ms(vp(1)) + (swap ? ar(0x3a) : ar(0x3b)));
        push_al(0x3e1, 1);
        break;
    case 2: {
        const std::uint16_t f = fb_word(vp(2));
        // Select 1: the lines feed each other (c, e words); otherwise each feeds itself.
        wr(0xaf) = wr(0xab) = swap ? 0 : f;
        wr(0xae) = wr(0xac) = swap ? f : 0;
        push_wl(0x3dc, 4);
        break;
    }
    case 3:
        if (vp(3) == 0) {
            wr(0xaa) = 0x200;
            wr(0xad) = 0;
            ar(0x3a) = 1;
            ar(0x3b) = kLine2;
        } else if (vp(3) == 1) {
            wr(0xaa) = 0x200;
            wr(0xad) = 0;
            ar(0x3a) = kLine2;
            ar(0x3b) = 1;
        } else {
            wr(0xaa) = 0;
            wr(0xad) = 0x200;
            ar(0x3a) = 1;
            ar(0x3b) = kLine2;
        }
        push_wl(0x3ea, 2);
        push_al(0x3e0, 1);
        push_al(0x3e5, 1);
        // The tap addresses (parameters 1 / 2) are not recomputed here.
        var_cross_delay(2);
        var_cross_delay(9);
        break;
    case 4:
        damp(0, static_cast<std::uint8_t>(vp(4)), 0x40, 0x3b0);
        damp(0, static_cast<std::uint8_t>(vp(4)), 0x40, 0x3b2);
        break;
    case 9: {
        wr(0xb8) = wr(0xb2) = static_cast<std::uint16_t>(tb_.kLevelCurve[128 - dw]);
        auto s = static_cast<std::int16_t>(tb_.kLevelCurve[dw]);
        if (vp(3) < 2) s = static_cast<std::int16_t>(s >> 1);
        wr(0xb9) = wr(0xb4) = static_cast<std::uint16_t>(s);
        push_wl(0x3ec, 2);
        push_wl(0x3ef, 2);
        break;
    }
    case 12: case 13:
        filter(1, 2, static_cast<std::uint8_t>(vp(12)), static_cast<std::uint8_t>(vp(13)), 0, 0x3a0);
        filter(1, 2, static_cast<std::uint8_t>(vp(12)), static_cast<std::uint8_t>(vp(13)), 0, 0x3a8);
        break;
    case 14: case 15:
        filter(1, 3, static_cast<std::uint8_t>(vp(14)), static_cast<std::uint8_t>(vp(15)), 0, 0x3a4);
        filter(1, 3, static_cast<std::uint8_t>(vp(14)), static_cast<std::uint8_t>(vp(15)), 0, 0x3ac);
        break;
    default: break;
    }
}

void EffectControl::var_type18(std::uint8_t index) {
    // category 0 type 18, a single delay tap; no XG type selects it.
    const std::uint16_t dw = dry_wet();
    if (index == kParamInit) {
        for (std::uint8_t i : {0, 1, 5, 7}) var_type18(i);
        return;
    }
    switch (index) {
    case 0:
        ar(0x33) = static_cast<std::int16_t>(ms(tb_.kChorusDelay[std::min<std::uint16_t>(vp(0), 127)]) + ar(0x3a));
        push_al(0x3e3, 1);
        break;
    case 1: case 9: {
        wr(0xb7) = wr(0xb2) = static_cast<std::uint16_t>(tb_.kPanCurve[128 - dw]);
        auto s = static_cast<std::uint16_t>(tb_.kPanCurve[dw]);
        if (!system_connection()) s = static_cast<std::uint16_t>(static_cast<std::int16_t>(s) >> 1);
        if (s == 0x200) s = 0x1FF;
        const auto neg = static_cast<std::uint16_t>(-s & 0x3FF);
        if (vp(1) == 1) {
            wr(0xb5) = neg;
            wr(0xba) = s;
        } else {
            wr(0xb5) = s;
            wr(0xba) = neg;
        }
        push_wl(0x3ec, 1);
        push_wl(0x3ee, 1);
        push_wl(0x3f4, 2);
        break;
    }
    case 5: case 6:
        filter(1, 2, static_cast<std::uint8_t>(vp(5)), static_cast<std::uint8_t>(vp(6)), 0, 0x3a0);
        filter(1, 2, static_cast<std::uint8_t>(vp(5)), static_cast<std::uint8_t>(vp(6)), 0, 0x3a8);
        break;
    case 7: case 8:
        filter(1, 3, static_cast<std::uint8_t>(vp(7)), static_cast<std::uint8_t>(vp(8)), 0, 0x3a4);
        filter(1, 3, static_cast<std::uint8_t>(vp(7)), static_cast<std::uint8_t>(vp(8)), 0, 0x3ac);
        break;
    default: break;
    }
}

void EffectControl::var_eq2(std::uint8_t index) {
    // per channel a low shelf (p1 freq, p2 gain) and a high shelf (p3, p4).
    if (index == kParamInit) {
        var_eq2(0);
        var_eq2(2);
        return;
    }
    if (index < 2) {
        filter(1, 2, static_cast<std::uint8_t>(vp(0)), static_cast<std::uint8_t>(vp(1)), 0, 0x3a0);
        filter(1, 2, static_cast<std::uint8_t>(vp(0)), static_cast<std::uint8_t>(vp(1)), 0, 0x3a8);
    } else if (index < 4) {
        filter(1, 3, static_cast<std::uint8_t>(vp(2)), static_cast<std::uint8_t>(vp(3)), 0, 0x3a4);
        filter(1, 3, static_cast<std::uint8_t>(vp(2)), static_cast<std::uint8_t>(vp(3)), 0, 0x3ac);
    }
}

} // namespace hvyxg::fx
