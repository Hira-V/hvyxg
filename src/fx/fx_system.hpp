// Copyright (C) 2026 Kevin H. Patterson
// SPDX-License-Identifier: BSD-3-Clause (see LICENSE)
#pragma once
// System effects block shared by both engine models: XG effect RAM → effect control → reverb and
// chorus DSP units, plus the engine-side mixer (input gains, returns, pans, chorus → reverb
// send, their slewing) and the per-unit sleep gate. The per-DLL differences are the
// `EngineModel` constants (syxg50/effects.cpp, syxg2006le/effects.cpp).
//
// Signal domain: the buses follow the bus contract (buses.hpp), i.e. DLL bus × the factor of
// the DLL's dry path. Both DLLs invert the dry path (dry × −1) and use negative effect input
// gains; here the dry path is kept as is, so every effect input gain is applied negated and
// everything after it (kernels, returns, effect-to-effect sends) is linear and unchanged.

#include <array>
#include <cstddef>
#include <cstdint>

#include "fx/fx_dsp.hpp"
#include "fx/fx_control.hpp"
#include "fx/fx_insertion.hpp"
#include "fx/fx_variation.hpp"
#include "module.hpp"

namespace hvyxg::midi::xg {
struct EffectData;
}

namespace hvyxg::fx {

/// The variation takes a stereo input (engine[0x66dc]): voices then send
/// to the variation without the centre-pan attenuation of the mono-input types (system
/// connection only).
bool variation_stereo_input(const Tables& tb, const midi::xg::EffectData& ram);
/// The chorus takes a stereo input (engine[0x66dd]: a flanger, or chorus parameter 15 = 1):
/// voices then send to the chorus without the centre-pan attenuation, and the variation →
/// chorus send uses its stereo offset.
bool chorus_stereo_input(const midi::xg::EffectData& ram);

/// How the control depths (variation 02 01 5C..60, XG2.0 insertion 03 nn 0D..11) turn the
/// part's controllers into an offset of the effect type's controlled parameter.
enum class ControlLaw : std::uint8_t {
    /// the bend term is (bend − 32) · d / 32 with the full 14-bit bend
    /// (the centre of a 6-bit bend subtracted from a 14-bit value, so any bend above 32
    /// drives the offset to its limit), the sum is wrapped to 16 bits.
    Syxg50,
    /// S-YXG2006LE and MU2000 (measured): the bend term is (bend − 8192) · d / 8192.
    Centred,
};

/// Σ (depth − 64) · source / 64 over MW, CAT, AC1 and AC2 plus the bend term, each rounded
/// toward zero, clipped to ±127. `depths`: MW, bend, CAT, AC1, AC2 (64 = none).
int control_offset(const std::uint8_t* depths, const midi::Part& part, ControlLaw law);
/// Move `cur` toward `target` by at most 4 (per 10-ms tick).
inline int control_step(int cur, int target) {
    return target < cur ? (cur - 4 > target ? cur - 4 : target) : (cur + 4 < target ? cur + 4 : target);
}
/// The parameter (0-based) a type's control depths drive, or −1: parameter 1 for rotary,
/// tremolo, auto pan, distortion, overdrive and amp simulator (MSB 45..47, 49..4B) and ring
/// modulator (71), parameter 3 for the auto wah (4E), none for "no effect" (00), compressor
/// (53) and noise gate (54), dry/wet (parameter 10) otherwise (table
/// the XG2.0 types measured on the MU2000).
int controlled_parameter(std::uint8_t msb);
/// The offset as the S-YXG2006LE and the MU2000 apply it to a parameter of range lo..hi:
/// offset · (hi − lo) / 127, rounded toward zero (dry/wet 1..127 moves by one less) [meas].
inline int scale_control(int offset, int lo, int hi) { return offset * (hi - lo) / 127; }

struct EngineModel {
    /// Control tick of the mixer slews (S-YXG50 10 ms, S-YXG2006LE ≈ 12 ms).
    float tick_seconds;
    /// Input-gain ramp per tick (units of 1/128) and effect-send slew per tick.
    int input_step;
    int send_step;
    /// Scale the DSP applies to the chorus return words (×0.5) and to
    /// the chorus → reverb words (×0.3536).
    float chorus_return_scale;
    float chorus_send_scale;
    /// One DLL bus unit in bus-contract units (the sleep-gate threshold).
    float bus_unit;
    /// The bus unit of the distortion family's clip level (±2^17 units; variation and
    /// insertion blocks): 0 = `bus_unit`. Full XG on the 2006LE takes the S-YXG50's 2^-15,
    /// so the same song drives the distortion as hard as on the S-YXG50.
    float clip_unit = 0.0f;
    float distortion_clip_unit() const { return clip_unit > 0.0f ? clip_unit : bus_unit; }
    /// Sleep gate: seconds of silent output after which a unit stops (DLL: 0x400 × 128 /
    /// 0x800 × 64 blocks for the reverb, a quarter of that for the chorus).
    float reverb_hold_seconds;
    float chorus_hold_seconds;
    /// The S-YXG50 engine limits the delay offset of every flanger type (MSB 0x43, including
    /// the hidden 43 07) to 63 before the effect control sees it (measured).
    bool flanger_delay_63;
    /// Variation parameter limits (`kVarLimitsSyxg50` / `kVarLimits2006LE`).
    const std::uint16_t* variation_limits;
    /// Sleep gate of the variation (0x400 × 128 / 0x800 × 64 blocks, like the reverb).
    float variation_hold_seconds;
    /// Variation control depths: syxg50 applies them per 10-ms tick, moving the offset by 4
    /// per tick, with `ControlLaw::Syxg50` and unscaled; the 2006LE applies them in the block
    /// of the controller event, at once, with `ControlLaw::Centred` and scaled to the
    /// parameter's range, and re-applies them after a type change [meas: effect control dumps].
    bool variation_control_ticked;
    /// The insertion part's voice update moves the return
    /// / send targets before the tick of the block in which its voices start (the note-on
    /// block's tick already slews toward them) [meas: register log]; otherwise from
    /// the next tick. A change of the part's volume, expression or the master volume
    /// moves them at once, voices or not.
    bool insertion_update_in_block = false;
    /// Every write of the variation connection re-applies it, also with the same value (S-YXG50
    /// case 9: type re-sent, input gain from 0, control offset 0; measured). The
    /// 2006LE's variation is silent, so a re-sent value is taken as no change there: its
    /// insertion part keeps playing as in the DLL instead of dipping through the input ramp.
    bool connection_write_resends;
    /// Scale the DSP applies to the variation return and to the variation → reverb /
    /// → chorus words (S-YXG50 1; the 2006LE multiplies them by 0, see its effects.cpp).
    /// System-connection variation return / send scales (insertion connection takes the
    /// part's own level law instead).
    float variation_return_scale;
    float variation_reverb_scale;
    float variation_chorus_scale;
    /// The insertion part's master × volume × expression as an attenuation (1/16 octave) of
    /// the variation return: the engine's own volume law (for the 2006LE,
    /// whose insertion return is superset behaviour, the law of its voice level).
    int (*insertion_attenuation)(const EngineTables& tables, const EngineState& shared, const midi::Part& part);
    /// XG2.0 insertion chains (fx_insertion.hpp): the attenuation a chained voice carries on
    /// its dry pair beyond the send path (2006LE: its fixed dry offset 8; S-YXG50 0), whether
    /// the voices' reverb send includes the pan (2006LE: mean of L / R), and the centre-pan
    /// attenuation of mono-input chorus / variation sends (none).
    int insertion_chain_offset;
    /// The part's master × volume × expression as an attenuation (1/16 octave, may be
    /// fractional) for an insertion chain: the law the part's voices would have applied
    /// (S-YXG50: the part gain through the level table, ≈ 40·log10(g / 128);
    /// 2006LE: `insertion_attenuation`).
    float (*insertion_chain_attenuation)(const EngineTables& tables, const EngineState& shared,
                                         const midi::Part& part);
    bool insertion_reverb_panned;
    const std::uint8_t* insertion_chorus_pan_term;
    /// SXG-Create-NF patched syxg50.dll (`--enable-mu`, fx_mu.hpp): the reverb and the
    /// system variation return scaled per type, the reverb return recomputed after a type
    /// change, and the input-gain ramps after a type change in larger steps (reverb 32,
    /// chorus 52, variation 40 per tick instead of `input_step`).
    bool mu = false;
    /// The DLL's variation never reaches the output (S-YXG2006LE): in DLL-exact mode it stays
    /// silent here too.
    bool variation_inert = false;
    int reverb_input_step = 0;
    int chorus_input_step = 0;
    int variation_input_step = 0;
};

class SystemFx final : public SystemEffects {
public:
    SystemFx(const ModuleContext& ctx, const EngineModel& model);

