// Copyright (C) 2026 Kevin H. Patterson
// SPDX-License-Identifier: BSD-3-Clause (see LICENSE)
#pragma once
// Note-on parameter computation: the DLL's per-note "component objects". Everything here runs once per note-on (or note-off for the release
// terms); the results drive the DSP voice.

#include <array>
#include <cstdint>

#include "syxg2006le/bank_data.hpp"
#include "midi/xg_ram.hpp"
#include "syxg2006le/tbl/dll_tables.hpp"

namespace hvyxg::midi {
struct Part;
}

namespace hvyxg::syxg2006le {

/// The note event the components read (`ev`).
struct NoteEvent {
    std::uint8_t note = 60;      ///< ev[0]: MIDI note (scale tuning / per-key controller index)
    std::uint8_t pitch_key = 60; ///< ev[1]: group-tracked key + transpose; 60 for drum voices
    std::uint8_t cents = 0;      ///< ev[2]: key-tracking cents remainder
    std::uint8_t scale_key = 60; ///< ev[3]: played key + group transpose (key-curve index)
    std::uint8_t vel1 = 128;     ///< ev[4]: velocity + 1 after the part velocity sense (2..128)
    /// Index for velocity curves (ev[4] − 1).
    int vel_index() const { return vel1 - 1; }
};

/// What the components see of the voice being set up.
struct SetupContext {
    const tbl::dll::Tables* tb = nullptr;
    const BankData* bank = nullptr;
    const midi::Part* part = nullptr;
    const syxg2006le::tbl::Voice* voice = nullptr;
    const syxg2006le::tbl::Group* group = nullptr;
    const syxg2006le::tbl::Zone* zone = nullptr;
    const ElementData* element = nullptr;
    /// Drum-setup parameters of the note (drum voices only).
    const midi::xg::DrumSetup* drum = nullptr;
    NoteEvent ev;
    bool is_drum() const { return drum != nullptr; }
};

/// One envelope of the pitch or filter EG (levels in cents, rates 0..127, delay in 10 ms).
struct ModEgParams {
    bool active = false;
    int delay = 0;
    std::array<int, 3> rate{};
    /// L0 (start / initial level) .. L3 (sustain).
    std::array<int, 4> level{};
    int release_rate = 0;
    int release_level = 0;
    /// Filter EG: release rate / level with the damper down (block D [8], [0x14]).
    int release_rate_damper = 0;
    int release_level_damper = 0;
};

struct AmpEgParams {
    int attack = 0;
    int decay1 = 1;
    int decay2 = 1;
    int decay3 = 0;
    /// L0..L3 attenuations (L0 = L1 for an instant attack).
    std::array<int, 4> level{};
    /// Release inputs (the rate is chosen at note-off from the pedal).
    int release_up = 0;
    int release_down = 0;
    int rate_scaling = 0;
    int damper_mode = 0;
    /// Voice type 6 (MSB 118 / 119): the alternate amp-EG adjusters.
    bool type6 = false;
    /// The rates before the part's XG attack / decay adjusters, which an XG EG change re-applies to sounding voices. `decay3_base` 0 = hold.
    int attack_base = 0, decay1_base = 1, decay2_base = 1, decay3_base = 0;
    bool drum = false;
};

/// The XG attack (part 0x1A, CC73) applied to `attack_base` (and the type-6
/// cap), clamped 0..0x7e.
int xg_attack_rate(const tbl::dll::Tables& tb, const AmpEgParams& p, int xg_attack);
/// Decay 1 / 2 / 3 after the XG attack cap and the XG decay (CC75) rule; writes p.decay1..3.
void apply_xg_decay(AmpEgParams& p, int xg_attack, int xg_decay);

struct LfoParams {
    int wave = 6;
    bool key_sync = false;
    int speed = 509;      ///< speed value v (≈ Hz × 100)
    int phase_deg = 0;
    int amp_depth = 0;    ///< attenuation units
    int pitch_depth = 0;  ///< cents peak-to-peak
    int filter_depth = 0; ///< cents
    int delay = 0;        ///< 10 ms
    int fade = 0;         ///< 10 ms
    int fade_flags = 0;   ///< bit 2 pitch, bit 1 amp, bit 0 filter
    int invert = 0;       ///< bit 2 pitch, bit 1 amp, bit 0 filter
};

struct FilterParams {
    int type = 0;
    /// Cutoff in cents before the part / controller filter terms.
    int cutoff = 19200;
    /// Resonance before the part resonance (16..140).
    int resonance = 16;
};

/// The per-note parameters of one DSP voice.
struct VoiceParams {
    /// Pitch sum in cents without the controller pitch.
    int pitch_cents = 0;
    /// Level attenuation without the controller amplitude sum, 0..255.
    int level = 0;
    /// The level component's velocity term (read by the voice score).
    int velocity_level = 0;
    /// Pan 1..127, or 0 for random pan.
    int pan = 64;
    bool random_pan = false;
    /// Key-on delay in 10 ms units (element header).
    int key_on_delay = 0;
    ModEgParams peg;
    ModEgParams feg;
    AmpEgParams amp;
    LfoParams lfo;
    FilterParams filter;
    /// Drum send levels (drum setup bytes 5..7), 127 for melodic voices.
    int drum_reverb = 127, drum_chorus = 127, drum_variation = 127;
};

/// Velocity after the part velocity sense: ev[4] (velocity + 1, 2..128).
std::uint8_t sensed_velocity(const BankData& bank, const syxg2006le::tbl::Voice& voice,
                             const midi::Part& part, std::uint8_t velocity);

/// Run every note-on component for one matched zone.
VoiceParams setup_voice(const SetupContext& ctx);

/// The amp-EG release rate at note-off: `pedal` is the damper value 0..127.
int amp_release_rate(const tbl::dll::Tables& tb, const AmpEgParams& amp, const midi::Part& part, bool drum,
                     int pedal);

} // namespace hvyxg::syxg2006le
