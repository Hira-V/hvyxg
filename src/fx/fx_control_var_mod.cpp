// Copyright (C) 2026 Kevin H. Patterson
// SPDX-License-Identifier: BSD-3-Clause (see LICENSE)
// Variation control, modulation family: chorus / celeste / flanger / symphonic / rotary /
// tremolo / auto pan (category 0) and phaser (category 1). Reimplements the behaviour of the 2006LE controller
// syxg50.dll has the same logic.
//
// Registers by role (2006LE lists): 0x77 LFO A word, 0x6e / 0x79 / 0x88 tap
// phase offsets, 0x75 / 0x85 / 0x95 / 0xa5 delay-modulation depths, 0x83 / 0x94 / 0xa4 AM
// depths, {0x72, 0x71, 0x6f, 0x73} / {0x7c, 0x7b, 0x7a, 0x7d} / {0x8b, 0x8a, 0x89, 0x8c} /
// {0x90, 0x8f, 0x8e, 0x91} the LFO smoothers {gain, pole, offset, centre}.

#include <algorithm>

#include "fx/fx_control.hpp"
#include "fx/fx_tables.hpp"

namespace hvyxg::fx {

namespace {

constexpr std::uint8_t kParamInit = 0xFF;
constexpr std::int16_t kLine2 = 16384;  // (0xFFFF − 0x8000) / 2 + 1

// feedback ±0.97 / ±0.73.
std::uint16_t fb_word(int p) { return static_cast<std::uint16_t>(((p - 64) * 125 >> 4) & 0x3FF); }
std::uint16_t chorus_fb_word(int p) { return static_cast<std::uint16_t>(((p - 64) * 94 >> 4) & 0x3FF); }
// LFO phase difference, (64 − v) · 3° (0x200 = 180°).
std::uint16_t phase_word(int v) { return static_cast<std::uint16_t>(((64 - v) * 0x200 / 60) & 0x3FF); }
// depth, v · 128 / 127.
std::uint16_t depth_word(std::uint8_t v) { return static_cast<std::uint16_t>((v * 0x80u) / 0x7Fu); }
// AM depth −v / 127 (s10 / 512), the DLL's division by −127 bit for bit.
std::uint16_t am_word(std::int8_t v) {
    if (v == 127) return 0x201;
    if (v == -127) return 0x1FF;
    const std::int32_t x = v * 0x200;
    const auto hi = static_cast<std::int32_t>((static_cast<std::int64_t>(x) * 0x7EFDFBF7LL) >> 32);
    const std::int32_t t = hi - x;
    return static_cast<std::uint16_t>(((t >> 6) - (t >> 31)) & 0x3FF);
}
std::uint16_t level(const Tables& tb, int i) { return static_cast<std::uint16_t>(tb.kPanCurve[static_cast<std::size_t>(i)]); }
std::uint8_t b(std::uint16_t v) { return static_cast<std::uint8_t>(v); }

} // namespace

// --- chorus / celeste and flanger ----------------------------

void EffectControl::var_chorus(std::uint8_t index) {
    const std::uint16_t dw = dry_wet();
    switch (index) {
    case kParamInit:
        for (std::uint8_t i : {0, 1, 2, 3}) var_chorus(i);
        return;
    case 0:
        w_[tb_.var_list(0x3b4)[0]] = tb_.kLfoWord[std::min<std::uint16_t>(vp(0), 127)];
        push_wl(0x3b4, 1);
        return;
    case 1: {
        const auto v = static_cast<std::uint8_t>(vp(1) << 1);
        for (const std::uint32_t l : {0x3c1u, 0x3c4u, 0x3c7u, 0x3c9u}) {
            w_[tb_.var_list(l)[0]] = depth_word(v);
            push_wl(l, 1);
        }
        return;
    }
    case 2:
        wr(0xaf) = wr(0xab) = chorus_fb_word(vp(2));
        push_wl(0x3dc, 2);
        return;
    case 3: case 14: {
        const std::int16_t d = ms(tb_.kChorusDelay[std::min<std::uint16_t>(vp(3), 127)]);
        std::int16_t base = ar(0x3a);
        ar(0x28) = static_cast<std::int16_t>(base + 1 + d);
        ar(0x29) = static_cast<std::int16_t>(base + 2 + d);
        ar(0x33) = static_cast<std::int16_t>(base + 0x41 + d);
        ar(0x34) = static_cast<std::int16_t>(base + 0x42 + d);
        if (vp(14) == 0) {  // mono input: L + R into line 1, the R taps read it too
            wr(0xaa) = 0x200;
            ar(0x3b) = 0;
        } else {
            wr(0xaa) = 0;
            base = kLine2;
            ar(0x3b) = base;
        }
        ar(0x2d) = static_cast<std::int16_t>(base + 1 + d);
        ar(0x2e) = static_cast<std::int16_t>(base + 2 + d);
        ar(0x38) = static_cast<std::int16_t>(base + 0x41 + d);
        ar(0x39) = static_cast<std::int16_t>(base + 0x42 + d);
        push_wl(0x3ea, 1);
        push_al(0x3e1, 4);
        push_al(0x3e5, 5);
        var_chorus(5);
        var_chorus(7);
        var_chorus(9);
        return;
    }
    case 5: case 6: {
        // ×√2 low shelf in system connection or with stereo input.
        const std::uint8_t kind = (system_connection() || vp(14) != 0) ? 0x13 : 2;
        filter(1, kind, b(vp(5)), b(vp(6)), 0, 0x3a0);
        filter(1, kind, b(vp(5)), b(vp(6)), 0, 0x3a8);
        return;
    }
    case 7: case 8:
        filter(1, 3, b(vp(7)), b(vp(8)), 0, 0x3a4);
        filter(1, 3, b(vp(7)), b(vp(8)), 0, 0x3ac);
        return;
    case 9: {
        auto dry = static_cast<std::int16_t>(level(tb_, 128 - dw));
        if (vp(14) == 1) dry = static_cast<std::int16_t>(dry * 7 / 10);
        wr(0xb8) = wr(0xb2) = static_cast<std::uint16_t>(dry);
        auto wet = static_cast<std::int16_t>(level(tb_, dw));
        if (vp(14) == 0 && !system_connection()) wet = static_cast<std::int16_t>(wet * 7 / 10);
        wr(0xb9) = wr(0xb4) = static_cast<std::uint16_t>(out_gain(wet));
        if (var_type_ > 4) wr(0xbb) = wr(0xb5) = static_cast<std::uint16_t>(out_gain(wet));
        push_wl(0x3ec, 6);
        return;
    }
    default: return;
    }
}

void EffectControl::var_flanger(std::uint8_t index) {
    const std::uint16_t dw = dry_wet();
    switch (index) {
    case kParamInit:
        for (std::uint8_t i : {0, 1, 2, 3, 13}) var_flanger(i);
        return;
    case 0: var_chorus(0); return;
    case 1:
        for (const std::uint32_t l : {0x3c1u, 0x3c4u}) {
            w_[tb_.var_list(l)[0]] = depth_word(b(vp(1)));
            push_wl(l, 1);
        }
        return;
    case 2:
        wr(0xaf) = wr(0xab) = fb_word(vp(2));
        push_wl(0x3dc, 2);
        return;
    case 3: case 14: {
        const std::int16_t d = ms(tb_.kChorusDelay[std::min<std::uint16_t>(vp(3), 127)]);
        std::int16_t base = ar(0x3a);
        ar(0x28) = static_cast<std::int16_t>(base + 1 + d);
        ar(0x29) = static_cast<std::int16_t>(base + 2 + d);
        if (vp(14) == 0) {
            wr(0xaa) = 0x200;
            ar(0x3b) = 0;
        } else {
            wr(0xaa) = 0;
            base = kLine2;
            ar(0x3b) = base;
        }
        ar(0x2d) = static_cast<std::int16_t>(base + 1 + d);
        ar(0x2e) = static_cast<std::int16_t>(base + 2 + d);
        push_wl(0x3ea, 1);
        push_al(0x3e1, 2);
        push_al(0x3e5, 3);
        var_flanger(5);
        var_flanger(7);
        var_flanger(9);
        return;
    }
    case 5: case 6: case 7: case 8: var_chorus(index); return;
    case 9: {
        auto dry = static_cast<std::int16_t>(level(tb_, 128 - dw));
        if (vp(14) == 1) dry = static_cast<std::int16_t>(dry * 7 / 10);
        wr(0xb8) = wr(0xb2) = static_cast<std::uint16_t>(dry);
        auto wet = static_cast<std::int16_t>(level(tb_, dw));
        if (vp(14) == 0 && !system_connection()) wet = static_cast<std::int16_t>(wet * 7 / 10);
        wr(0xb9) = wr(0xb4) = static_cast<std::uint16_t>(out_gain(wet));
        push_wl(0x3ec, 2);
        push_wl(0x3ef, 2);
        return;
    }
    case 13:
        w_[tb_.var_list(0x3ba)[0]] = phase_word(vp(13));
        push_wl(0x3ba, 1);
        return;
    default: return;
    }
}

// --- LFO smoothers ---------------------------------------------
// Each tap's LFO goes through a one-pole smoother {gain, pole, offset, centre}; its rate follows
// the LFO word 0x77 (the smoother, rotary's rotor, runs an octave higher).

namespace {

std::uint16_t octave_up(std::uint16_t u) {
    const auto v = static_cast<std::uint16_t>((u & 0x380) + 0x80);
    return v < 0x380 ? static_cast<std::uint16_t>(v | (u & 0x7F)) : std::uint16_t{0x37F};
}

} // namespace

// --- symphonic -----------------------------------------------------------------

void EffectControl::var_symphonic(std::uint8_t index) {
    const std::uint16_t dw = dry_wet();
    // smoother at half the LFO rate (the LFO rate, doubled).
    const auto smoother = [this](std::uint32_t l) {
        const std::uint16_t u = w_[0x77];
        const std::uint8_t* r = tb_.var_list(l);
        if (l == 0x3d8) {
            w_[r[0]] = u;
            w_[r[1]] = octave_up(u);
        } else {
            w_[r[0]] = static_cast<std::uint16_t>((((u & 0xFF) >> 1) & 0x3F) | (u & 0x380));
            w_[r[1]] = u;
        }
        w_[r[2]] = 0x100;
        w_[r[3]] = 0x3C0;
        push_wl(l, 4);
    };
    switch (index) {
    case kParamInit:
        for (std::uint8_t i : {0, 1, 2, 5, 7, 9}) var_symphonic(i);
        return;
    case 0:
        var_chorus(0);
        smoother(0x3cc);
        smoother(0x3d0);
        smoother(0x3d4);
        return;
    case 1: {
        const auto a = static_cast<std::uint8_t>((vp(1) * 5) >> 2);
        for (const std::uint32_t l : {0x3c1u, 0x3c4u, 0x3c7u}) {
            w_[tb_.var_list(l)[0]] = depth_word(a);
            push_wl(l, 1);
        }
        const auto c = static_cast<std::uint8_t>((vp(1) * 3) >> 3);
        for (const std::uint32_t l : {0x3c2u, 0x3c5u, 0x3c8u}) {
            w_[tb_.var_list(l)[0]] = depth_word(c);
            push_wl(l, 1);
        }
        return;
    }
    case 2: {
        const std::int16_t d = ms(tb_.kChorusDelay[std::min<std::uint16_t>(vp(2), 127)]);
        const std::int16_t base = ar(0x3a);
        ar(0x28) = static_cast<std::int16_t>(base + 1 + d);
        ar(0x29) = static_cast<std::int16_t>(base + 2 + d);
        ar(0x33) = static_cast<std::int16_t>(base + 0x41 + d);
        ar(0x34) = static_cast<std::int16_t>(base + 0x42 + d);
        ar(0x2d) = static_cast<std::int16_t>(base + 0x81 + d);
        ar(0x2e) = static_cast<std::int16_t>(base + 0x82 + d);
        push_al(0x3e1, 4);
        push_al(0x3e6, 2);
        return;
    }
    case 5: case 6:
        filter(1, 2, b(vp(5)), b(vp(6)), 0, 0x3a0);
        filter(1, 2, b(vp(5)), b(vp(6)), 0, 0x3a8);
        return;
    case 7: case 8:
        filter(1, 3, b(vp(7)), b(vp(8)), 0, 0x3a4);
        filter(1, 3, b(vp(7)), b(vp(8)), 0, 0x3ac);
        return;
    case 9: {
        wr(0xb8) = wr(0xb2) = level(tb_, 128 - dw);
        auto wet = static_cast<std::int16_t>(level(tb_, dw));
        if (!system_connection()) wet = static_cast<std::int16_t>(wet >> 1);
        const auto g = static_cast<std::uint16_t>(out_gain(static_cast<std::int16_t>(wet * 10 / 7)));
        wr(0xba) = wr(0xb9) = wr(0xb5) = wr(0xb4) = g;
        push_wl(0x3ec, 5);
        push_wl(0x3f5, 1);
        return;
    }
    default: return;
    }
}

// --- rotary --------------------------------------------------------------------

void EffectControl::var_rotary(std::uint8_t index) {
    const std::uint16_t dw = dry_wet();
    const auto smoother = [this](std::uint32_t l) {
        const std::uint16_t u = w_[0x77];
        const std::uint8_t* r = tb_.var_list(l);
        if (l == 0x3d8) {
            w_[r[0]] = u;
            w_[r[1]] = octave_up(u);
        } else {
            w_[r[0]] = static_cast<std::uint16_t>((((u & 0xFF) >> 1) & 0x3F) | (u & 0x380));
            w_[r[1]] = u;
        }
        w_[r[2]] = 0x100;
        w_[r[3]] = 0x3C0;
        push_wl(l, 4);
    };
    switch (index) {
    case kParamInit:
        for (std::uint8_t i : {0, 1, 5, 7, 9}) var_rotary(i);
        return;
    case 0:
        var_chorus(0);
        for (const std::uint32_t l : {0x3ccu, 0x3d0u, 0x3d4u, 0x3d8u}) smoother(l);
        return;
    case 1: {
        const auto am = static_cast<std::int8_t>(vp(1) < 0x20 ? static_cast<std::uint8_t>(vp(1) << 2) : 0x7F);
        for (const std::uint32_t l : {0x3c0u, 0x3c3u, 0x3c6u}) {
            w_[tb_.var_list(l)[0]] = am_word(am);
            push_wl(l, 1);
        }
        const auto pm = static_cast<std::uint8_t>(b(vp(1)) >> 1);
        for (const std::uint32_t l : {0x3c1u, 0x3c4u, 0x3c7u, 0x3c9u}) {
            w_[tb_.var_list(l)[0]] = depth_word(pm);
            push_wl(l, 1);
        }
        return;
    }
    case 5: case 6: {
        const std::uint8_t kind = system_connection() ? 0x13 : 2;
        filter(1, kind, b(vp(5)), b(vp(6)), 0, 0x3a0);
        filter(1, kind, b(vp(5)), b(vp(6)), 0, 0x3a8);
        return;
    }
    case 7: case 8:
        filter(1, 3, b(vp(7)), b(vp(8)), 0, 0x3a4);
        filter(1, 3, b(vp(7)), b(vp(8)), 0, 0x3ac);
        return;
    case 9: {
        wr(0xb8) = wr(0xb2) = level(tb_, 128 - dw);
        auto wet = static_cast<std::int16_t>(level(tb_, dw));
        if (!system_connection()) wet = static_cast<std::int16_t>(wet * 7 / 10);
        const auto g = static_cast<std::uint16_t>(out_gain(wet));
        wr(0xbb) = wr(0xb6) = wr(0xba) = wr(0xb9) = wr(0xb5) = wr(0xb4) = g;
        push_wl(0x3ec, 6);
        push_wl(0x3f3, 1);
        push_wl(0x3f5, 1);
        return;
    }
    default: return;
    }
}

// --- tremolo / auto pan ----------------------------------------

namespace {
// mode 1 for the lists other than 0x3d8: smoother at the LFO rate with the
// triangle offset (min(w >> 4, 10) + 245) / 512.
struct Smoother {
    std::uint16_t gain, pole, offset;
};
Smoother smoother_words(std::uint16_t u) {
    const int s = static_cast<std::int16_t>(u) >> 4;
    return {u, u, static_cast<std::uint16_t>((s < 11 ? s : 10) + 0xF5)};
}
} // namespace

void EffectControl::var_tremolo(std::uint8_t index) {
    const auto smoother = [this](std::uint32_t l) {
        const Smoother s = smoother_words(w_[0x77]);
        const std::uint8_t* r = tb_.var_list(l);
        w_[r[1]] = w_[r[0]] = s.gain;
        w_[r[2]] = s.offset;
        w_[r[3]] = 0x3C0;
        push_wl(l, 4);
    };
    switch (index) {
    case kParamInit:
        for (std::uint8_t i : {0, 1, 2, 13, 14}) var_tremolo(i);
        return;
    case 0:
        var_chorus(0);
        smoother(0x3cc);
        smoother(0x3d0);
        return;
    case 1:
        for (const std::uint32_t l : {0x3c0u, 0x3c3u}) {
            w_[tb_.var_list(l)[0]] = am_word(static_cast<std::int8_t>(vp(1)));
            push_wl(l, 1);
        }
        return;
    case 2:
        for (const std::uint32_t l : {0x3c1u, 0x3c4u}) {
            w_[tb_.var_list(l)[0]] = depth_word(static_cast<std::uint8_t>(b(vp(2)) >> 2));
            push_wl(l, 1);
        }
        return;
    case 5: case 6: {
        const std::uint8_t kind = (system_connection() || vp(14) != 0) ? 0x13 : 2;
        filter(1, kind, b(vp(5)), b(vp(6)), 0, 0x3a0);
        filter(1, kind, b(vp(5)), b(vp(6)), 0, 0x3a8);
        return;
    }
    case 7: case 8:
        filter(1, 3, b(vp(7)), b(vp(8)), 0, 0x3a4);
        filter(1, 3, b(vp(7)), b(vp(8)), 0, 0x3ac);
        return;
    case 13:
        w_[tb_.var_list(0x3ba)[0]] = phase_word(vp(13));
        push_wl(0x3ba, 1);
        return;
    case 14:
        if (vp(14) == 0) {
            wr(0xaa) = 0x200;
            wr(0xb9) = wr(0xb4) = static_cast<std::uint16_t>(out_gain(system_connection() ? 0x200 : 0x16A));
            ar(0x3b) = 0;
            ar(0x2d) = static_cast<std::int16_t>(ar(0x3a) + 1);
            ar(0x2e) = static_cast<std::int16_t>(ar(0x3a) + 2);
        } else {
            wr(0xaa) = 0;
            wr(0xb9) = wr(0xb4) = static_cast<std::uint16_t>(out_gain(0x200));
            ar(0x3b) = 0x400;
            ar(0x2d) = 0x401;
            ar(0x2e) = 0x402;
        }
        push_wl(0x3ea, 1);
        push_wl(0x3ed, 1);
        push_wl(0x3f0, 1);
        push_al(0x3e5, 3);
        var_tremolo(5);
        var_tremolo(7);
        return;
    default: return;
    }
}

void EffectControl::var_auto_pan(std::uint8_t index) {
    const auto smoother = [this](std::uint32_t l) {
        const Smoother s = smoother_words(w_[0x77]);
        const std::uint8_t* r = tb_.var_list(l);
        w_[r[1]] = w_[r[0]] = s.gain;
        w_[r[2]] = s.offset;
        w_[r[3]] = 0x3C0;
        push_wl(l, 4);
    };
    const auto phase = [this](std::uint8_t v, std::uint32_t l) {
        w_[tb_.var_list(l)[0]] = phase_word(v);
        push_wl(l, 1);
    };
    switch (index) {
    case kParamInit:
        wr(0xb9) = wr(0xb4) = static_cast<std::uint16_t>(out_gain(system_connection() ? 0x2D4 : 0x16A));
        for (std::uint8_t i : {0, 3, 5, 7}) var_auto_pan(i);
        return;
    case 0:
        var_chorus(0);
        if (vp(3) < 5) {
            smoother(0x3cc);
            smoother(0x3d0);
        }
        return;
    case 1: {
        const auto v = static_cast<std::int8_t>(vp(3) < 5 ? vp(1) : -static_cast<int>(vp(1)));
        for (const std::uint32_t l : {0x3c0u, 0x3c3u}) {
            w_[tb_.var_list(l)[0]] = am_word(v);
            push_wl(l, 1);
        }
        return;
    }
    case 2: {
        const auto v = static_cast<std::uint8_t>((vp(3) == 3 || vp(3) == 4) ? b(vp(2)) >> 2 : 0);
        for (const std::uint32_t l : {0x3c1u, 0x3c4u}) {
            w_[tb_.var_list(l)[0]] = depth_word(v);
            push_wl(l, 1);
        }
        return;
    }
    case 3: {
        if (vp(3) > 4) {
            // Square-like L/R: fixed phases and hard smoother coefficients.
            phase(0x40, 0x3b8);
            phase(0x7C, 0x3ba);
            wr(0x7b) = wr(0x71) = 0x3FF;
            wr(0x7c) = wr(0x72) = 0x3FF;
            wr(0x7a) = wr(0x6f) = 0x340;
            wr(0x7d) = wr(0x73) = 0x200;
            push_wl(0x3cc, 4);
            push_wl(0x3d0, 4);
        } else {
            // Direction: L↔R, L→R, L←R, turn L, turn R.
            static constexpr std::uint8_t kL[5] = {0x40, 0x54, 0x40, 0x5E, 0x40};
            static constexpr std::uint8_t kR[5] = {0x7C, 0x40, 0x54, 0x40, 0x5E};
            phase(kL[vp(3)], 0x3b8);
            phase(kR[vp(3)], 0x3ba);
            smoother(0x3cc);
            smoother(0x3d0);
        }
        var_auto_pan(1);
        var_auto_pan(2);
        return;
    }
    case 5: case 6:
        filter(1, 2, b(vp(5)), b(vp(6)), 0, 0x3a0);
        filter(1, 2, b(vp(5)), b(vp(6)), 0, 0x3a8);
        return;
    case 7: case 8:
        filter(1, 3, b(vp(7)), b(vp(8)), 0, 0x3a4);
        filter(1, 3, b(vp(7)), b(vp(8)), 0, 0x3ac);
        return;
    default: return;
    }
}

// --- phaser (category 1) ----------------------------------------

void EffectControl::var_phaser1(std::uint8_t index) {
    const std::uint16_t dw = dry_wet();
    switch (index) {
    case kParamInit:
        for (std::uint8_t i : {0, 1, 3, 5, 7, 9, 10}) var_phaser1(i);
        wr(0x85) = wr(0x84) = system_connection() ? 0x200 : 0x100;
        return;
    case 0:
        w_[tb_.var_list(0x447)[0]] = tb_.kLfoWord[std::min<std::uint16_t>(vp(0), 127)];
        push_wl(0x447, 1);
        return;
    case 1: case 2: {
        const auto u = static_cast<std::uint16_t>((vp(2) * 0x200u) / 0x7Fu);
        wr(0x9d) = wr(0x82) = u;
        wr(0x9e) = wr(0x83) = static_cast<std::uint16_t>(static_cast<int>(vp(1) * (0x200 - u)) / 0x7F);
        push_wl(0x44c, 2);
        push_wl(0x44a, 2);
        return;
    }
    case 3:
        wr(0x87) = fb_word(vp(3));
        push_wl(0x452, 1);
        return;
    case 5: case 6:
        filter(1, 2, b(vp(5)), b(vp(6)), 0, 0x438);
        filter(1, 2, b(vp(5)), b(vp(6)), 0, 0x440);
        return;
    case 7: case 8:
        filter(1, 3, b(vp(7)), b(vp(8)), 0, 0x43c);
        filter(1, 3, b(vp(7)), b(vp(8)), 0, 0x444);
        return;
    case 9:
        wr(0xa6) = wr(0xa2) = level(tb_, 128 - dw);
        wr(0xa7) = level(tb_, dw);
        push_wl(0x45c, 1);
        push_wl(0x460, 2);
        if (vp(10) < 9) {
            wr(0xa4) = level(tb_, dw);
            push_wl(0x45e, 1);
        } else {
            wr(0xa5) = level(tb_, dw);
            push_wl(0x45f, 1);
        }
        return;
    case 10: {
        // Stage count / output tap: {7b, 7e, 7c, 7d, 80} = stage-select words.
        std::uint16_t s7b = 0, s7e = 0, s7c = 0, s7d = 0, s80 = 0, a4 = 0, a5 = 0;
        const std::uint16_t v = vp(10);
        if (v <= 6) { s7b = 0x200; s7e = 0x200; a4 = level(tb_, dw); }
        else if (v == 7) { s7e = 0x200; s7c = 0x200; a4 = level(tb_, dw); }
        else if (v == 8) { s7e = 0x200; s7d = 0x200; a4 = level(tb_, dw); }
        else if (v == 9) { s7c = 0x200; s80 = 0x200; a5 = level(tb_, dw); }
        else { s7d = 0x200; s80 = 0x200; a5 = level(tb_, dw); }
        wr(0x7b) = s7b;
        wr(0x7e) = s7e;
        wr(0x7c) = s7c;
        wr(0x7d) = s7d;
        wr(0x80) = s80;
        wr(0xa4) = a4;
        wr(0xa5) = a5;
        push_wl(0x454, 4);
        push_wl(0x459, 1);
        push_wl(0x45e, 2);
        return;
    }
    default: return;
    }
}

void EffectControl::var_phaser2(std::uint8_t index) {
    const std::uint16_t dw = dry_wet();
    switch (index) {
    case kParamInit:
        for (std::uint8_t i : {0, 1, 3, 5, 7, 9, 10, 12}) var_phaser2(i);
        return;
    case 0: case 5: case 6: case 7: case 8: var_phaser1(index); return;
    case 1: case 2: {
        auto u = static_cast<std::uint16_t>((vp(2) * 0x200u) / 0x7Fu);
        wr(0x82) = u;
        wr(0x83) = static_cast<std::uint16_t>(static_cast<int>((0x200 - u) * vp(1)) / 0x7F);
        u = static_cast<std::uint16_t>((vp(2) * 0x1E0u) / 0x7Fu);
        wr(0x9d) = u;
        wr(0x9e) = static_cast<std::uint16_t>(static_cast<int>((0x200 - u) * vp(1)) / 0x7F);
        push_wl(0x44c, 2);
        push_wl(0x44a, 2);
        return;
    }
    case 3:
        wr(0xa1) = wr(0x86) = fb_word(vp(3));
        push_wl(0x450, 2);
        return;
    case 9:
        wr(0xa6) = wr(0xa2) = level(tb_, 128 - dw);
        wr(0xa7) = wr(0xa3) = level(tb_, dw);
        push_wl(0x45c, 2);
        push_wl(0x460, 2);
        return;
    case 10: {
        const std::uint16_t v = vp(10);
        const bool a = v < 4, c = v == 4, e = v > 4;
        wr(0x7e) = wr(0x7b) = a ? 0x200 : 0;
        wr(0x7f) = wr(0x7c) = c ? 0x200 : 0;
        wr(0x80) = wr(0x7d) = e ? 0x200 : 0;
        push_wl(0x454, 6);
        return;
    }
    case 12:
        w_[tb_.var_list(0x449)[0]] = phase_word(vp(12));
        push_wl(0x449, 1);
        return;
    default: return;
    }
}

} // namespace hvyxg::fx
