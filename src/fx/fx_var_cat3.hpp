// Copyright (C) 2026 Kevin H. Patterson
// SPDX-License-Identifier: BSD-3-Clause (see LICENSE)
#pragma once
// Kernel state of the variation category 3 (hall / room / stage / plate reverbs, auto wah,
// karaoke): the 2006LE engine fields the kernels keep between blocks. Cleared on a type
// change.

#include <array>
#include <cstdint>

namespace hvyxg::fx {

struct VarCat3State {
    // Input filter of the mono reverb / karaoke path (x[-1], y1, y2), which
    // the auto wah uses as its R input EQ.
    float x1 = 0, y1 = 0, y2 = 0;
    // Auto wah L input EQ.
    float lx1 = 0, ly1 = 0, ly2 = 0;
    // Comb FIR previous samples.
    std::array<float, 4> comb{};
    // Auto wah band-pass / low-pass integrators L, R, LFO
    // phase and the cutoff held between LFO steps.
    float bp_l = 0, lp_l = 0, bp_r = 0, lp_r = 0;
    std::uint32_t phase = 0;

    void clear() { *this = VarCat3State{}; }
};

} // namespace hvyxg::fx
