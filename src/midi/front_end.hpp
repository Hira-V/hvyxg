// Copyright (C) 2026 Kevin H. Patterson
// SPDX-License-Identifier: BSD-3-Clause (see LICENSE)
#pragma once
// The MIDI front end: turns complete MIDI messages into writes to the XG parameter memory
// (xg_ram.hpp), live part state (part.hpp) and note / pedal actions for the tone module.
//
// It owns all the state the engine reads (`state()`), at stable addresses: create it once
// (it is neither copyable nor movable) and keep it for the life of the engine.
//
// Behaviour follows the XG specification and the MU-series parameter tables, with the
// DLLs' measured behaviour taking precedence (docs/ENGINE.md).

#include <array>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <span>

#include "actions.hpp"
#include "bank_impl.hpp"
#include "engine_state.hpp"
#include "midi/gs_effects.hpp"
#include "midi/part.hpp"
#include "midi/xg_ram.hpp"
#include "hvyxg/types.hpp"

namespace hvyxg::midi {

struct FrontEndOptions {
    /// Ports of 16 parts (1..4; the DLLs have one).
    std::size_t ports = 1;
    /// SysEx device number (0..15) the Yamaha / Roland / universal messages must carry,
    /// or −1 to accept every device number (both DLLs ignore it).
    int device_id = -1;
    /// DLL-exact mode (`Synth::Options::dll_exact`): messages only the superset acts on are
    /// ignored (multi EQ 02 40, XG2.0 insertion blocks 03, part extensions 0A, and the XG bulk
    /// dumps the S-YXG2006LE drops).
    bool dll_exact = false;
    /// Full XG on the S-YXG2006LE (`Synth::Options::full_xg_2006le`): the XGlite limits of its
    /// front end are lifted (XG parameters after a GS reset; GS messages with several
    /// parameters, every byte to its own address as on the S-YXG50).
    bool full_xg = false;
};

class FrontEnd {
public:
    FrontEnd(std::shared_ptr<const Bank> bank, ActionSink& sink, FrontEndOptions options = {});
    FrontEnd(const FrontEnd&) = delete;
    FrontEnd& operator=(const FrontEnd&) = delete;

    /// Send `Init` (the engine binds to `state()`) and the power-on reset (XG).
    void power_on();

    /// One complete message: a channel message (status byte first, no running status), a
    /// SysEx `F0 … F7`, or a system message (ignored). `port` selects parts 16·port.. .
    void message(std::span<const std::uint8_t> msg, std::size_t port = 0);

    /// Reset everything to the power-on state of `mode` (what the reset messages do).
    void reset(ResetMode mode);

    /// Release every note (e.g. on an input disconnect or an active-sensing timeout).
    void all_notes_off();

    EngineState state() const;

    // Introspection.
    ResetMode reset_mode() const { return mode_; }
    std::size_t part_count() const { return part_count_; }
    const Part& part(std::size_t i) const { return parts_[i]; }
    const xg::MultiPart& part_ram(std::size_t i) const { return ram_[i]; }
    const xg::System& system() const { return system_; }
    const xg::EffectData& effects() const { return effect_; }
    const xg::EffectInsertions& insertions() const { return insertion_; }
    const xg::MultiEq& multi_eq() const { return multi_eq_; }
    const xg::MultiPartExt& part_ram_ext(std::size_t i) const { return ram_ext_[i]; }
    const xg::DrumKitSetup& drum_setup(std::size_t slot) const { return drum_[slot]; }
    /// The last universal real-time master volume (14 bit); its MSB is also written to the
    /// XG master volume, which is what the engines play.
    std::uint16_t gm_master_volume() const { return gm_master_volume_; }

private:
    /// Marks every part touched (master volume, GM2 master tuning).
    void touch_all();
    /// Part gain inputs from the RAM (`Part::gain_volume`, `gain_master`).
    void refresh_gain(Part& p);
    void refresh_gain_all();
    /// The channel map from the RAM's receive channels.
    void rebuild_routing();
    /// Set while an S-YXG50 parameter change writes a parameter whose end-of-message handler
    /// it skips (bytes past the parameter): the RAM is written, nothing is applied.
    bool deferred_lost_ = false;
    enum class Selected : std::uint8_t { None, Rpn, Nrpn };
    /// Front-end-only per-part state.
    struct PartState {
        std::uint8_t bank_msb = 0;  // pending bank select (CC0 / CC32)
        std::uint8_t bank_lsb = 0;
        Selected selected = Selected::None;
        std::uint8_t rpn_msb = 0x7F, rpn_lsb = 0x7F;
        std::uint8_t nrpn_msb = 0x7F, nrpn_lsb = 0x7F;
        /// GS variation number (CC#0) of a GS-translating engine (Bank::gs_banks), sent as
        /// the bank LSB at the next program change.
        std::uint8_t gs_variation = 0;
        /// The receive channel the routing uses (the S-YXG50 rebuilds its channel map from the
        /// RAM at each receive-channel change it applies).
        std::uint8_t routed_channel = 0x7F;
        /// The part mode last applied; the RAM can be ahead of it.
        std::uint8_t applied_mode = 0;
        /// S-YXG50: the bank and program last selected in normal mode (MSB, LSB, program, and LSB / program for MSB 121) and in drum mode 1 (MSB, then LSB / program for MSB 127, for other MSBs and for MSB 120). A part-mode
        /// change selects the new mode's.
        std::array<std::uint8_t, 5> normal_bank{0, 0, 0, 0, 0};
        std::array<std::uint8_t, 7> drum_bank{0x7F, 0, 0, 0, 0, 0, 0};
    };

