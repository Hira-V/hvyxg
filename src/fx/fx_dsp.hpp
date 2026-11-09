// Copyright (C) 2026 Kevin H. Patterson
// SPDX-License-Identifier: BSD-3-Clause (see LICENSE)
#pragma once
// The effect DSP's reverb and chorus micro-programs (shared by syxg50.dll and
// S-YXG2006LE.DLL). Each unit decodes the effect control's coefficient words and delay addresses
// and runs the DLL's plain-C kernel with exact delay addressing:
//   reverb
//   chorus / celeste
// Addresses are 44.1 kHz sample counts; at other rates every address is scaled by
// fs / 44100 so that delays keep their length in milliseconds.

#include <array>
#include <cstddef>
#include <cstdint>
#include <vector>

#include "fx/fx_control.hpp"

namespace hvyxg::fx {

/// Decode a 10-bit coefficient word.
float decode_word(std::uint16_t word, std::uint8_t format);

class ReverbUnit {
public:
    explicit ReverbUnit(float sample_rate);

    /// Reload every coefficient and address from the effect control.
    void load(const EffectControl& ctrl);
    /// Clear the ring and filter state (type change).
    void clear();
    /// `io_l` holds the mono input (after the input gain) and receives the left output;
    /// `out_r` receives the right output.
    void process(float* io_l, float* out_r, std::size_t frames);

private:
    float c(std::size_t i) const { return coef_[i]; }
    std::size_t d(std::size_t i) const { return static_cast<std::size_t>(addr_[i]); }

    float rate_;
    std::vector<float> ring_;
    std::size_t mask_ = 0;
    std::size_t pos_ = 0;
    std::array<float, EffectControl::kWords> coef_{};
    std::array<std::int32_t, EffectControl::kAddrs> addr_{};
    float x1_ = 0, y1_ = 0, y2_ = 0;          // input HPF / LPF state
    std::array<float, 4> comb_prev_{};         // comb FIR previous sample
};

class ChorusUnit {
public:
    explicit ChorusUnit(float sample_rate);

    void load(const EffectControl& ctrl);
    /// Clear the ring, EQ state and LFO phase (type change).
    void clear();
    /// In place on the chorus send pair (after the input gains).
    void process(float* l, float* r, std::size_t frames);

private:
    float rate_;
    std::vector<float> ring_;
    std::size_t mask_ = 0;
    std::size_t pos_ = 0;
    std::array<float, EffectControl::kWords> coef_{};
    std::array<std::int32_t, EffectControl::kAddrs> addr_{};
    bool celeste_ = false;
    // LFO (23-bit phase, triangle), stepped once every 4 samples.
    std::uint32_t phase_ = 0;
    std::uint32_t inc_ = 0;
    std::uint32_t off_r_ = 0, off_3_ = 0;
    std::int64_t depth_l_ = 0, depth_r_ = 0, depth_3_ = 0;
    std::uint32_t lfo_count_ = 0;
    std::int64_t mod_l_ = 0, mod_r_ = 0, mod_3_ = 0;
    // Input EQ state: L / R × {x[-1], y1[-1], y2[-1]}.
    std::array<float, 6> eq_{};
};

} // namespace hvyxg::fx
