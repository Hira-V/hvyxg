// Copyright (C) 2026 Kevin H. Patterson
// SPDX-License-Identifier: BSD-3-Clause (see LICENSE)
#pragma once
// Kernel state of the variation category 2 (early reflections / gate / reverse, distortion /
// overdrive / amp simulator, 3-band EQ). Cleared on a type change. The 2006LE engine offsets
// are given for each field.

#include <cstdint>

namespace hvyxg::fx {

struct VarCat2State {
    // Pre stage: x[n−1], first and second section outputs.
    float pre_x = 0, pre_y1 = 0, pre_y2 = 0;
    // Post stage: first order x[n−1] / y, biquad history,
    // last first order.
    float post_x = 0;
    float bq_x1 = 0;    // first-order output, the biquad's x[n−1]
    float bq_x2 = 0;
    float bq_y1 = 0;
    float bq_y2 = 0;
    float out_y = 0;
    // Early reflections: damping filter x[n−1] / y.
    float er_x = 0, er_y = 0;

    void clear() { *this = VarCat2State{}; }
};

} // namespace hvyxg::fx