    void emit(const Action& a) { sink_.act(a); }
    /// GS mode on an engine that translates GS onto XG banks (the 2006LE).
    bool gs_translated() const { return mode_ == ResetMode::GS && bank_->gs_banks().has_value(); }
    gs::Dialect gs_dialect() const {
        return bank_->type() == Model::Syxg2006le ? gs::Dialect::Syxg2006le : gs::Dialect::Syxg50;
    }

    // Channel messages.
    void channel_message(Part& p, std::uint8_t status, std::uint8_t d1, std::uint8_t d2);
    void control_change(Part& p, std::uint8_t cc, std::uint8_t v);
    void program_change(Part& p, std::uint8_t program);
    void reset_controllers(Part& p);
    void data_entry(Part& p, int msb, int lsb, int delta);
    void set_rpn(Part& p, std::uint8_t msb, std::uint8_t lsb, int value_msb, int value_lsb, int delta);
    void set_nrpn(Part& p, std::uint8_t msb, std::uint8_t lsb, int value, int delta);

    // Programs and drums.
    /// Re-derive the latched program (drum-mode MSB rule) and load the drum setups.
    void update_program(Part& p, bool reload_drums);
    bool plays_kit(const Part& p) const;
    /// The kit the bank should build the drum-setup image from for this bank MSB.
    std::uint8_t image_msb(std::uint8_t msb) const;
    void load_drum_slot(std::size_t slot, std::uint8_t msb, std::uint8_t program);
    /// S-YXG50: remember the part's bank / program for its mode and, at a
    /// part-mode change, select the new mode's.
    void save_mode_bank(std::size_t part);
    void restore_mode_bank(std::size_t part);
    xg::DrumSetup* drum_note(Part& p, std::uint8_t note);

    // SysEx.
    void sysex(std::span<const std::uint8_t> msg);
    bool device_matches(std::uint8_t dev) const;
    void yamaha(std::span<const std::uint8_t> body);
    void roland(std::span<const std::uint8_t> body);
    void universal_non_realtime(std::span<const std::uint8_t> body);
    void universal_realtime(std::span<const std::uint8_t> body);
    /// XG parameter change (`bulk` false) or bulk dump: `data` bytes to consecutive
    /// addresses from (hi, mid, lo).
    void xg_parameter(std::uint8_t hi, std::uint8_t mid, std::uint8_t lo, std::span<const std::uint8_t> data,
                      bool bulk);
    void xg_write(std::uint8_t hi, std::uint8_t mid, std::uint8_t lo, std::uint8_t value);
    void part_write(std::size_t part, std::uint8_t addr, std::uint8_t value);
    /// S-YXG50: all sound off on the variation insertion part (02 01 5B), if it is one of the
    /// first 16 parts.
    void kill_insertion_part();
    /// One GS parameter; `single`: the message carried one data byte.
    void gs_write(std::uint8_t a1, std::uint8_t a2, std::uint8_t a3, std::uint8_t value, bool single);
    void gs_part_write(std::size_t part, std::uint8_t addr, std::uint8_t value);
    /// A GS part message (40 1x a …) through the 2006LE's GS layer.
    void le_gs_part(std::size_t part, std::uint8_t addr, std::span<const std::uint8_t> data);

    std::shared_ptr<const BankImpl> bank_;
    ActionSink& sink_;
    FrontEndOptions options_;
    std::size_t part_count_;

    ResetMode mode_ = ResetMode::XG;
    /// EngineState::events: every message, reset and all-notes-off.
    std::uint32_t events_ = 0;
    xg::System system_;
    std::uint16_t gm_master_volume_ = 0x3FFF;
    xg::EffectData effect_;
    gs::EffectState gs_effect_;
    xg::EffectInsertions insertion_{};
    DrumSetupRam drum_{};
    /// The kit each drum setup was loaded from (drum setup reset reloads it).
    std::array<Program, xg::kDrumSetups> drum_kit_{};
    /// S-YXG50: the bank / program last selected for each drum setup (laid out as `PartState::drum_bank`).
    std::array<std::array<std::uint8_t, 7>, xg::kDrumSetups> setup_bank_{};
    /// The XG write being applied comes from a bulk dump.
    bool bulk_ = false;
    std::array<xg::MultiPart, kMaxPartSize> ram_;
    std::array<xg::MultiPartExt, kMaxPartSize> ram_ext_{};
    xg::MultiEq multi_eq_;
    std::array<Part, kMaxPartSize> parts_{};
    std::array<PartState, kMaxPartSize> ps_{};
};

} // namespace hvyxg::midi
