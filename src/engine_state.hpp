// Copyright (C) 2026 Kevin H. Patterson
// SPDX-License-Identifier: BSD-3-Clause (see LICENSE)
#pragma once
// The front-end state the engine reads at every block: pointers into the pinned front end
// (midi/front_end.hpp). Plain fields, no locks: front end and engine run on one thread.

#include <cstddef>
#include <cstdint>

#include "midi/xg_ram.hpp"
#include "hvyxg/types.hpp"

namespace hvyxg::midi {
struct Part;
}

namespace hvyxg {

using DrumSetupRam = std::array<midi::xg::DrumKitSetup, midi::xg::kDrumSetups>;

struct EngineState {
    /// XG system RAM (master volume, master tune, transpose). The universal real-time
    /// master volume (04 01) writes its MSB to the XG master volume, as both DLLs do.
    const midi::xg::System* system = nullptr;
    /// The mode of the last reset message.
    const ResetMode* reset_mode = nullptr;
    /// Drum setup RAM 3n (part mode n + 2 reads setup n).
    const DrumSetupRam* drum_setup = nullptr;
    /// XG effect RAM 02 01 and the insertion blocks 03 nn.
    const midi::xg::EffectData* effect1 = nullptr;
    const midi::xg::EffectInsertions* effect_insertion = nullptr;
    /// XG2.0 multi EQ 02 40 (rendered by the renderer on the final output).
    const midi::xg::MultiEq* multi_eq = nullptr;
    /// The parts, indexed by `Part::id`.
    const midi::Part* parts = nullptr;
    std::size_t part_count = 0;
    /// Counts what changes the state above: every message the front end takes, its resets and its
    /// all-notes-off. An engine may keep what it works out from that state while this stays the same.
    const std::uint32_t* events = nullptr;
};

/// The master volume the voices use, 0..127: the XG master volume, which the universal
/// real-time master volume also sets (MSB only, last message wins; both DLLs [meas]).
inline int effective_master_volume(const EngineState& s) {
    return s.system ? s.system->master_volume : 127;
}

} // namespace hvyxg