    void prepare(Buses& buses) override {
        if (!ctx_.dll_exact) insertion_.prepare(buses);
    }
    void process(Buses& buses, std::size_t frames) override;
    /// `leader` is engine A's effects, made by the same bank: a SystemFx too.
    void follow_clock(const SystemEffects& leader) override {
        tick_acc_ = static_cast<const SystemFx&>(leader).tick_acc_;
    }
    bool active() const override {
        return reverb_running_ || chorus_running_ || var_running_ || insertion_.active();
    }
    const InsertionFx& insertion() const { return insertion_; }

    // Introspection for tests.
    const EffectControl& control() const { return ctrl_; }
    float reverb_input_gain() const;
    float chorus_input_gain() const;
    float variation_input_gain() const;
    /// Current variation return L / R and send words as linear gains.
    float variation_return(bool right) const;
    float variation_to_reverb(bool right) const;
    float variation_to_chorus(bool right) const;

private:
    struct Slew {
        int cur = 0;
        int target = 0;
        void step(int amount);
    };

    void sync_parameters();
    void sync_variation(bool first);
    void set_reverb_type(std::uint8_t msb, std::uint8_t lsb);
    void set_chorus_type(std::uint8_t msb, std::uint8_t lsb);
    void set_variation_type();
    void update_chorus_mixer();
    /// Variation return / send targets (systeminsertion). `immediate`: the currents jump to the targets (insertion part change).
    void update_variation_mixer(bool immediate, bool gain_only = false);
    /// The insertion part, or nullptr (system connection, part off, part not present).
    const midi::Part* insertion_part() const;
    void process_variation(Buses& buses, std::size_t frames, bool reverb_on, bool chorus_on);
    /// The insertion variation's control depths (02 01 5C..60) applied to its controlled
    /// parameter: per tick (syxg50) or per block (2006LE), see `variation_control_ticked`.
    void variation_control(const midi::Part& part);
    void tick();
    /// Advance a fade over one block; returns its per-sample step.
    float advance_fade(float& fade, std::size_t frames) const;

