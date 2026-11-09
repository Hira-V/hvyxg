// Copyright (C) 2026 Kevin H. Patterson
// SPDX-License-Identifier: BSD-3-Clause (see LICENSE)
#pragma once
// The effect DSP's variation micro-programs (shared by syxg50.dll and S-YXG2006LE.DLL):
// decodes the effect control's variation words / addresses and runs the DLLs' plain-C kernels with
// exact delay addressing (the DLLs' plain-C kernels).
//
// The unit works in place on the variation bus pair, after the input gain, as the DLL kernels
// do on bus 0x600 / 0x700; the post-mix (return, sends) belongs to the system effects.
//
// Kernel state is kept per family (fx_var_*.hpp) so that each family's kernels and effect control
// live in their own files.

#include <array>
#include <cstddef>
#include <cstdint>
#include <vector>

#include "buses.hpp"
#include "fx/fx_control.hpp"
#include "fx/fx_var_cat2.hpp"
#include "fx/fx_var_cat3.hpp"
#include "fx/fx_var_mod.hpp"

namespace hvyxg::fx {

class VariationUnit {
public:
    /// `bus_unit`: one DLL bus unit in the units the unit is fed (the system effects' bus
    /// contract, `EngineModel::distortion_clip_unit`; 1 for DLL units). Only the distortion's
    /// clip level (±2^17 DLL units) depends on it.
    explicit VariationUnit(float sample_rate, float bus_unit = 1.0f);

    /// Reload every coefficient and address of the current type from the effect control's DSP
    /// image (decode per the type's category, and derive the kernel values,
    void load(const EffectControl& ctrl);
    /// Clear the ring and the kernel state (type change).
    void clear();
    /// The type has a kernel (sets the "variation present" flag): false for
    /// {0, 0} (off) and unknown types.
    bool present() const { return kernel_ != Kernel::None; }
    /// In place on the variation send pair (after the input gains).
    void process(float* l, float* r, std::size_t frames);

private:
    enum class Kernel : std::uint8_t {
        None, Thru, Chorus, Celeste, Symphonic, Rotary, Tremolo, Delay, Eq2, Phaser,
        Er, Distortion, Eq3, Reverb, Karaoke, AutoWah,
    };
    static Kernel kernel_of(VariationType t);

    /// Derive the kernel values of coefficient register `reg` from `p_` (the
    /// lane-0 values the plain-C kernels read, and the LFO integers).
    void derive(std::size_t reg);

    // Kernels (the 2006LE scalar engine's; syxg50.dll's plain-C twins compute the same).
    void eq_input(float* l, float* r, std::size_t n);
    void k_delay(float* l, float* r, std::size_t n);
    void k_chorus(float* l, float* r, std::size_t n);
    void k_celeste(float* l, float* r, std::size_t n);
    void k_symphonic(float* l, float* r, std::size_t n);
    void k_rotary(float* l, float* r, std::size_t n);
    void k_tremolo(float* l, float* r, std::size_t n);
    void k_phaser(float* l, float* r, std::size_t n);
    void k_er(float* l, float* r, std::size_t n);
    void k_distortion(float* l, float* r, std::size_t n);
    void k_eq3(float* l, float* r, std::size_t n);
    void k_reverb(float* l, float* r, std::size_t n);
    void k_karaoke(float* l, float* r, std::size_t n);
    void k_auto_wah(float* l, float* r, std::size_t n);

    /// Lane 0 of coefficient slot `reg`, what the kernels read.
    float k(std::size_t reg) const { return k_[reg]; }
    /// Integer address `i` scaled to the output rate.
    std::int32_t a(std::size_t i) const { return addr_[i]; }
    /// The LFO / modulation integers derives.
    std::int32_t& lfo(unsigned engine_offset) { return lfo_[(engine_offset - 0x244u) / 4]; }
    /// Ring access relative to the write position: `ring(A)` = B[(A + p) & mask].
    float& ring(std::int32_t addr) {
        return ring_[(static_cast<std::size_t>(addr) + pos_) & mask_];
    }
    /// A linearly interpolated tap `whole + frac` samples past address `addr`, as the
    /// modulation kernels read it: x[i] + (x[i + 1] − x[i])·frac.
    float interp(std::int32_t addr, std::int32_t whole, float frac) const {
        const std::size_t i0 = (static_cast<std::size_t>(addr + whole) + pos_) & mask_;
        const float x0 = ring_[i0];
        return (ring_[(i0 + 1) & mask_] - x0) * frac + x0;
    }

    float rate_;
    float r_;           // fs / 44100
    float inv_r_;       // 44100 / fs
    float depth_scale_; // 2048 · fs / 44100
    float clip_hi_;     // 131071 DLL units
    float clip_lo_;     // −131072 DLL units
    std::vector<float> ring_;
    std::size_t mask_ = 0;
    std::size_t pos_ = 0;  // decrements
    VariationType type_{};
    Kernel kernel_ = Kernel::None;
    std::array<float, EffectControl::kWords> p_{};
    std::array<float, EffectControl::kWords> k_{};
    std::array<std::int32_t, EffectControl::kAddrs> addr_{};
    std::array<std::int32_t, 14> lfo_{};
    // Input EQ and delay FIR state.
    float eq_xl_ = 0, eq_y1l_ = 0, eq_y2l_ = 0, eq_xr_ = 0, eq_y1r_ = 0, eq_y2r_ = 0;
    float dly_t1_ = 0, dly_t2_ = 0;
    // Family state.
    VarModState mod_;
    VarCat2State cat2_;
    VarCat3State cat3_;
    // Scratch buses (DLL bus channels 12 / 13: the dry copies of categories 2 / 3).
    std::array<float, Buses::kMaxBlock> dry_l_{};
    std::array<float, Buses::kMaxBlock> dry_r_{};
};

} // namespace hvyxg::fx
