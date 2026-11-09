// Copyright (C) 2026 Kevin H. Patterson
// SPDX-License-Identifier: BSD-3-Clause (see LICENSE)
// Variation kernels, category 3 (2006LE scalar engine; syxg50 plain-C twins). Each read of
// the ring happens where the DLL reads it (some taps are read again after a write).

#include <cstdint>

#include "fx/fx_variation.hpp"

namespace hvyxg::fx {

namespace {

// Dry copies (bus 0xc00 / 0xd00) and the mono input L = c6d·L + c6e·R, then the input
// HPF / LPF (6f..74) in place on L (the reverb and karaoke kernels' first two loops).
void mono_input(float* l, const float* r, float* dry_l, float* dry_r, std::size_t n, float kb9,
                float kac, float k6d, float k6e) {
    for (std::size_t i = 0; i < n; ++i) {
        dry_l[i] = kb9 * l[i];
        dry_r[i] = kac * r[i];
        l[i] = k6d * l[i] + k6e * r[i];
    }
}

} // namespace

void VariationUnit::k_reverb(float* l, float* r, std::size_t n) {
    VarCat3State& s = cat3_;
    mono_input(l, r, dry_l_.data(), dry_r_.data(), n, k(0xb9), k(0xac), k(0x6d), k(0x6e));
    for (std::size_t i = 0; i < n; ++i) {
        const float x = l[i];
        const float y1 = k(0x71) * s.y1 + k(0x6f) * s.x1 + k(0x70) * x;
        const float y2 = k(0x72) * s.y1 + k(0x74) * s.y2 + y1 * k(0x73);
        s.x1 = x;
        s.y1 = y1;
        s.y2 = y2;
        l[i] = y2;
    }
    for (std::size_t i = 0; i < n; ++i) {
        // Pre-delay with feedback, two all-passes.
        ring(a(0x27)) = ring(a(0x24)) * k(0x75) + l[i];
        ring(a(0x28)) = ring(a(0x24)) * k(0x77) + ring(a(0x25)) * k(0x76);
        float f = ring(a(0x24)) * k(0x79) + ring(a(0x25)) * k(0x78);
        ring(a(0x29)) = f * k(0x7b) + ring(a(0x26)) * k(0x7a);
        f = f * k(0x7d) + ring(a(0x26)) * k(0x7c);
        // Four combs with a two-tap FIR in the loop.
        float u = ring(a(0x34));
        ring(a(0x38)) = k(0xa5) * s.comb[0] + f * k(0xa7) + u * k(0xa6);
        s.comb[0] = ring(a(0x34));
        u = ring(a(0x35));
        ring(a(0x3a)) = k(0xa8) * s.comb[1] + f * k(0xaa) + u * k(0xa9);
        s.comb[1] = ring(a(0x35));
        u = ring(a(0x39));
        ring(a(0x3d)) = k(0xb3) * s.comb[2] + f * k(0xb5) + u * k(0xb4);
        s.comb[2] = ring(a(0x39));
        u = ring(a(0x3b));
        ring(a(0x3e)) = k(0xb6) * s.comb[3] + f * k(0xb8) + u * k(0xb7);
        s.comb[3] = ring(a(0x3b));
        // Early tap plus four comb taps per side, then one output all-pass per side.
        const float el = ring(a(0x2a)) * k(0x93) +
                         (ring(a(0x2d)) + ring(a(0x2c)) + ring(a(0x2b)) + ring(a(0x2e))) * k(0x8f);
        ring(a(0x3c)) = el * k(0xb0) + ring(a(0x37)) * k(0xaf);
        l[i] = el * k(0xb2) + ring(a(0x37)) * k(0xb1);
        const float er = ring(a(0x2a)) * k(0xa0) +
                         (ring(a(0x32)) + ring(a(0x31)) + ring(a(0x30)) + ring(a(0x2f))) * k(0x9c);
        ring(a(0x36)) = er * k(0xa2) + ring(a(0x33)) * k(0xa1);
        r[i] = er * k(0xa4) + ring(a(0x33)) * k(0xa3);
        pos_ = (pos_ - 1) & mask_;
    }
    for (std::size_t i = 0; i < n; ++i) {
        l[i] = k(0xbb) * l[i] + dry_l_[i];
        r[i] = k(0xae) * r[i] + dry_r_[i];
    }
}

void VariationUnit::k_karaoke(float* l, float* r, std::size_t n) {
    VarCat3State& s = cat3_;
    mono_input(l, r, dry_l_.data(), dry_r_.data(), n, k(0xb9), k(0xac), k(0x6d), k(0x6e));
    for (std::size_t i = 0; i < n; ++i) {
        const float x = l[i];
        const float y1 = k(0x71) * s.y1 + k(0x6f) * s.x1 + k(0x70) * x;
        const float y2 = k(0x72) * s.y1 + k(0x74) * s.y2 + y1 * k(0x73);
        s.x1 = x;
        s.y1 = y1;
        s.y2 = y2;
        l[i] = y2;
    }
    for (std::size_t i = 0; i < n; ++i) {
        const float x = l[i];
        ring(a(0x28)) = k(0x77) * x + ring(a(0x25)) * k(0x76);
        const float f = k(0x79) * x + ring(a(0x25)) * k(0x78);
        ring(a(0x29)) = f * k(0x7b) + ring(a(0x26)) * k(0x7a);
        ring(a(0x38)) = k(0xa5) * s.comb[0] + (f * k(0x7d) + ring(a(0x26)) * k(0x7c)) * k(0xa7) +
                        ring(a(0x34)) * k(0xa6);
        s.comb[0] = ring(a(0x34));
        const float tl = ring(a(0x2b)) * k(0x8f);
        ring(a(0x3c)) = tl * k(0xb0) + ring(a(0x37)) * k(0xaf);
        l[i] = tl * k(0xb2) + ring(a(0x37)) * k(0xb1);
        const float tr = ring(a(0x2f)) * k(0x9c);
        ring(a(0x36)) = tr * k(0xa2) + ring(a(0x33)) * k(0xa1);
        r[i] = tr * k(0xa4) + ring(a(0x33)) * k(0xa3);
        pos_ = (pos_ - 1) & mask_;
    }
    for (std::size_t i = 0; i < n; ++i) {
        l[i] = k(0xbb) * l[i] + dry_l_[i];
        r[i] = k(0xae) * r[i] + dry_r_[i];
    }
}

void VariationUnit::k_auto_wah(float* l, float* r, std::size_t n) {
    VarCat3State& s = cat3_;
    // Input EQ per channel (L 80..85, R 6f..74).
    for (std::size_t i = 0; i < n; ++i) {
        float x = l[i];
        float y1 = k(0x81) * x + k(0x82) * s.ly1 + k(0x80) * s.lx1;
        float y2 = k(0x83) * s.ly1 + k(0x85) * s.ly2 + y1 * k(0x84);
        s.lx1 = x;
        s.ly1 = y1;
        s.ly2 = y2;
        l[i] = y2;
        x = r[i];
        y1 = k(0x71) * s.y1 + k(0x70) * x + k(0x6f) * s.x1;
        y2 = k(0x72) * s.y1 + k(0x74) * s.y2 + y1 * k(0x73);
        s.x1 = x;
        s.y1 = y1;
        s.y2 = y2;
        r[i] = y2;
    }
    // Dry copies; the filter inputs are c88·L and c94·L + c95·R.
    for (std::size_t i = 0; i < n; ++i) {
        dry_l_[i] = k(0xb9) * l[i];
        dry_r_[i] = k(0xab) * r[i];
        r[i] = k(0x94) * l[i] + k(0x95) * r[i];
        l[i] = k(0x88) * l[i];
    }
    // LFO every 4 samples: v = tri·c86 − c87, f = 2^⌊v⌋·cbf·(1 + frac v), clamped to ±1
    // (x87 extended precision in the DLL; double here).
    const std::int32_t inc = lfo(0x278);
    double f = 0.0;
    for (std::size_t i = 0; i < n; ++i) {
        if ((i & 3) == 0) {
            s.phase = (s.phase + static_cast<std::uint32_t>(inc)) & 0x7FFFFFu;
            const std::uint32_t tri = s.phase ^ ((s.phase & 0x400000u) ? 0x7FFFFFu : 0u);
            const double v = static_cast<double>(static_cast<std::int32_t>(tri)) * k(0x86) - k(0x87);
            const auto e = static_cast<std::int32_t>(v);
            const auto pow2 = static_cast<std::int32_t>(1u << (static_cast<std::uint32_t>(e) & 31u));
            f = static_cast<double>(pow2) * k(0xbf) * ((v - e) + 1.0);
            if (f > 1.0) f = 1.0;
            else if (f < -1.0) f = -1.0;
        }
        const auto bl = static_cast<float>(k(0x8b) * static_cast<double>(s.bp_l) +
                                           (k(0x8a) * static_cast<double>(s.lp_l) +
                                            k(0x89) * static_cast<double>(s.bp_l) + l[i]) * f);
        s.bp_l = bl;
        l[i] = bl;
        s.lp_l = static_cast<float>(f * bl + k(0x8d) * static_cast<double>(s.lp_l));
        const auto br = static_cast<float>(k(0x98) * static_cast<double>(s.bp_r) +
                                           (k(0x96) * static_cast<double>(s.bp_r) +
                                            k(0x97) * static_cast<double>(s.lp_r) + r[i]) * f);
        s.bp_r = br;
        r[i] = br;
        s.lp_r = static_cast<float>(f * br + k(0x9a) * static_cast<double>(s.lp_r));
    }
    for (std::size_t i = 0; i < n; ++i) {
        l[i] = k(0xba) * l[i] + dry_l_[i];
        r[i] = k(0xad) * r[i] + dry_r_[i];
    }
}

} // namespace hvyxg::fx