    ModuleContext ctx_;
    EngineModel model_;
    const Tables& tb_;
    EffectControl ctrl_;
    ReverbUnit reverb_;
    ChorusUnit chorus_;
    VariationUnit variation_;
    InsertionFx insertion_;

    /// Last XG effect RAM bytes seen (reverb 0x00..0x15, chorus 0x20..0x35, variation
    /// 0x40..0x60 and 0x70..0x75).
    std::array<std::uint8_t, 0x76> ram_{};
    /// `EffectData::*_type_writes` at the last sync: a change reloads the unit even when
    /// the type bytes are unchanged (type re-sent, reset).
    std::uint32_t reverb_type_writes_ = 0;
    std::uint32_t chorus_type_writes_ = 0;
    std::uint32_t variation_type_writes_ = 0;
    std::uint32_t variation_connection_writes_ = 0;
    std::uint32_t restarts_ = 0;
    bool first_sync_ = true;
    /// Variation in system connection (XG 02 01 5A = 1).
    bool var_system_ = false;

    // Mixer (engine words, s10 / 128 for the input gains and sends).
    Slew reverb_in_;
    Slew chorus_in_;
    Slew chorus_send_l_, chorus_send_r_;
    float chorus_return_l_ = 0, chorus_return_r_ = 0;
    // Variation mixer: input gain (/128) and return / send words (return /512, sends /128),
    // slewed toward their targets by `send_step` per tick.
    Slew var_in_;
    Slew var_ret_l_, var_ret_r_, var_rev_l_, var_rev_r_, var_cho_l_, var_cho_r_;
    /// Variation input seen since the last tick (the insertion part's voices sound).
    bool var_input_seen_ = false;
    /// The insertion part's volume attenuation at the last check (`insertion_update_in_block`).
    const midi::Part* ins_part_seen_ = nullptr;
    int ins_att_seen_ = 0;
    /// The insertion part's pan and reverb / chorus sends as of its last voice update: a gain
    /// change recomputes the targets with these [meas].
    std::uint8_t ins_pan_ = 64, ins_rev_ = 0, ins_cho_ = 0;
    /// Control-change modulation of the insertion variation (engine[0x66d4]): the current offset, slewed by 4 per tick.
    int var_ctrl_ = 0;
    /// A type change since the last control update (the 2006LE re-applies the offset).
    bool var_ctrl_dirty_ = false;
    float tick_len_;
    float tick_acc_ = 0;

    // Sleep gates.
    bool reverb_running_ = false;
    bool chorus_running_ = false;
    bool var_running_ = false;
    // Per-unit output fade (syxg50 regs 11/12, 87/88, 188/189): starts at 0 and rises with a 64-sample time constant while the unit runs,
    // stepped per block and interpolated across it. A type change keeps it and the unit's
    // running state (the DSP clears only the ring and kernel state; measured on syxg50).
    float reverb_fade_ = 0;
    float chorus_fade_ = 0;
    float var_fade_ = 0;
    float fade_pole_;  // per sample, 504/512 at 44.1 kHz
    std::size_t reverb_quiet_ = 0, chorus_quiet_ = 0, var_quiet_ = 0;
    std::size_t reverb_hold_, chorus_hold_, var_hold_;

    std::array<float, Buses::kMaxBlock> rev_in_{};
    std::array<float, Buses::kMaxBlock> rev_r_{};
    std::array<float, Buses::kMaxBlock> cho_l_{};
    std::array<float, Buses::kMaxBlock> cho_r_{};
    std::array<float, Buses::kMaxBlock> var_l_{};
    std::array<float, Buses::kMaxBlock> var_r_{};
};

} // namespace hvyxg::fx
