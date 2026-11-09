// Copyright (C) 2026 Kevin H. Patterson
// SPDX-License-Identifier: BSD-3-Clause (see LICENSE)
#pragma once
// Kernel state of the variation modulation family (chorus, celeste, flanger, symphonic,
// rotary, tremolo / auto pan, phaser): the 2006LE engine fields the kernels keep between
// blocks. Cleared on a type change.

#include <array>
#include <cstdint>

namespace hvyxg::fx {

struct VarModState {
    std::uint32_t phase_a = 0;  // LFO A, 23-bit
    std::uint32_t phase_b = 0;  // LFO B
    // LFO smoothers.
    float s1 = 0, s2 = 0, s3 = 0, s4 = 0;
    // Phaser: input EQ and the two 5-stage all-pass chains
    // (element 0 = previous input).
    std::array<float, 6> eq{};
    std::array<float, 6> ap_l{};
    std::array<float, 6> ap_r{};

    void clear() { *this = VarModState{}; }
};

} // namespace hvyxg::fx
