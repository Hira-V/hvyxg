// Copyright (C) 2026 Kevin H. Patterson
// SPDX-License-Identifier: BSD-3-Clause (see LICENSE)
#pragma once
// System-effect behaviour of the SXG-Create-NF patched syxg50.dll (`--enable-mu`,
// docs/ENGINE.md "MU banks"). Learned from SXG-Create-NF's BSD-3-Clause docs
// (docs/dll-patching.md) and the patch tables in its utility/dllpatch.py, and confirmed by
// comparing the patched plugin with the stock DLL.

#include <cstdint>
#include <utility>

#include "fx/fx_control.hpp"

namespace hvyxg::fx {

/// Effect types the DLL has no kernel for become the nearest type it has (reverb Canyon →
/// Tunnel; chorus Symphonic → Celeste 1, Phaser → Flanger 1, Ensemble Detune → Chorus 1;
/// variation Auto / Touch Wah + Dist / OD and the Dist / Comp / Wah + Delay chains →
/// Distortion / Overdrive, Touch Wah and Talking Modulator → Auto Wah, 2-Way Rotary →
/// Rotary, Ensemble Detune → Symphonic). Other types are returned unchanged.
std::pair<std::uint8_t, std::uint8_t> mu_effect_type(Block block, std::uint8_t msb, std::uint8_t lsb);

/// The return level (0..127) of the reverb or the system-connection variation, scaled by the
/// patched DLL's per-type factor (64 = ×1, measured by SXG-Create-NF on the S-MU2000) and
/// capped at 127: (level · factor + 32) >> 6.
std::uint8_t mu_return_level(Block block, std::uint8_t msb, std::uint8_t lsb, std::uint8_t level);

} // namespace hvyxg::fx
