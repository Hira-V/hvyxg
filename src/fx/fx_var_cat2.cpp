// Copyright (C) 2026 Kevin H. Patterson
// SPDX-License-Identifier: BSD-3-Clause (see LICENSE)
// Variation kernels, category 2 (2006LE scalar engine; syxg50 plain-C twins). Every type runs
// a mono path: the pre stage keeps dry copies of L / R and mixes `c6d·L + c6e·R` through two
// first-order sections; the result goes to both outputs (`L = c·y + dry L`, `R = c'·y + dry R`).

#include <algorithm>

#include "fx/fx_variation.hpp"

namespace hvyxg::fx {

namespace {

// dry copies, mono mix and the two pre filters, in place on `l`.
void cat2_pre(VarCat2State& s, const float* k, float* l, const float* r, float* dry_l, float* dry_r,
              std::size_t n) {
    for (std::size_t i = 0; i < n; ++i) {
        dry_l[i] = k[0xad] * l[i];
        dry_r[i] = k[0xb1] * r[i];
        l[i] = k[0x6e] * r[i] + k[0x6d] * l[i];
    }
    for (std::size_t i = 0; i < n; ++i) {
        const float x = l[i];
        const float y1 = k[0x6f] * s.pre_x + k[0x70] * x + k[0x71] * s.pre_y1;
        s.pre_x = x;
        const float y2 = y1 * k[0x73] + k[0x72] * s.pre_y1 + k[0x74] * s.pre_y2;
        s.pre_y2 = y2;
        l[i] = y2;
        s.pre_y1 = y1;
    }
}

// first order → biquad → first order on `l`, then the outputs
// R = cb3·y + dry R, L = cb0·y + dry L.
void cat2_post(VarCat2State& s, const float* k, float* l, float* r, const float* dry_l,
               const float* dry_r, std::size_t n) {
    for (std::size_t i = 0; i < n; ++i) {
        const float x = l[i];
        const float f1 = k[0xa4] * s.bq_x1 + k[0xa2] * s.post_x + k[0xa3] * x;
        s.post_x = x;
        const float x2 = s.bq_x2;
        const float y2 = s.bq_y2;
        s.bq_y2 = s.bq_y1;
        s.bq_x2 = s.bq_x1;
        const float bq = k[0xa5] * y2 + k[0xa6] * s.bq_y1 + k[0xa7] * x2 + k[0xa8] * s.bq_x1 + f1 * k[0xa9];
        s.bq_x1 = f1;
        const float y = k[0xaa] * s.bq_y1 + bq * k[0xab] + k[0xac] * s.out_y;
        s.out_y = y;
        l[i] = y;
        s.bq_y1 = bq;
    }
    for (std::size_t i = 0; i < n; ++i) {
        r[i] = k[0xb3] * l[i] + dry_r[i];
        l[i] = k[0xb0] * l[i] + dry_l[i];
    }
}

} // namespace

void VariationUnit::k_er(float* l, float* r, std::size_t n) {
    // Early reflections / gate / reverse: two input all-passes (A24/A2a, A28/A2c), a damped
    // feedback tap (A25 → A2d), 7 + 7 taps cross-coupled through A3d / A3e, one output
    // all-pass per side (A29/A2e, A2b/A34).
    cat2_pre(cat2_, k_.data(), l, r, dry_l_.data(), dry_r_.data(), n);
    auto& s = cat2_;
    for (std::size_t i = 0; i < n; ++i) {
        const float x = l[i];
        ring(a(0x2a)) = k(0x7c) * x + ring(a(0x24)) * k(0x7b);
        const float f7 = k(0x7f) * x + ring(a(0x24)) * k(0x7e);
        ring(a(0x2c)) = f7 * k(0x82) + ring(a(0x28)) * k(0x81);
        const float u28 = ring(a(0x28));
        const float u25 = ring(a(0x25));
        s.er_y = k(0x77) * s.er_x + k(0x78) * u25 + k(0x79) * s.er_y;
        s.er_x = u25;
        ring(a(0x2d)) = k(0x86) * s.er_y + f7 * k(0x85) + u28 * k(0x84);
        const float ta = ring(a(0x25)) * k(0x99) + ring(a(0x32)) * k(0x9f) + ring(a(0x26)) * k(0x9a) +
                         ring(a(0x27)) * k(0x9b) + ring(a(0x2f)) * k(0x9c) + ring(a(0x30)) * k(0x9d) +
                         ring(a(0x31)) * k(0x9e);
        const float tb = ring(a(0x3b)) * k(0xba) + ring(a(0x35)) * k(0xb4) + ring(a(0x36)) * k(0xb5) +
                         ring(a(0x37)) * k(0xb6) + ring(a(0x38)) * k(0xb7) + ring(a(0x39)) * k(0xb8) +
                         ring(a(0x3a)) * k(0xb9);
        ring(a(0x3d)) = ta;
        const float sl = ring(a(0x3c)) * k(0xbb) + tb;
        ring(a(0x3e)) = tb;
        const float sr = ring(a(0x33)) * k(0xa0) + ta;
        const float u29 = ring(a(0x29));
        ring(a(0x2e)) = k(0x89) * sl + u29 * k(0x88);
        l[i] = k(0x8c) * sl + ring(a(0x29)) * k(0x8b);
        const float u2b = ring(a(0x2b));
        ring(a(0x34)) = k(0x8f) * sr + u2b * k(0x8e);
        r[i] = k(0x98) * sr + ring(a(0x2b)) * k(0x97);
        pos_ = (pos_ - 1) & mask_;
    }
    for (std::size_t i = 0; i < n; ++i) {
        l[i] = k(0xaf) * l[i] + dry_l_[i];
        r[i] = k(0xb2) * r[i] + dry_r_[i];
    }
}

void VariationUnit::k_distortion(float* l, float* r, std::size_t n) {
    // Distortion / overdrive / amp simulator: a six-stage cascade of hard clips at ±2^17 DLL units
    // (K76 = drive² · normalisation · 4), tapped and summed, between the pre and post filters.
    cat2_pre(cat2_, k_.data(), l, r, dry_l_.data(), dry_r_.data(), n);
    const float hi = clip_hi_, lo = clip_lo_;
    const auto clip = [hi, lo](float v) { return v > hi ? hi : (v < lo ? lo : v); };
    for (std::size_t i = 0; i < n; ++i) {
        const float x1 = clip(k(0x76) * l[i]);
        const float x2 = clip(x1 * k(0x83));
        const float x3 = clip(x2 * k(0x87));
        const float x4 = clip(x3 * k(0x8a));
        const float x5 = clip(x4 * k(0x8d));
        const float x6 = clip(x5 * k(0x90));
        l[i] = (x6 * k(0x96) + x5 * k(0x95) + x4 * k(0x94) + x3 * k(0x93) + x2 * k(0x92) + x1 * k(0x91)) *
               k(0xa1);
    }
    cat2_post(cat2_, k_.data(), l, r, dry_l_.data(), dry_r_.data(), n);
}

void VariationUnit::k_eq3(float* l, float* r, std::size_t n) {
    // 3-band EQ: the pre and post stages only.
    cat2_pre(cat2_, k_.data(), l, r, dry_l_.data(), dry_r_.data(), n);
    cat2_post(cat2_, k_.data(), l, r, dry_l_.data(), dry_r_.data(), n);
}

} // namespace hvyxg::fx
