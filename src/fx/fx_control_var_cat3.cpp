// Copyright (C) 2026 Kevin H. Patterson
// SPDX-License-Identifier: BSD-3-Clause (see LICENSE)
// Variation control, category 3: hall / room / stage / plate reverbs, auto wah, karaoke.
// Reimplements the behaviour of the 2006LE controller; syxg50.dll
// has the same logic. The reverb types use the system reverb's helpers (comb
// gains, tap spread, all-passes) over the variation's register
// lists; they are re-implemented here over a list so the system reverb keeps its own.

#include <algorithm>

#include "fx/fx_control.hpp"
#include "fx/fx_tables.hpp"

namespace hvyxg::fx {

namespace {

constexpr std::uint8_t kParamInit = 0xFF;

// 10 µs → 44.1 kHz samples.
std::int16_t samples_10us(std::uint16_t v) {
    return static_cast<std::int16_t>((static_cast<std::uint32_t>(v) * 0x70E5u) >> 16);
}

// per comb {input, fir, fir'} from the reverb time.
void var_comb_gains(const Tables& tb, std::uint16_t* w, std::uint16_t time, std::uint16_t set, const std::uint8_t* l) {
    const std::uint8_t base = tb.kReverbTimeBase[static_cast<std::size_t>(time) + 1];
    for (std::size_t k = 0; k < 4; ++k, l += 3) {
        const auto row = static_cast<std::uint8_t>(tb.kReverbTimeOffset[set * 4u + k] + base);
        w[l[0]] = tb.kCombGain[2u * row + 1];
        w[l[1]] = w[l[2]] = tb.kCombGain[2u * row];
    }
}

// comb lengths and the stereo spread of the output taps; per comb
// {write, read, tap, tap}.
void var_diffusion(const Tables& tb, std::int16_t* a, std::uint16_t diff, std::uint16_t set, const std::uint8_t* l) {
    const std::uint16_t depth = tb.kDiffusionDepth[diff];
    for (std::size_t k = 0; k < 4; ++k, l += 4) {
        const std::int16_t tap = samples_10us(tb.kDiffusionGeometry[set * 8u + 2 * k]);
        const std::int16_t len = samples_10us(tb.kDiffusionGeometry[set * 8u + 2 * k + 1]);
        const auto spread = static_cast<std::int16_t>((tb.kDiffusionSpread[k] * depth) >> 7);
        std::int16_t base = a[l[0]];
        a[l[1]] = static_cast<std::int16_t>(base + len);
        base = static_cast<std::int16_t>(base + tap);
        if ((k & 1) == 0) {
            a[l[2]] = base;
            base = static_cast<std::int16_t>(base + spread);
        } else {
            a[l[2]] = static_cast<std::int16_t>(base + spread);
        }
        a[l[3]] = base;
    }
}

// four all-passes; `doubled` marks the stages with ×2 output words.
void var_allpasses(const Tables& tb, std::uint16_t* w, std::int16_t* a, std::uint16_t density, std::int16_t diff,
               std::uint16_t set, unsigned doubled, const std::uint8_t* coef, const std::uint8_t* addr) {
    unsigned mask = tb.kDensityMask[std::min<std::uint16_t>(density, 3)];
    const std::uint16_t* row = &tb.kAllpassGeometry[set * 8u];
    for (std::size_t st = 0; st < 4; ++st, coef += 4, addr += 2) {
        std::int16_t len = st == 3 && diff == 0 ? samples_10us(row[2 * st - 2]) : samples_10us(row[2 * st]);
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
        if (doubled & 1) {
            out_x <<= 1;
            out_d <<= 1;
        }
        mask >>= 1;
        doubled >>= 1;
        w[coef[0]] = static_cast<std::uint16_t>(out_x & 0x3FF);
        w[coef[1]] = static_cast<std::uint16_t>(out_d & 0x3FF);
        w[coef[2]] = static_cast<std::uint16_t>(in_x & 0x3FF);
        w[coef[3]] = static_cast<std::uint16_t>(in_d & 0x3FF);
    }
}

std::uint8_t b(std::uint16_t v) { return static_cast<std::uint8_t>(v); }

} // namespace

void EffectControl::var_reverb(std::uint8_t index) {
    // Hall / room / stage / plate on the variation ring (kernel).
    const std::uint16_t set = tb_.kReverbTimeSet[var_type_];
    const auto ap = [&] {
        var_allpasses(tb_, w_.data(), a_.data(), vp(11), static_cast<std::int16_t>(vp(1)), set, 0xC,
                  tb_.var_list(0x4a0), tb_.var_list(0x498));
    };
    switch (index) {
    case kParamInit:
        for (std::uint8_t i : {0, 1, 2, 3, 4, 9, 11, 12, 14}) var_reverb(i);
        // Mono input mix: L + R in system connection, (L + R) / 2 in insertion.
        wr(0x6e) = wr(0x6d) = system_connection() ? 0x200 : 0x100;
        break;
    case 0:
        var_comb_gains(tb_, w_.data(), vp(0), set, tb_.var_list(0x48c));
        push_wl(0x48c, 12);
        break;
    case 1: {
        var_diffusion(tb_, a_.data(), vp(1), set, tb_.var_list(0x47c));
        const std::uint8_t* l = tb_.var_list(0x47c);
        const std::uint8_t taps[8] = {l[2], l[3], l[6], l[7], l[10], l[11], l[14], l[15]};
        push_addrs(taps, 8);
        ap();  // recomputed in the image, sent with the next density change
        push_al(0x49f, 1);
        break;
    }
    case 2: case 10: {
        const auto pre = static_cast<std::int16_t>(ms(tb_.kInitialDelay[std::min<std::uint16_t>(vp(2), 63)]) + ar(0x27));
        ar(0x2a) = pre;
        ar(0x24) = static_cast<std::int16_t>(ms(tb_.kInitialDelay[std::min<std::uint16_t>(vp(10), 63)]) + pre);
        push_al(0x4b1, 2);
        break;
    }
    case 3: filter(1, 1, b(vp(3)), 0x40, 0, 0x468); break;
    case 4: filter(1, 0, b(vp(4)), 0x40, 0, 0x46c); break;
    case 9: {
        const std::uint16_t dw = dry_wet();
        wr(0xac) = wr(0xb9) = static_cast<std::uint16_t>(tb_.kLevelCurve[128 - dw]);
        wr(0xae) = wr(0xbb) = static_cast<std::uint16_t>(tb_.kLevelCurve[dw]);
        push_wl(0x4c4, 1);
        push_wl(0x4c6, 1);
        push_wl(0x4c9, 2);
        break;
    }
    case 11:
        ap();
        push_wl(0x4a0, 16);
        break;
    case 12: {
        // ER / reverb balance; the ER scale is indexed by the variation type.
        const std::uint16_t v = vp(12);
        const auto er = static_cast<std::uint16_t>(
            static_cast<std::int16_t>((tb_.kLevelCurve[128 - v] * tb_.kErScale[var_type_]) >> 8));
        wr(0xa0) = wr(0x93) = er;
        const auto rev = static_cast<std::uint16_t>(tb_.kLevelCurve[v]);
        for (unsigned o : {0x9fu, 0x9eu, 0x9du, 0x9cu, 0x92u, 0x91u, 0x90u, 0x8fu}) wr(o) = rev;
        push_wl(0x4b4, 10);
        break;
    }
    case 14:
        wr(0x75) = static_cast<std::uint16_t>(((vp(14) - 64) * 125 >> 4) & 0x3FF);
        push_wl(0x4be, 1);
        break;
    default: break;
    }
}

void EffectControl::var_auto_wah(std::uint8_t index) {
    // Kernel: triangle LFO → exponential cutoff → two-integrator band-pass.
    switch (index) {
    case kParamInit:
        for (std::uint8_t i : {0, 1, 3, 5, 7, 9}) var_auto_wah(i);
        break;
    case 0:  // LFO frequency
        w_[tb_.var_list(0x477)[0]] = tb_.kLfoWord[std::min<std::uint16_t>(vp(0), 127)];
        push_wl(0x477, 1);
        break;
    case 1: case 2: {
        // Cutoff offset (p3) and LFO depth (p2): depth = (offset − 0x200) & 0x3ff · p2 / 127.
        const auto off = static_cast<std::int16_t>(tb_.var_list(0x4d0)[2 * vp(2)] |
                                                   (tb_.var_list(0x4d0)[2 * vp(2) + 1] << 8));
        wr(0x87) = static_cast<std::uint16_t>(off);
        wr(0x86) = static_cast<std::uint16_t>(
            ((static_cast<unsigned>(off - 0x200) & 0x3FFu) * vp(1)) / 0x7Fu);
        push_wl(0x479, 1);
        push_wl(0x478, 1);
        break;
    }
    case 3: {
        // Resonance: −5120 / p4 (/512) above 10, else −0.998.
        std::uint16_t r = 0x201;
        if (vp(3) >= 11) r = static_cast<std::uint16_t>(static_cast<int>(-5120.0 / vp(3)) & 0x3FF);
        wr(0x96) = wr(0x89) = r;
        push_wl(0x4c0, 2);
        break;
    }
    case 5: case 6:
        filter(1, 2, b(vp(5)), b(vp(6)), 0, 0x468);
        filter(1, 2, b(vp(5)), b(vp(6)), 0, 0x470);
        break;
    case 7: case 8:
        filter(1, 3, b(vp(7)), b(vp(8)), 0, 0x46c);
        filter(1, 3, b(vp(7)), b(vp(8)), 0, 0x474);
        break;
    case 9: {
        const std::uint16_t dw = dry_wet();
        wr(0xab) = wr(0xb9) = static_cast<std::uint16_t>(tb_.kPanCurve[128 - dw]);
        wr(0xad) = wr(0xba) = static_cast<std::uint16_t>(tb_.kPanCurve[dw]);
        push_wl(0x4c4, 2);
        push_wl(0x4c7, 2);
        break;
    }
    default: break;
    }
}

void EffectControl::var_karaoke(std::uint8_t index) {
    // Kernel: input filter, two all-passes, one damped feedback line.
    switch (index) {
    case kParamInit:
        for (std::uint8_t i : {0, 1, 2, 3, 9, 10}) var_karaoke(i);
        wr(0x6e) = wr(0x6d) = system_connection() ? 0x16A : 0xB5;
        break;
    case 0: {
        const auto d = static_cast<std::int16_t>(ms(tb_.kKaraokeDelay[std::min<std::uint16_t>(vp(0), 127)]) + ar(0x3e));
        ar(0x2f) = ar(0x2b) = ar(0x34) = d;
        push_al(0x47d, 3);
        break;
    }
    case 1: {
        const auto f = static_cast<std::uint16_t>(((vp(1) - 0x40) * 0x7D >> 5) & 0x3FF);
        wr(0xa5) = wr(0xa6) = f;
        push_wl(0x48d, 2);
        break;
    }
    case 2: filter(1, 1, b(vp(2)), 0x40, 0, 0x468); break;
    case 3: filter(1, 0, b(vp(3)), 0x40, 0, 0x46c); break;
    case 9: {
        // the stored dry/wet (forced to 127 only when the type is loaded in
        // system connection).
        const std::uint16_t v = std::min<std::uint16_t>(vp(9), 127);
        wr(0xac) = wr(0xb9) = static_cast<std::uint16_t>(tb_.kLevelCurve[128 - v]);
        wr(0xae) = wr(0xbb) = static_cast<std::uint16_t>(tb_.kLevelCurve[v]);
        push_wl(0x4c4, 1);
        push_wl(0x4c6, 1);
        push_wl(0x4c9, 2);
        break;
    }
    case 10:
        var_allpasses(tb_, w_.data(), a_.data(), vp(10), 10, tb_.kReverbTimeSet[3], 0, tb_.var_list(0x4a0),
                  tb_.var_list(0x498));
        push_wl(0x4a0, 16);
        break;
    default: break;
    }
}

} // namespace hvyxg::fx
