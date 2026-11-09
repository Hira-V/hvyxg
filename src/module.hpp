// Copyright (C) 2026 Kevin H. Patterson
// SPDX-License-Identifier: BSD-3-Clause (see LICENSE)
#pragma once
// The two halves of a sound engine, one implementation per Yamaha engine model:
//
// - `ToneModule`: the voices. Turns note events + the parts' RAM / controller state into
//   samples on the mix buses, one fixed-size block at a time.
// - `SystemEffects`: reverb / chorus / variation and the mixer that returns them into the
//   output.
//
// Both are created by a `Bank` (bank.hpp) and driven by `Renderer` (renderer.hpp), all on
// one thread. MIDI events reach them only between blocks, as the DLLs apply events at
// block boundaries.

#include <cstddef>
#include <cstdint>

#include "buses.hpp"
#include "engine_state.hpp"
#include "hvyxg/types.hpp"

namespace hvyxg::midi {
struct Part;
}

namespace hvyxg {

struct EngineTables;

/// What a module needs from the outside world. Every pointer stays valid for the module's
/// life (they point into the pinned front end, midi/front_end.hpp).
struct ModuleContext {
    /// XG system / effect / drum-setup RAM, the GM master volume and the reset mode.
    EngineState shared;
    /// Output sample rate.
    float sample_rate = 44100.0f;
    /// Voices (register sets / DSP voices) of the tone module.
    std::uint16_t max_polyphony = 64;
    /// Sample interpolation. `Linear` is what both DLLs do.
    Interpolation interpolation = Interpolation::Linear;
    /// Superset: fade-out time of a stolen voice in ms; 0 = the DLL's fast steal release
    /// (`Synth::Options::steal_fade_ms`).
    float steal_fade_ms = 0.0f;
    /// No superset behaviour (`Synth::Options::dll_exact`): the part / drum EQ and HPF, the
    /// XG2.0 insertion chains and the 2006LE's audible variation are off.
    bool dll_exact = false;
    /// Superset option: the part / drum-note EQ with the MU2000's gain strength
    /// (`Synth::Options::mu_part_eq`).
    bool mu_part_eq = false;
    /// Superset option: the S-YXG2006LE plays full XG instead of its XGlite subset
    /// (`Synth::Options::full_xg_2006le`; only set for that bank, never with `dll_exact`).
    bool full_xg = false;
    /// The bank's engine tables (hvyxg/engine_tables.hpp); never null for a bank's modules.
    const EngineTables* tables = nullptr;
};

class ToneModule {
public:
    virtual ~ToneModule() = default;

    /// Samples per render block (the control / EG tick of the modelled engine).
    virtual std::size_t block_size() const = 0;

    /// A note-on for `part` (velocity ≥ 1). The part's selected program is `part.program`;
    /// its RAM (`*part.ram`), controller values and pitch bend are live and may be read at
    /// any later block too (`part` outlives the module).
    virtual void note_on(const midi::Part& part, std::uint8_t note, std::uint8_t velocity) = 0;
    /// The matching note-off. Sent even while the sustain pedal is down: holding notes for
    /// the pedals is the module's job (the engines release differently, e.g. the 2006LE's
    /// damper-dependent release rate).
    virtual void note_off(const midi::Part& part, std::uint8_t note) = 0;
    /// CC#64 / CC#66 changed (`part.controller` holds the new state).
    virtual void sustain_changed(const midi::Part& part) = 0;
    virtual void sostenuto_changed(const midi::Part& part) = 0;
    /// CC#84 portamento control (`key` 0..127), or 0x80 when reset all controllers clears it.
    /// The modules that do not track it ignore it (`part.controller.portamento_control`
    /// holds the last value).
    virtual void portamento_control(const midi::Part& part, std::uint8_t key) {
        (void)part;
        (void)key;
    }
    /// All notes off (CC#123 etc.): release every note of the part, pedals included.
    virtual void release_all(const midi::Part& part) = 0;
    /// Silence the part's voices at once and clear its note state (mono key list, portamento
    /// source): mono / poly and part-mode changes, variation insertion writes, resets.
    virtual void kill_all(const midi::Part& part) = 0;
    /// CC#120 all sound off: silence the part's voices at once (by default as `kill_all`).
    virtual void sound_off(const midi::Part& part) { kill_all(part); }
    /// A reset, after every part was silenced: the voice allocation starts over.
    virtual void reset() {}
    /// The part's EG attack (`attack`) or release time changed (`part.ram`).
    virtual void eg_time_changed(const midi::Part& part, bool attack) {
        (void)part;
        (void)attack;
    }

    /// Render one block of `block_size()` frames, adding into `buses` (already cleared).
    virtual void render(Buses& buses) = 0;

    /// Any voice still sounding.
    virtual bool active() const = 0;

    /// Voice allocation, for the SXG-Create-NF patched DLL's second engine (Renderer): the
    /// free register sets (S-YXG50: the free-voice FIFO's count) and the
    /// polyphony. Engines without a voice FIFO report 0 / 0.
    virtual std::size_t free_voices() const { return 0; }
    virtual std::size_t polyphony() const { return 0; }
    /// Take over the control-rate clock (the 10-ms tick phase) of another module of the same
    /// engine model, as the patched DLL's second engine does when a note wakes it.
    /// `leader` is a tone module the same bank made (engine A of two).
    virtual void follow_clock(const ToneModule& leader) { (void)leader; }
    /// Voices sounding now (rendered in the next block), stolen voices fading out included.
    virtual std::size_t voice_count() const = 0;
};

class SystemEffects {
public:
    virtual ~SystemEffects() = default;

    /// Before the voices of a block render: set the insertion chains of `buses` (which parts
    /// render into which chain pre-bus) from the effect RAM.
    virtual void prepare(Buses& buses) { (void)buses; }

    /// Process one block: read the send buses, add the effect returns to `buses.dry_l/r`
    /// (which then hold the final output of the block). `frames` = the tone module's block
    /// size. Effect parameters are read from `ModuleContext::shared` (XG effect RAM).
    virtual void process(Buses& buses, std::size_t frames) = 0;

    /// An effect tail may still be audible (reverb / delay buffers not yet silent).
    virtual bool active() const = 0;

    /// Take over the mixer tick phase of another instance of the same engine model (see
    /// `ToneModule::follow_clock`).
    /// `leader` is system effects the same bank made (engine A of two).
    virtual void follow_clock(const SystemEffects& leader) { (void)leader; }
};

} // namespace hvyxg
