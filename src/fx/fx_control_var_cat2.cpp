// Copyright (C) 2026 Kevin H. Patterson
// SPDX-License-Identifier: BSD-3-Clause (see LICENSE)
// Variation control, category 2: early reflections / gate / reverse gate, distortion /
// overdrive, amp simulator, 3-band EQ. Reimplements the behaviour of the 2006LE controller; syxg50.dll has the same logic
// over the same tables.
//
// Category-2 programs run a mono path: pre-stage `x = c6d·L + c6e·R` and two first-order
// filters (lists), the type's core, then a post chain of first order
// biquad → first order.

#include <algorithm>
#include <cstring>

#include "fx/fx_control.hpp"
#include "fx/fx_tables.hpp"

namespace hvyxg::fx {

namespace {

constexpr std::uint8_t kParamInit = 0xFF;

// (p − 64) · 125 >> 4.
std::uint16_t fb_word(int p) { return static_cast<std::uint16_t>(((p - 64) * 125 >> 4) & 0x3FF); }

// 10 µs → 44.1 kHz samples.
std::uint16_t samples_10us(std::uint16_t v) {
    return static_cast<std::uint16_t>((static_cast<std::uint32_t>(v) * 0x70E5u) >> 16);
}

// the comb gain table's FIR word of a row, clamped to rows 0..191.
std::uint16_t comb_row_gain(const Tables& tb, std::int16_t row) {
    if (row < 0) return tb.kCombGain[0];
    if (row < 0xC0) return tb.kCombGain[2u * static_cast<unsigned>(row)];
    return tb.kCombGain[2u * 0xBF];
}

float list_float(const Tables& tb, std::uint32_t offset) {
    float f;
    std::memcpy(&f, tb.var_list(offset), sizeof f);
    return f;
}

// with the early reflections' lists: four all-pass stages from geometry `set`,
// enabled per the density mask (no ×2 output stages here). `coef` = 4 × {out x, out d, in x,
// in d}, `addr` = 4 × {write, read}.
void er_allpasses(const Tables& tb, std::uint16_t* w, std::int16_t* a, std::uint16_t density, std::int16_t diffusion,
                  std::uint16_t set, const std::uint8_t* coef, const std::uint8_t* addr) {
    unsigned mask = tb.kDensityMask[std::min<std::uint16_t>(density, 3)];
    const std::uint16_t* row = &tb.kAllpassGeometry[set * 8u];
    for (std::size_t st = 0; st < 4; ++st, coef += 4, addr += 2, mask >>= 1) {
        auto len = static_cast<std::int16_t>(
            st == 3 && diffusion == 0 ? samples_10us(row[2 * st - 2]) : samples_10us(row[2 * st]));
        if (len == 0) len = 1;
        a[addr[1]] = static_cast<std::int16_t>(len + a[addr[0]]);
        const auto g = static_cast<std::int16_t>(row[2 * st + 1]);
        int out_x = 0x80, out_d = 0, in_x = 0, in_d = 0;
        if (mask & 1) {
            out_x = static_cast<std::int16_t>(-static_cast<int>(g) >> 2);
            out_d = 0x80 - out_x;
            in_x = 0x200 - g;
            in_d = g;
        }
        w[coef[0]] = static_cast<std::uint16_t>(out_x & 0x3FF);
        w[coef[1]] = static_cast<std::uint16_t>(out_d & 0x3FF);
        w[coef[2]] = static_cast<std::uint16_t>(in_x & 0x3FF);
        w[coef[3]] = static_cast<std::uint16_t>(in_d & 0x3FF);
    }
}

} // namespace

// --- early reflections, gate reverb, reverse gate (kernel) -------------------
// p1 pattern, p2 room size, p3 diffusion, p4 initial delay, p5 feedback, p6 HPF, p7 LPF,
// p10 dry/wet, p11 liveness, p12 density, p13 high damp.

void EffectControl::var_er(std::uint8_t index) {
    const std::uint16_t dw = dry_wet();
    // the 14 tap delays and gains from the type's pattern. Room size and initial
    // delay stop at 44 / 63: the 2006LE control limits say so, and the S-YXG50 engine clamps
    // them before its effect control (whose own table allows 127; measured: 100 → 44 / 63).
    const auto taps = [&] {
        std::uint16_t pattern = vp(0);
        if (var_type_ == 3) pattern = static_cast<std::uint16_t>(pattern + 6);
        else if (var_type_ == 4) pattern = static_cast<std::uint16_t>(pattern + 8);
        const std::uint8_t room = tb_.var_list(0x0d8)[std::min<std::uint16_t>(vp(1), 44)];
        const std::uint8_t live = tb_.var_list(0x158)[vp(10)];
        const std::uint8_t* tap = tb_.var_list(0x168) + pattern * 0x38u;
        const auto base = static_cast<std::uint16_t>(ms(tb_.kInitialDelay[std::min<std::uint16_t>(vp(3), 63)]) +
                                                     ar(0x2d));
        constexpr int kMax = 0xFFFF - 0x8000;
        const std::uint8_t* gl = tb_.var_list(0x068);
        for (std::size_t k = 0; k < 14; ++k, tap += 4) {
            const std::uint16_t t = static_cast<std::uint16_t>(tap[0] | tap[1] << 8);
            const std::uint16_t g = static_cast<std::uint16_t>(tap[2] | tap[3] << 8);
            int d = static_cast<int>(samples_10us(t)) * room / 10 + base;
            std::uint16_t gw = 0;
            if (d > kMax) {
                d = kMax;
            } else if (static_cast<std::int16_t>(g) < 1) {
                gw = static_cast<std::uint16_t>(
                    -comb_row_gain(tb_, static_cast<std::int16_t>((live - 0x80) - g)) & 0x3FF);
            } else {
                gw = comb_row_gain(tb_, static_cast<std::int16_t>(g + (live - 0x80)));
            }
            a_[tb_.kErTapAddr[k]] = static_cast<std::int16_t>(d);
            w_[gl[k]] = gw;
        }
    };
    const auto allpasses = [&] {
        er_allpasses(tb_, w_.data(), a_.data(), vp(11), static_cast<std::int16_t>(vp(2)),
                     tb_.kErSet[std::min<std::size_t>(vp(0) + var_type_ * 6u, tb_.kErSet.size() - 1)],
                     tb_.var_list(0x044), tb_.var_list(0x03c));
    };
    switch (index) {
    case kParamInit:
        for (std::uint8_t i : {0, 2, 4, 5, 6, 9, 12}) var_er(i);
        wr(0x6e) = wr(0x6d) = system_connection() ? 0x200 : 0x100;
        break;
    case 0:
        taps();
        allpasses();
        push_wl(0x068, 14);
        push_addrs(tb_.kErTapAddr.data(), 14);
        push_wl(0x044, 16);
        push_al(0x03c, 8);
        break;
    case 1: case 3:
        taps();
        push_wl(0x068, 14);
        push_addrs(tb_.kErTapAddr.data(), 14);
        break;
    case 2: {
        // Diffusion: the two cross taps; the all-pass coefficients are recomputed but not sent.
        const std::uint8_t extra = tb_.var_list(0x094)[vp(2)];
        ar(0x3c) = static_cast<std::int16_t>(extra + ar(0x3d));
        ar(0x33) = static_cast<std::int16_t>(extra + ar(0x3e));
        push_al(0x07a, 2);
        allpasses();
        push_al(0x043, 1);
        break;
    }
    case 4: wr(0x86) = fb_word(vp(4)); push_wl(0x054, 1); break;
    case 5: filter(1, 1, static_cast<std::uint8_t>(vp(5)), 0x40, 0, 0x008); break;
    case 6: filter(1, 0, static_cast<std::uint8_t>(vp(6)), 0x40, 0, 0x00c); break;
    case 9:
        wr(0xb1) = wr(0xad) = static_cast<std::uint16_t>(tb_.kLevelCurve[128 - dw]);
        wr(0xb2) = wr(0xaf) = static_cast<std::uint16_t>(tb_.kLevelCurve[dw]);
        push_wl(0x07c, 2);
        push_wl(0x080, 2);
        break;
    case 10:
        taps();
        push_wl(0x068, 14);
        break;
    case 11:
        allpasses();
        push_wl(0x044, 16);
        break;
    case 12:
        filter(1, 0, tb_.var_list(0x087)[vp(12)], 0x40, 0, 0x010);
        break;
    default: break;
    }
}

// --- distortion / overdrive / amp simulator (kernel) --------------------------

namespace {

// drive → the shaper input gain d/128 (words 0x76, 0x7a) and the output level
// (word 0xa1), which keeps the small-signal gain (d/13)² below drive 11.
void drive_words(const Tables& tb, std::uint16_t* w, std::uint16_t drive, std::uint16_t level) {
    const auto d = static_cast<std::int16_t>(static_cast<unsigned>(drive) * 499u / 127u + 13);
    w[0x7a] = w[0x76] = static_cast<std::uint16_t>(d);
    const double c0 = list_float(tb, 0x000);  // 13
    const double c1 = list_float(tb, 0x004);  // 52
    const double ratio = drive < 11 ? (c0 * c0) / (static_cast<double>(d) * d) : (c0 * c0) / (c1 * c1);
    auto v = static_cast<std::int16_t>(
        static_cast<std::int32_t>(ratio * tb.kLevelSquare[std::min<std::uint16_t>(level, 127)] * 512.0));
    if (v == 0 && level != 0) v = 1;
    w[0xa1] = static_cast<std::uint16_t>(v);
}

// edge → the stage gains (255 − edge)/128 of the clip cascade,
// the tap weights 0.2, and the input normalisation 512 / (0.2 · (1 + s + s² + s³ + s⁴)).
void edge_words(std::uint16_t* w, std::uint16_t edge) {
    w[0x83] = 0x200;
    const auto s = static_cast<std::int16_t>(0xFF - (edge & 0xFF));
    w[0x87] = w[0x8a] = w[0x8d] = w[0x90] = static_cast<std::uint16_t>(s);
    w[0x91] = 0;
    w[0x92] = w[0x93] = w[0x94] = w[0x95] = w[0x96] = 0x66;
    const double f = static_cast<double>(s) * 0.0078125;
    const double poly = (((1.0 + f) * f + 1.0) * f + 1.0) * f + 1.0;
    w[0x7d] = static_cast<std::uint16_t>(
        static_cast<std::int16_t>(static_cast<std::int32_t>(512.0 / (poly * static_cast<double>(0.2f)))));
    w[0x80] = 0x200;
}

} // namespace

void EffectControl::var_distortion(std::uint8_t index) {
    // p1 drive, p2/p3 EQ low freq/gain, p4 LPF cutoff, p5 output level, p7/p8/p9 EQ mid
    // freq/gain/Q, p10 dry/wet, p11 edge.
    const std::uint16_t dw = dry_wet();
    switch (index) {
    case kParamInit:
        // Fixed pre filters: distortion HPF 20 Hz / LPF 10 kHz, overdrive 50 Hz / 2.8 kHz, +6 dB.
        filter(1, 1, var_type_ == 5 ? 0 : 8, 0x46, 0, 0x008);
        filter(1, 0, var_type_ == 5 ? 0x36 : 0x2c, 0x46, 0, 0x00c);
        for (std::uint8_t i : {0, 1, 3, 6, 9, 10}) var_distortion(i);
        break;
    case 0: case 4:
        drive_words(tb_, w_.data(), vp(0), vp(4));
        push_wl(0x024, 2);
        push_wl(0x028, 1);
        break;
    case 1: case 2:
        filter(1, 2, static_cast<std::uint8_t>(vp(1)), static_cast<std::uint8_t>(vp(2)), 0, 0x018);
        break;
    case 3: filter(1, 0, static_cast<std::uint8_t>(vp(3)), 0x43, 0, 0x014); break;
    case 6: case 7: case 8:
        filter(2, 10, static_cast<std::uint8_t>(vp(6)), static_cast<std::uint8_t>(vp(7)),
               static_cast<std::uint8_t>(vp(8)), 0x01c);
        break;
    case 9: {
        wr(0xb1) = wr(0xad) = static_cast<std::uint16_t>(tb_.kPanCurve[128 - dw]);
        auto s = static_cast<std::int16_t>(tb_.kPanCurve[dw]);
        if (!system_connection()) s = static_cast<std::int16_t>(s >> 1);
        wr(0xb3) = wr(0xb0) = static_cast<std::uint16_t>(out_gain(s));
        push_wl(0x07c, 1);
        push_wl(0x07e, 1);
        push_wl(0x080, 1);
        push_wl(0x082, 1);
        break;
    }
    case 10:
        edge_words(w_.data(), vp(10));
        push_wl(0x02c, 5);
        push_wl(0x026, 2);
        break;
    default: break;
    }
}

void EffectControl::var_amp_sim(std::uint8_t index) {
    // p1 drive, p2 amp type, p3 LPF cutoff, p4 output level, p10 dry/wet, p11 edge.
    const std::uint16_t dw = dry_wet();
    switch (index) {
    case kParamInit:
        for (std::uint8_t i : {0, 1, 9, 10}) var_amp_sim(i);
        break;
    case 0: case 3:
        drive_words(tb_, w_.data(), vp(0), vp(3));
        push_wl(0x024, 2);
        push_wl(0x028, 1);
        break;
    case 1: {
        // The amp type's filters (13 bytes per type): pre HPF / LPF, post
        // low shelf and biquad, each {kind, gain, freq} (+ Q for the biquad).
        const std::uint8_t* t = tb_.var_list(0x0a0) + vp(1) * 13u;
        filter(1, t[0], t[2], static_cast<std::uint8_t>(t[1] + 3), 0, 0x008);
        filter(1, t[3], t[5], static_cast<std::uint8_t>(t[4] + 3), 0, 0x00c);
        filter(1, t[6], t[8], t[7], 0, 0x018);
        filter(2, t[9], t[11], t[10], t[12], 0x01c);
        var_amp_sim(2);
        break;
    }
    case 2: {
        const std::uint16_t type = vp(1);
        const std::uint8_t gain = type == 1 ? 0x3E : (type != 3 ? 0x46 : 0x45);
        filter(1, 0, static_cast<std::uint8_t>(vp(2)), gain, 0, 0x014);
        break;
    }
    case 9: {
        wr(0xb1) = wr(0xad) = static_cast<std::uint16_t>(tb_.kPanCurve[128 - dw]);
        auto s = static_cast<std::int16_t>(tb_.kPanCurve[dw]);
        if (!system_connection()) s = static_cast<std::int16_t>(s >> 1);
        wr(0xb3) = wr(0xb0) = static_cast<std::uint16_t>(out_gain(s));
        push_wl(0x07c, 1);
        push_wl(0x07e, 1);
        push_wl(0x080, 1);
        push_wl(0x082, 1);
        break;
    }
    case 10:
        edge_words(w_.data(), vp(10));
        push_wl(0x02c, 5);
        push_wl(0x026, 2);
        break;
    default: break;
    }
}

// --- 3-band EQ (pre + post stages only, mono) ----------------------------------------------

void EffectControl::var_eq3(std::uint8_t index) {
    // p1 low gain, p2 mid freq, p3 mid gain, p4 mid Q, p5 high gain, p6 low freq, p7 high freq.
    // Each shelf is split over two first-order sections (half the gain each).
    switch (index) {
    case kParamInit:
        for (std::uint8_t i : {0, 1, 4}) var_eq3(i);
        wr(0x6e) = wr(0x6d) = system_connection() ? 0x200 : 0x100;
        break;
    case 0: case 5: {
        const auto half = static_cast<std::int8_t>((static_cast<std::int16_t>(vp(0)) - 0x40) / 2);
        filter(1, 2, static_cast<std::uint8_t>(vp(5)), static_cast<std::uint8_t>(half + 0x40), 0, 0x008);
        filter(1, 2, static_cast<std::uint8_t>(vp(5)), static_cast<std::uint8_t>(vp(0) - half), 0, 0x00c);
        break;
    }
    case 1: case 2: case 3:
        filter(2, 10, static_cast<std::uint8_t>(vp(1)), static_cast<std::uint8_t>(vp(2)),
               static_cast<std::uint8_t>(vp(3)), 0x01c);
        break;
    case 4: case 6: {
        const auto half = static_cast<std::int8_t>((static_cast<std::int16_t>(vp(4)) - 0x40) / 2);
        filter(1, 3, static_cast<std::uint8_t>(vp(6)), static_cast<std::uint8_t>(half + 0x40), 0, 0x014);
        filter(1, 3, static_cast<std::uint8_t>(vp(6)), static_cast<std::uint8_t>(vp(4) - half), 0, 0x018);
        break;
    }
    default: break;
    }
}

} // namespace hvyxg::fx
