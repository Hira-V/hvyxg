// Copyright (C) 2026 Kevin H. Patterson
// SPDX-License-Identifier: BSD-3-Clause (see LICENSE)
#pragma once
// The audio renderer: drives a bank's tone module and system effects in fixed blocks and
// hands out interleaved stereo frames of any count.
//
// The engines render fixed blocks (S-YXG50: 128 samples, S-YXG2006LE: 64) and apply MIDI
// events only between blocks, as the DLLs do. Actions therefore take effect at the next
// block boundary after they arrive (at most one block, 1.5 / 2.9 ms, of latency).

#include <array>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <span>

#include "actions.hpp"
#include "bank_impl.hpp"
#include "buses.hpp"
#include "eq.hpp"
#include "module.hpp"

namespace hvyxg {

class Renderer {
public:
    Renderer(std::shared_ptr<const Bank> bank, float sample_rate, std::uint16_t max_polyphony,
             Interpolation interpolation = Interpolation::Linear,
             float steal_fade_ms = 0.0f, bool dll_exact = false, bool second_engine = false,
             bool mu_part_eq = false, bool full_xg = false);

    /// Apply one engine → renderer action. The modules are built on `Init`; until then the
    /// output is silence.
    void handle_action(const Action& action);

    /// The SXG-Create-NF patched DLL's second engine (`second_engine`, docs/ENGINE.md "MU
    /// banks"): the patch routes each host event, not each part action, so the caller marks
    /// the MIDI message whose actions follow. A note-on message (velocity > 0) goes to engine
    /// A while A had two free voices at the start of the block (minus two per note-on already
    /// given to it in the block), else to B while B had two, else to A; every other message
    /// goes to both. `end_message` returns to per-action routing (each NoteOn decided alone).
    void begin_message(bool note_on);
    void end_message() { route_ = Route::PerAction; }
    /// A SysEx message arrived: a sleeping engine B stays awake for at least 250 ms.
    void sysex_received();

    /// Render `interleaved_stereo.size() / 2` frames (L, R, L, R, …).
    void render(std::span<float> interleaved_stereo);

    /// Something may still be audible: a voice is sounding, an effect tail is ringing, or
    /// less than the sleep delay has passed since both went quiet.
    bool needs_render() const;
    /// Silence after which `needs_render` turns false (0 = never).
    void set_sleep_delay_frames(std::uint64_t frames) { sleep_delay_frames_ = frames; }

    /// Frames left in the current block before the next block boundary (0 = at a boundary:
    /// an action now takes effect from the next frame rendered).
    std::size_t frames_to_boundary() const { return len_ - pos_; }
    /// The tone module's block size (valid after `Init`).
    std::size_t block_size() const { return block_; }
    /// Introspection: the modules (nullptr before `Init`).
    const ToneModule* tone_module() const { return tones_.get(); }
    const SystemEffects* system_effects() const { return effects_.get(); }
    /// The patched DLL's second engine (nullptr when not modelled or before `Init`).
    const ToneModule* second_tone_module() const { return b_ ? b_->tones.get() : nullptr; }
    const SystemEffects* second_system_effects() const { return b_ ? b_->effects.get() : nullptr; }
    /// Engine B renders this block (woken by a note, voices left or a recent SysEx).
    bool second_engine_awake() const { return b_ && b_->awake > 0; }

    float sample_rate() const { return sample_rate_; }
    std::uint64_t frame_count() const { return frames_; }
    const BankImpl& bank() const { return *bank_; }

private:
    void render_block();
    /// Pick the engine for a note-on message (consumes its budget).
    bool note_to_b();
    void wake_b();

    enum class Route : std::uint8_t { PerAction, Both, A, B };
    /// Engine B of the patched DLL's polyphony-256 patch: its own tone module, effects and
    /// buses; it renders after A, into the same output, only while awake.
    struct SecondEngine {
        std::unique_ptr<ToneModule> tones;
        std::unique_ptr<SystemEffects> effects;
        Buses buses;
        /// Samples it stays awake (slot + 8: 4 s after its last voice, 250 ms after a SysEx).
        std::int64_t awake = 0;
        /// A SysEx arrived since the last block.
        bool sysex = false;
    };

    std::shared_ptr<const BankImpl> bank_;
    float sample_rate_;
    std::uint16_t max_polyphony_;
    Interpolation interpolation_;
    float steal_fade_ms_;
    bool dll_exact_;
    bool full_xg_;
    bool second_engine_;
    bool mu_part_eq_;
    std::unique_ptr<ToneModule> tones_;
    std::unique_ptr<SystemEffects> effects_;
    std::size_t block_ = Buses::kMaxBlock;
    /// XG2.0 multi EQ on the final output (superset; nothing while flat).
    const midi::xg::MultiEq* multi_eq_ram_ = nullptr;
    eq::MultiEqStage multi_eq_;

    std::unique_ptr<SecondEngine> b_;
    Route route_ = Route::PerAction;
    /// Free voices of A and B as read at the start of the block, minus two per note-on given.
    std::int64_t budget_a_ = 0;
    std::int64_t budget_b_ = 0;

    Buses buses_;
    /// The current block's output, interleaved; `pos_` frames of it already handed out.
    std::array<float, 2 * Buses::kMaxBlock> out_{};
    std::size_t pos_ = 0;
    std::size_t len_ = 0;

    std::uint64_t frames_ = 0;
    std::uint64_t quiet_frames_ = 0;
    std::uint64_t sleep_delay_frames_ = 0;
};

} // namespace hvyxg
