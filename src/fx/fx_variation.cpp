// Copyright (C) 2026 Kevin H. Patterson
// SPDX-License-Identifier: BSD-3-Clause (see LICENSE)
#include "fx/fx_variation.hpp"

#include <algorithm>
#include <cmath>

#include "fx/fx_dsp.hpp"
#include "fx/fx_tables.hpp"

namespace hvyxg::fx {

namespace {

constexpr float kRefRate = 44100.0f;
constexpr std::size_t kRing = 131072;  // (0x80010 bytes)

std::size_t ring_size(float rate) {
    std::size_t n = kRing;
    while (static_cast<float>(n) < static_cast<float>(kRing) * rate / kRefRate) n <<= 1;
    return n;
}

std::int32_t ftol(double v) { return static_cast<std::int32_t>(v); }

} // namespace

VariationUnit::VariationUnit(float sample_rate, float bus_unit)
    : rate_(sample_rate), r_(sample_rate / kRefRate), inv_r_(kRefRate / sample_rate),
      depth_scale_(2048.0f * sample_rate / kRefRate), clip_hi_(131071.0f * bus_unit),
      clip_lo_(-131072.0f * bus_unit), ring_(ring_size(sample_rate) + 4),
      mask_(ring_size(sample_rate) - 1) {}

VariationUnit::Kernel VariationUnit::kernel_of(VariationType t) {
    // the type → its kernel.
    switch (t.category) {
    case 0:
        switch (t.type) {
        case 1: return Kernel::Thru;
        case 2: case 3: case 4: case 8: case 9: case 0x14: case 0x16: return Kernel::Chorus;
        case 5: case 6: case 7: case 0x15: return Kernel::Celeste;
        case 10: return Kernel::Symphonic;
        case 11: return Kernel::Rotary;
        case 12: case 13: return Kernel::Tremolo;
        case 14: case 15: case 16: case 17: return Kernel::Delay;
        case 19: return Kernel::Eq2;
        default: return Kernel::None;
        }
    case 1: return t.type == 1 || t.type == 2 ? Kernel::Phaser : Kernel::None;
    case 2:
        if (t.type >= 1 && t.type <= 4) return Kernel::Er;
        if (t.type >= 5 && t.type <= 7) return Kernel::Distortion;
        if (t.type == 8) return Kernel::Eq3;
        return Kernel::None;
    case 3:
        if (t.type >= 1 && t.type <= 8) return Kernel::Reverb;
        if (t.type == 9) return Kernel::AutoWah;
        if (t.type >= 10 && t.type <= 12) return Kernel::Karaoke;
        return Kernel::None;
    default: return Kernel::None;
    }
}

void VariationUnit::load(const EffectControl& ctrl) {
    type_ = ctrl.variation();
    kernel_ = kernel_of(type_);
    const std::uint8_t* fmt = &ctrl.tables().kVarCoefFormat[static_cast<std::size_t>(type_.category & 3) * 0xC0u];
    for (const std::uint8_t reg : ctrl.tables().kVarCoefList) {
        if (fmt[reg] == 0xFF) continue;
        p_[reg] = decode_word(ctrl.dsp_word(reg), fmt[reg]);
    }
    for (const std::uint8_t reg : ctrl.tables().kVarCoefList) derive(reg);
    // the addresses a category's kernels use, × max(1, fs / 44100), truncated.
    const double scale = std::max(1.0, static_cast<double>(r_));
    for (std::size_t i = 0x24; i <= 0x3E; ++i) {
        bool used = type_.category >= 2;
        if (type_.category == 0)
            used = i == 0x28 || i == 0x2d || i == 0x33 || i == 0x38 || i == 0x3a || i == 0x3b;
        if (used) addr_[i] = ftol(static_cast<double>(static_cast<std::uint16_t>(ctrl.dsp_addr(i))) * scale);
    }
}

void VariationUnit::derive(std::size_t reg) {
    // for the variation registers 0x6d..0xbf: the plain-C
    // kernels read lane 0 of each slot. Most slots take the decoded word as it is; the cases
    // below scale it, combine registers, or set the LFO integers instead.
    const float p = p_[reg];
    const double inv_r = inv_r_;
    const double dscale = depth_scale_;
    constexpr double k2p22 = 4194304.0;
    constexpr double k2p24 = 16777216.0;
    switch (type_.category) {
    case 0:
        switch (reg) {
        case 0x6d: case 0x70:
            lfo(0x248) = ftol(static_cast<double>(p_[0x77]) * p_[0x70] * p_[0x6d] * inv_r * k2p24);
            return;
        case 0x6e: lfo(0x24c) = ftol(p * k2p22); return;
        case 0x71: case 0x7b: case 0x8a: case 0x8f: {
            const float v = 1.0f - p * inv_r_ * 4.0f;
            k_[reg] = v < 0.0f ? 0.0f : v;
            return;
        }
        case 0x72: case 0x7c: case 0x8b: case 0x90: {
            const float v = p * inv_r_ * 4.0f;
            k_[reg] = v > 1.0f ? 1.0f : v;
            return;
        }
        case 0x74: lfo(0x250) = ftol(p * k2p22); k_[reg] = p; return;
        case 0x75: lfo(0x264) = ftol(p * dscale); k_[reg] = p * depth_scale_; return;
        case 0x76: case 0x86: case 0x96: k_[reg] = p * depth_scale_; return;
        case 0x77:
            lfo(0x244) = ftol(inv_r * p * k2p24);
            lfo(0x248) = ftol(static_cast<double>(p_[0x70]) * p_[0x6d] * inv_r * p * k2p24);
            return;
        case 0x79: lfo(0x254) = ftol(p * k2p22); return;
        case 0x84: lfo(0x258) = ftol(p * k2p22); k_[reg] = p; return;
        case 0x85: lfo(0x268) = ftol(p * dscale); k_[reg] = p * depth_scale_; return;
        case 0x88: lfo(0x25c) = ftol(p * k2p22); return;
        case 0x8d: lfo(0x260) = ftol(p * k2p22); return;
        case 0x95: lfo(0x26c) = ftol(p * dscale); k_[reg] = p * depth_scale_; return;
        case 0xa5: k_[reg] = p * depth_scale_; return;
        case 0x78: case 0x81: case 0x82: case 0x87: case 0x92: case 0x93: case 0x97:
        case 0xa2: case 0xa3: case 0xa6: case 0xb0: case 0xb1:
            return;  // not handled for this category: the slot keeps its value
        default: k_[reg] = p; return;
        }
    case 1:
        switch (reg) {
        case 0x79: lfo(0x270) = ftol(p * inv_r * k2p22); return;
        case 0x82: case 0x9d: k_[reg] = p * 8.0f; return;
        case 0x83: case 0x9e: k_[reg] = p * 1.9073486328125e-06f; return;
        case 0x9c: lfo(0x274) = ftol(p * k2p22); return;
        case 0x7a: case 0x81: case 0x88: case 0x8b: case 0x8c: case 0x8f: case 0x90:
        case 0x93: case 0x94: case 0x97: case 0x98: case 0x9b: case 0xa8: case 0xab:
        case 0xac: case 0xaf: case 0xb0: case 0xb3: case 0xb4: case 0xb7: case 0xb8:
        case 0xbb:
            return;
        default: k_[reg] = p; return;
        }
    case 2:
        switch (reg) {
        case 0x76: case 0x7a: case 0x7d: case 0x80:
            k_[0x76] = p_[0x80] * p_[0x7d] * p_[0x7a] * p_[0x76];
            return;
        case 0x75: case 0xae:
            return;
        default: k_[reg] = p; return;
        }
    default:  // 3
        switch (reg) {
        case 0x7e:
            lfo(0x278) = ftol(static_cast<double>(p_[0x7e]) * inv_r * k2p24);
            k_[0xbf] = inv_r_ * 0.00390625f;
            return;
        case 0x86: k_[reg] = p * 1.9073486328125e-06f; return;
        case 0x87: k_[reg] = p * 8.0f; return;
        case 0x7f: case 0x8c: case 0x8e: case 0x99: case 0x9b:
            return;
        default: k_[reg] = p; return;
        }
    }
}

void VariationUnit::clear() {
    std::fill(ring_.begin(), ring_.end(), 0.0f);
    pos_ = 0;
    eq_xl_ = eq_y1l_ = eq_y2l_ = eq_xr_ = eq_y1r_ = eq_y2r_ = 0;
    dly_t1_ = dly_t2_ = 0;
    mod_.clear();
    cat2_.clear();
    cat3_.clear();
}

void VariationUnit::process(float* l, float* r, std::size_t frames) {
    switch (kernel_) {
    case Kernel::None: case Kernel::Thru: break;
    case Kernel::Chorus: k_chorus(l, r, frames); break;
    case Kernel::Celeste: k_celeste(l, r, frames); break;
    case Kernel::Symphonic: k_symphonic(l, r, frames); break;
    case Kernel::Rotary: k_rotary(l, r, frames); break;
    case Kernel::Tremolo: k_tremolo(l, r, frames); break;
    case Kernel::Delay: k_delay(l, r, frames); break;
    case Kernel::Eq2: eq_input(l, r, frames); break;
    case Kernel::Phaser: k_phaser(l, r, frames); break;
    case Kernel::Er: k_er(l, r, frames); break;
    case Kernel::Distortion: k_distortion(l, r, frames); break;
    case Kernel::Eq3: k_eq3(l, r, frames); break;
    case Kernel::Reverb: k_reverb(l, r, frames); break;
    case Kernel::Karaoke: k_karaoke(l, r, frames); break;
    case Kernel::AutoWah: k_auto_wah(l, r, frames); break;
    }
}

void VariationUnit::eq_input(float* l, float* r, std::size_t n) {
    // Per channel a low shelf then a high shelf, in place (L 7e/7f/80, a7/a8/a9;
    // R 98/99/9a, 9b/9c/9d).
    for (std::size_t i = 0; i < n; ++i) {
        float x = l[i];
        float y1 = k(0x7e) * eq_xl_ + k(0x7f) * x + k(0x80) * eq_y1l_;
        float y2 = y1 * k(0xa8) + k(0xa7) * eq_y1l_ + k(0xa9) * eq_y2l_;
        eq_xl_ = x;
        eq_y1l_ = y1;
        eq_y2l_ = y2;
        l[i] = y2;
        x = r[i];
        y1 = k(0x99) * x + k(0x98) * eq_xr_ + k(0x9a) * eq_y1r_;
        y2 = y1 * k(0x9c) + k(0x9b) * eq_y1r_ + k(0x9d) * eq_y2r_;
        eq_xr_ = x;
        eq_y1r_ = y1;
        eq_y2r_ = y2;
        r[i] = y2;
    }
}

void VariationUnit::k_delay(float* l, float* r, std::size_t n) {
    // Delay L,C,R / L,R / Echo / Cross delay: four taps, two lines with damped feedback.
    eq_input(l, r, n);
    for (std::size_t i = 0; i < n; ++i) {
        const float t1 = ring(a(0x28));
        const float t2 = ring(a(0x2d));
        const float t3 = ring(a(0x33));
        const float t4 = ring(a(0x38));
        const float f5 = k(0x9e) * dly_t1_ + t1 * k(0x9f);
        dly_t1_ = t1;
        const float f6 = k(0xa0) * dly_t2_ + t2 * k(0xa1);
        dly_t2_ = t2;
        const float x_l = l[i];
        const float x_r = r[i];
        ring(a(0x3a)) = k(0xaa) * x_r + f5 * k(0xab) + f6 * k(0xac) + x_l;
        ring(a(0x3b)) = k(0xad) * x_r + f5 * k(0xae) + f6 * k(0xaf);
        l[i] = k(0xb3) * x_r + t1 * k(0xb4) + t3 * k(0xb5) + t4 * k(0xb6) + k(0xb2) * x_l;
        r[i] = k(0xb7) * x_l + k(0xb8) * x_r + t2 * k(0xb9) + t3 * k(0xba) + t4 * k(0xbb);
        pos_ = (pos_ - 1) & mask_;
    }
}

} // namespace hvyxg::fx
