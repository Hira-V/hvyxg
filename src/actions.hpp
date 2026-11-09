// Copyright (C) 2026 Kevin H. Patterson
// SPDX-License-Identifier: BSD-3-Clause (see LICENSE)
#pragma once
// What the front end tells the engine besides the RAM it keeps: note events, pedal changes
// and the part-wide releases. Applied by `Renderer` at block boundaries.

#include <cstdint>
#include <variant>

#include "engine_state.hpp"

namespace hvyxg::action {

/// Build the tone module and system effects over the front-end state.
struct Init {
    EngineState state;
};
struct NoteOn {
    const midi::Part* part;
    std::uint8_t note;
    std::uint8_t velocity;
};
struct NoteOff {
    const midi::Part* part;
    std::uint8_t note;
};
struct SustainChange {
    const midi::Part* part;
};
struct SostenutoChange {
    const midi::Part* part;
};
/// CC#84 portamento control: `key` is the source key of the next note; 0x80 clears the pending
/// keys (reset all controllers).
struct PortamentoControl {
    const midi::Part* part;
    std::uint8_t key;
};
/// All notes off: release every note of the part, pedals included.
struct ReleaseAll {
    const midi::Part* part;
};
/// Silence the part at once. `sound_off`: CC#120 all sound off, which only silences the
/// voices; otherwise also the part's note state (mono / poly, part mode,
/// variation connection / insertion-part writes).
struct KillAll {
    const midi::Part* part;
    bool sound_off = false;
};
/// The part's EG attack (`attack`) or release time (08 nn 1A / 1C, CC73 / CC72, NRPN) was set:
/// the module may update the voices already sounding.
struct EgTimeChange {
    const midi::Part* part;
    bool attack;
};
/// A reset (XG / GM / GM2 / GS On): after the parts are silenced, the engine's voice state
/// starts over (S-YXG50: register sets freed, free-voice FIFO 0, 1, …).
struct Reset {};

} // namespace hvyxg::action

namespace hvyxg {

using Action = std::variant<action::Init, action::NoteOn, action::NoteOff, action::SustainChange,
                            action::SostenutoChange, action::PortamentoControl, action::ReleaseAll,
                            action::KillAll, action::Reset, action::EgTimeChange>;

/// Receives the front end's actions.
class ActionSink {
public:
    virtual ~ActionSink() = default;
    virtual void act(const Action& a) = 0;
};

} // namespace hvyxg
