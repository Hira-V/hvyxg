// Copyright (C) 2026 Kevin H. Patterson
// SPDX-License-Identifier: BSD-3-Clause (see LICENSE)
// Variation kernels, modulation family (the 2006LE scalar engine's,
// syxg50.dll's plain-C twins compute
// the same). The cat-0 LFOs step once every 4 samples (23-bit phase, triangle); the phaser
// LFO steps every sample.

#include "fx/fx_variation.hpp"

namespace hvyxg::fx {

namespace {

constexpr std::uint32_t kPhaseMask = 0x7FFFFF;
constexpr float k2m22 = 2.384185791015625e-07f;  // 2^-22

// Triangle of a 23-bit phase: phase XOR (bit 22 ? 0x7FFFFF : 0), 0 … 2^22 − 1.
std::int32_t tri(std::uint32_t ph) {
    return static_cast<std::int32_t>(ph ^ ((ph & 0x400000u) ? 0x7FFFFFu : 0u));
}

// A 64-bit delay modulation `tri · depth` split into whole samples (>> 22) and a fraction.
struct Mod {
    std::int32_t whole = 0;
    float frac = 0;
};
Mod split(std::int64_t m) {
    return {static_cast<std::int32_t>(m >> 22),
            static_cast<float>(static_cast<std::uint32_t>(m) & 0x3FFFFFu) * k2m22};
}
// A float modulation truncated toward zero (`_ftol`), the rest as the fraction.
Mod split(double m) {
    const auto w = static_cast<std::int32_t>(m);
    return {w, static_cast<float>(m - static_cast<double>(w))};
}

} // namespace

void VariationUnit::k_chorus(float* l, float* r, std::size_t n) {
    // Chorus 1-4 and flanger: one modulated tap per line, L + R·0xaa into line 1.
    eq_input(l, r, n);
    Mod ml, mr;
    for (std::size_t i = 0; i < n; ++i) {
        if ((i & 3) == 0) {
            const std::uint32_t ph = mod_.phase_a = (mod_.phase_a + static_cast<std::uint32_t>(lfo(0x244))) & kPhaseMask;
            ml = split(static_cast<std::int64_t>(tri(ph)) * lfo(0x264));
            mr = split(static_cast<std::int64_t>(tri((ph - static_cast<std::uint32_t>(lfo(0x254))) & kPhaseMask)) *
                       lfo(0x268));
        }
        const float tl = interp(a(0x28), ml.whole, ml.frac);
        const float tr = interp(a(0x2d), mr.whole, mr.frac);
        const float x_l = l[i], x_r = r[i];
        ring(a(0x3a)) = k(0xaa) * x_r + tl * k(0xab) + x_l;
        ring(a(0x3b)) = tr * k(0xaf) + x_r;
        l[i] = k(0xb2) * x_l + tl * k(0xb4);
        r[i] = k(0xb8) * x_r + tr * k(0xb9);
        pos_ = (pos_ - 1) & mask_;
    }
}

void VariationUnit::k_celeste(float* l, float* r, std::size_t n) {
    // Celeste: a third tap per line on a third LFO phase (±120°).
    eq_input(l, r, n);
    Mod ml, mr, m3;
    for (std::size_t i = 0; i < n; ++i) {
        if ((i & 3) == 0) {
            const std::uint32_t ph = mod_.phase_a = (mod_.phase_a + static_cast<std::uint32_t>(lfo(0x244))) & kPhaseMask;
            ml = split(static_cast<std::int64_t>(tri(ph)) * lfo(0x264));
            mr = split(static_cast<std::int64_t>(tri((ph - static_cast<std::uint32_t>(lfo(0x254))) & kPhaseMask)) *
                       lfo(0x268));
            m3 = split(static_cast<std::int64_t>(tri((ph - static_cast<std::uint32_t>(lfo(0x25c))) & kPhaseMask)) *
                       lfo(0x26c));
        }
        const float tl = interp(a(0x28), ml.whole, ml.frac);
        const float t3l = interp(a(0x33), m3.whole, m3.frac);
        const float tr = interp(a(0x2d), mr.whole, mr.frac);
        const float t3r = interp(a(0x38), m3.whole, m3.frac);
        const float x_l = l[i], x_r = r[i];
        ring(a(0x3a)) = tl * k(0xab) + k(0xaa) * x_r + x_l;
        ring(a(0x3b)) = tr * k(0xaf) + x_r;
        l[i] = k(0xb2) * x_l + tl * k(0xb4) + t3l * k(0xb5);
        r[i] = tr * k(0xb9) + t3r * k(0xbb) + k(0xb8) * x_r;
        pos_ = (pos_ - 1) & mask_;
    }
}

void VariationUnit::k_symphonic(float* l, float* r, std::size_t n) {
    // Symphonic: three taps, each modulated by a smoothed LFO A plus LFO B.
    eq_input(l, r, n);
    Mod ml, mr, m3;
    for (std::size_t i = 0; i < n; ++i) {
        if ((i & 3) == 0) {
            const std::uint32_t pa = mod_.phase_a = (mod_.phase_a + static_cast<std::uint32_t>(lfo(0x244))) & kPhaseMask;
            const std::uint32_t pb = mod_.phase_b = (mod_.phase_b + static_cast<std::uint32_t>(lfo(0x248))) & kPhaseMask;
            const auto ta = [&](unsigned off) {
                return static_cast<double>(tri((pa - static_cast<std::uint32_t>(lfo(off))) & kPhaseMask));
            };
            const auto tb = [&](unsigned off) {
                return static_cast<double>(tri((pb - static_cast<std::uint32_t>(lfo(off))) & kPhaseMask));
            };
            mod_.s1 = static_cast<float>(k(0x71) * static_cast<double>(mod_.s1) + (ta(0x24c) * k2m22 - k(0x6f)) * k(0x72));
            mod_.s2 = static_cast<float>(k(0x7b) * static_cast<double>(mod_.s2) + (ta(0x254) * k2m22 - k(0x7a)) * k(0x7c));
            const double s3 = k(0x8a) * static_cast<double>(mod_.s3) + (ta(0x25c) * k2m22 - k(0x89)) * k(0x8b);
            mod_.s3 = static_cast<float>(s3);
            ml = split((mod_.s1 - static_cast<double>(k(0x73))) * k(0x75) * 4.0 + tb(0x250) * k(0x76) * k2m22);
            mr = split(static_cast<double>(static_cast<float>(
                (mod_.s2 - static_cast<double>(k(0x7d))) * k(0x85) * 4.0 + tb(0x258) * k(0x86) * k2m22)));
            m3 = split(static_cast<double>(static_cast<float>(
                (s3 - k(0x8c)) * k(0x95) * 4.0 + tb(0x260) * k(0x96) * k2m22)));
        }
        const float tl = interp(a(0x28), ml.whole, ml.frac);
        const float tr = interp(a(0x2d), mr.whole, mr.frac);
        const float t3 = interp(a(0x33), m3.whole, m3.frac);
        const float x_l = l[i], x_r = r[i];
        ring(a(0x3a)) = k(0xaa) * x_r + x_l;
        ring(a(0x3b)) = k(0xad) * x_r;
        l[i] = k(0xb2) * x_l + tl * k(0xb4) + t3 * k(0xb5);
        r[i] = tr * k(0xb9) + t3 * k(0xba) + k(0xb8) * x_r;
        pos_ = (pos_ - 1) & mask_;
    }
}

void VariationUnit::k_rotary(float* l, float* r, std::size_t n) {
    // Rotary speaker: horn taps L / R (AM + PM on LFO A), a third AM + PM tap, and a rotor tap
    // on LFO B (twice LFO A's rate).
    eq_input(l, r, n);
    Mod m1, m2, m3, m4;
    double g_l = 0, g_r = 0;
    float g3 = 0;
    for (std::size_t i = 0; i < n; ++i) {
        if ((i & 3) == 0) {
            const std::uint32_t pa = mod_.phase_a = (mod_.phase_a + static_cast<std::uint32_t>(lfo(0x244))) & kPhaseMask;
            const std::uint32_t pb = mod_.phase_b = (mod_.phase_b + static_cast<std::uint32_t>(lfo(0x248))) & kPhaseMask;
            const auto ta = [&](unsigned off) {
                return static_cast<double>(tri((pa - static_cast<std::uint32_t>(lfo(off))) & kPhaseMask));
            };
            mod_.s1 = static_cast<float>((ta(0x24c) * k2m22 - k(0x6f)) * k(0x72) + k(0x71) * static_cast<double>(mod_.s1));
            mod_.s2 = static_cast<float>((ta(0x254) * k2m22 - k(0x7a)) * k(0x7c) + k(0x7b) * static_cast<double>(mod_.s2));
            mod_.s3 = static_cast<float>((ta(0x25c) * k2m22 - k(0x89)) * k(0x8b) + k(0x8a) * static_cast<double>(mod_.s3));
            const double s4 =
                (static_cast<double>(tri((pb - static_cast<std::uint32_t>(lfo(0x260))) & kPhaseMask)) * k2m22 - k(0x8e)) *
                    k(0x90) +
                k(0x8f) * static_cast<double>(mod_.s4);
            mod_.s4 = static_cast<float>(s4);
            const double u1 = (mod_.s1 - static_cast<double>(k(0x73))) * 4.0;
            const auto u2 = static_cast<float>((mod_.s2 - static_cast<double>(k(0x7d))) * 4.0);
            const auto u3 = static_cast<float>((mod_.s3 - static_cast<double>(k(0x8c))) * 4.0);
            m1 = split(static_cast<double>(static_cast<float>(u1 * k(0x75))));
            m2 = split(static_cast<double>(static_cast<float>(u2 * k(0x85))));
            m3 = split(static_cast<double>(static_cast<float>(u3 * k(0x95))));
            m4 = split(static_cast<double>(static_cast<float>((s4 - k(0x91)) * 4.0 * k(0xa5))));
            g_l = u1 * k(0x83) + 1.0;
            g_r = static_cast<double>(u2) * k(0x94) + 1.0;
            g3 = static_cast<float>(static_cast<double>(u3) * k(0xa4) + 1.0);
        }
        const double t1 = interp(a(0x28), m1.whole, m1.frac) * g_l;
        const double t2 = interp(a(0x2d), m2.whole, m2.frac) * g_r;
        const double t3 = static_cast<double>(interp(a(0x33), m3.whole, m3.frac)) * g3;
        const double t4 = interp(a(0x38), m4.whole, m4.frac);
        const float x_l = l[i], x_r = r[i];
        ring(a(0x3a)) = k(0xaa) * x_r + x_l;
        ring(a(0x3b)) = k(0xad) * x_r;
        l[i] = static_cast<float>(k(0xb2) * static_cast<double>(x_l) + t4 * k(0xb6) + t3 * k(0xb5) + t1 * k(0xb4));
        r[i] = static_cast<float>(k(0xb8) * static_cast<double>(x_r) + t2 * k(0xb9) + t3 * k(0xba) + t4 * k(0xbb));
        pos_ = (pos_ - 1) & mask_;
    }
}

void VariationUnit::k_tremolo(float* l, float* r, std::size_t n) {
    // Tremolo / auto pan: a clipped, smoothed triangle per side drives an amplitude gain and a
    // small delay modulation.
    eq_input(l, r, n);
    Mod ml, mr;
    float g_l = 0;
    double g_r = 0;
    const auto clip = [](double v) { return v > 1.0 ? 1.0 : (v < -1.0 ? -1.0 : v); };
    for (std::size_t i = 0; i < n; ++i) {
        if ((i & 3) == 0) {
            const std::uint32_t pa = mod_.phase_a = (mod_.phase_a + static_cast<std::uint32_t>(lfo(0x244))) & kPhaseMask;
            const auto ta = [&](unsigned off) {
                return static_cast<double>(tri((pa - static_cast<std::uint32_t>(lfo(off))) & kPhaseMask));
            };
            const double cl = clip(static_cast<float>(ta(0x24c) * k2m22 - k(0x6f)));
            const double cr = clip(static_cast<float>(ta(0x254) * k2m22 - k(0x7a)));
            mod_.s1 = static_cast<float>(k(0x71) * static_cast<double>(mod_.s1) + cl * k(0x72));
            const double s2 = k(0x7b) * static_cast<double>(mod_.s2) + cr * k(0x7c);
            mod_.s2 = static_cast<float>(s2);
            const double ul = clip(static_cast<float>((mod_.s1 - static_cast<double>(k(0x73))) * 4.0));
            const double ur = clip((s2 - k(0x7d)) * 4.0);
            ml = split(static_cast<double>(static_cast<float>(ul * k(0x75) - static_cast<double>(k(0x76)) * k(0x74))));
            mr = split(static_cast<double>(static_cast<float>(ur * k(0x85) - static_cast<double>(k(0x86)) * k(0x84))));
            g_l = static_cast<float>(ul * k(0x83) + 1.0);
            g_r = ur * k(0x94) + 1.0;
        }
        const float tl = interp(a(0x28), ml.whole, ml.frac);
        const float tr = interp(a(0x2d), mr.whole, mr.frac);
        const float x_l = l[i], x_r = r[i];
        ring(a(0x3a)) = k(0xaa) * x_r + x_l;
        ring(a(0x3b)) = k(0xad) * x_r;
        l[i] = tl * g_l * k(0xb4);
        r[i] = static_cast<float>(tr * g_r * k(0xb9));
        pos_ = (pos_ - 1) & mask_;
    }
}

void VariationUnit::k_phaser(float* l, float* r, std::size_t n) {
    // Phaser (category 1): input EQ, L/R matrix, two 5-stage first-order all-pass chains whose
    // coefficient follows a per-sample LFO on an exponential law, then the dry copies added.
    auto& e = mod_.eq;
    for (std::size_t i = 0; i < n; ++i) {
        float x = l[i];
        float y1 = k(0x6e) * x + k(0x6f) * e[1] + k(0x6d) * e[0];
        e[0] = x;
        float y2 = k(0x70) * e[1] + k(0x72) * e[2] + y1 * k(0x71);
        e[2] = y2;
        e[1] = y1;
        l[i] = y2;
        x = r[i];
        y1 = k(0x75) * e[4] + k(0x74) * x + k(0x73) * e[3];
        e[3] = x;
        y2 = k(0x76) * e[4] + k(0x78) * e[5] + y1 * k(0x77);
        e[5] = y2;
        e[4] = y1;
        r[i] = y2;
    }
    for (std::size_t i = 0; i < n; ++i) {
        dry_l_[i] = k(0xa2) * l[i];
        dry_r_[i] = k(0xa6) * r[i];
        l[i] = k(0x84) * l[i] + k(0x85) * r[i];
        r[i] = k(0x9f) * r[i];
    }
    auto& sl = mod_.ap_l;
    auto& sr = mod_.ap_r;
    const auto coef = [](double v) {
        // 1 − 2^⌊v⌋ · (1 + frac v) / 256 (the shift count is the low 5 bits, as the x86 shl).
        const auto w = static_cast<std::int32_t>(v);
        const auto p = static_cast<std::int32_t>(1u << (static_cast<std::uint32_t>(w) & 31u));
        return static_cast<float>(1.0 - static_cast<double>(p) * static_cast<float>((v - w) + 1.0) * 0.00390625);
    };
    // One 5-stage chain: y_k = c·(y_k[n−1] − y_{k−1}[n]) + y_{k−1}[n−1]; s[0] = previous input.
    const auto chain = [](std::array<float, 6>& s, double c, double x) {
        double prev_in = s[0];
        s[0] = static_cast<float>(x);
        double in = x;
        for (std::size_t k = 1; k <= 5; ++k) {
            const double y = c * s[k] + prev_in - in * c;
            prev_in = s[k];
            s[k] = static_cast<float>(y);
            in = y;
        }
    };
    for (std::size_t i = 0; i < n; ++i) {
        const double x_l = l[i];
        const double x_r = r[i];
        const auto fa = static_cast<float>(static_cast<double>(k(0x7d)) * sl[5] + static_cast<double>(k(0x7b)) * sl[3] +
                                           static_cast<double>(k(0x7c)) * sl[4]);
        const auto fb = static_cast<float>(static_cast<double>(k(0x80)) * sr[5] + static_cast<double>(k(0x7f)) * sr[4] +
                                           static_cast<double>(k(0x7e)) * sr[3]);
        l[i] = static_cast<float>(static_cast<double>(fa) * k(0xa3) + static_cast<double>(k(0xa5)) * sr[4] +
                                  static_cast<double>(k(0xa4)) * sr[2]);
        r[i] = fb * k(0xa7);
        const std::uint32_t ph = mod_.phase_a = (mod_.phase_a + static_cast<std::uint32_t>(lfo(0x270))) & kPhaseMask;
        const double x1 = static_cast<double>(tri(ph)) * k(0x83) + k(0x82);
        const double x2 =
            static_cast<double>(tri((ph - static_cast<std::uint32_t>(lfo(0x274))) & kPhaseMask)) * k(0x9e) + k(0x9d);
        const float c1 = coef(x1);
        const float c2 = coef(x2);
        chain(sl, c1, static_cast<double>(fb) * k(0x87) + static_cast<double>(fa) * k(0x86) + x_l);
        chain(sr, c2, static_cast<double>(fb) * k(0xa1) + static_cast<double>(fa) * k(0xa0) + x_r);
    }
    for (std::size_t i = 0; i < n; ++i) {
        l[i] += dry_l_[i];
        r[i] += dry_r_[i];
    }
}

} // namespace hvyxg::fx
