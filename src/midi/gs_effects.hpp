// Copyright (C) 2026 Kevin H. Patterson
// SPDX-License-Identifier: BSD-3-Clause (see LICENSE)
#pragma once
// GS reverb / chorus (Roland patch common 40 01 30..40) on the XG effect RAM, as each DLL
// implements them (docs/ENGINE.md "GS mode").
//
// Neither DLL has GS effect units: both translate the GS macros, levels and times onto the XG
// reverb (Room / Hall / Plate types) and the XG Chorus 1 type, through tables read from the
// DLLs (gs_effects.cpp). The two translations differ in details (reset image, parameter rows,
// type checks, which messages are accepted), so the dialect selects one.

#include <cstdint>

#include "midi/xg_ram.hpp"
#include "hvyxg/types.hpp"

namespace hvyxg::midi::gs {

/// The GS effect parameters the DLLs keep (GS patch common 40 01 30..40).
struct EffectState {
    std::uint8_t reverb_macro = 4;      // 30: Hall 2
    std::uint8_t reverb_character = 4;  // 31
    std::uint8_t reverb_pre_lpf = 0;    // 32
    std::uint8_t reverb_level = 64;     // 33
    std::uint8_t reverb_time = 64;      // 34
    std::uint8_t reverb_feedback = 0;   // 35 (stored, no effect in either DLL)
    std::uint8_t reverb_predelay = 0;   // 37 (stored, no effect)
    std::uint8_t chorus_macro = 2;      // 38: Chorus 3
    std::uint8_t chorus_pre_lpf = 0;    // 39 (stored, no effect)
    std::uint8_t chorus_level = 64;     // 3A
    std::uint8_t chorus_feedback = 8;   // 3B
    std::uint8_t chorus_delay = 80;     // 3C
    std::uint8_t chorus_rate = 3;       // 3D
    std::uint8_t chorus_depth = 19;     // 3E
    std::uint8_t chorus_send_reverb = 0;  // 3F
    std::uint8_t chorus_send_delay = 0;   // 40 (stored, no effect)
};

/// Which DLL's translation to follow.
enum class Dialect : std::uint8_t { Syxg50, Syxg2006le };

/// The GS reset: the GS effect state and the XG effect RAM after a GS reset. `ram` must already
/// hold the XG reset image.
void reset(EffectState& gs, xg::EffectData& ram, Dialect d);

/// One GS patch-common effect parameter (40 01 `addr`). `single` tells whether it came in a
/// one-parameter message (the 2006LE ignores the effect parameters in longer messages).
void write(EffectState& gs, xg::EffectData& ram, Dialect d, std::uint8_t addr, std::uint8_t value,
           bool single);

/// GS reverb time → XG reverb time (parameter 1) for a reverb character (0 Room 1 and 5 Plate
/// use the first table; both DLLs share the tables).
/// GS reverb time → XG reverb time for a character (the DLLs' table, from `tables`).
std::uint8_t reverb_time(const EngineTables& tables, std::uint8_t character, std::uint8_t time);

} // namespace hvyxg::midi::gs
