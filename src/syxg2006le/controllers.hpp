// Copyright (C) 2026 Kevin H. Patterson
// SPDX-License-Identifier: BSD-3-Clause (see LICENSE)
#pragma once
// The part controller block: six modulation
// sources (pitch bend, poly / channel aftertouch, mod wheel, AC1, AC2), each contributing to
// amplitude, filter, pitch and the LFO pitch / filter / amplitude depths through the part's
// XG depth parameters, plus volume / expression / master volume and the RPN / master tuning.

#include <array>
#include <cstdint>

#include "engine_state.hpp"
#include "midi/xg_ram.hpp"
#include "syxg2006le/tbl/dll_tables.hpp"

namespace hvyxg::midi {
struct Part;
}

namespace hvyxg::syxg2006le {

struct ControllerSums {
    /// Slot 0: attenuation units 0..0x101 (volume, expression, master volume, Σ amp).
    int amp = 0;
    /// Slot 1: filter offset in cents, ±4800 (includes the XG part cutoff).
    int filter = 0;
    /// Slot 2: pitch offset in cents, ±9600 (bend, controllers, RPN tuning, master tune).
    int pitch = 0;
    /// Slots 3 / 4 / 5: LFO pitch depth (cents p-p), filter depth (cents), amp depth (units).
    int pmod = 0, fmod = 0, amod = 0;
};

/// The modulation sources whose contributions the DLL applies on its 12 ms controller timer
/// (only flags them; flushes them): pitch bend, channel
/// aftertouch, mod wheel, AC1, AC2.
struct ControllerSources {
    std::uint16_t bend = 0x2000;
    std::uint8_t cat = 0;
    std::uint8_t mw = 0;
    std::array<std::uint8_t, 2> ac{};
};

/// The current source values of `part`.
ControllerSources controller_sources(const midi::Part& part);

/// The controller sums of `part` for a note on MIDI key `key` (poly aftertouch is per key),
/// with the timer-latched `src`. Drum voices get the part note shift (as cents) instead of
/// the master tune. `insertion`: the part is the variation insertion part, whose volume,
/// expression and master volume apply after the effect (superset routing, as syxg50).
/// `master256`: the master volume on the 0..256 scale of the level law (`master256()`), or
/// −1 for the XG master volume of `system` alone.
ControllerSums controller_sums(const tbl::dll::Tables& tb, const midi::Part& part, const ControllerSources& src,
                               const midi::xg::System& system, std::uint8_t key, bool drum,
                               bool insertion = false, int master256 = -1);

/// The master volume of the level law, 0..256: V[XG master volume] (2·v, 127 → 256). The
/// universal real-time master volume sets the XG master volume to its MSB (measured over the
/// whole 14-bit range: LSB ignored, last message wins).
int master256(const EngineState& state);

} // namespace hvyxg::syxg2006le
