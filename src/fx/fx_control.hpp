// Copyright (C) 2026 Kevin H. Patterson
// SPDX-License-Identifier: BSD-3-Clause (see LICENSE)
#pragma once
// Effect control: turns XG reverb / chorus parameters into the effect DSP's 10-bit
// coefficient words and 16-bit delay addresses, reimplementing the behaviour of the effect
// controller both DLLs share (the same logic over the same tables). Words and addresses are the
// values the controller computes at 44.1 kHz; the DSP (fx_dsp.hpp) decodes them and scales
// addresses to the output rate.

#include <array>
#include <cstdint>

#include "fx/fx_tables.hpp"

namespace hvyxg::fx {

enum class Block : std::uint8_t { Reverb = 0, Chorus = 1, Variation = 2, Mixer = 3 };

/// Chorus algorithms run on the celeste kernel (three taps).
bool is_celeste(std::uint8_t chorus_algorithm);

/// XG reverb MSB/LSB → internal algorithm 1..18, 0 = off / not supported (both DLLs' type
/// maps; an LSB above 9 reads as 0).
std::uint8_t reverb_algorithm(const Tables& tb, std::uint8_t msb, std::uint8_t lsb);
/// XG chorus MSB/LSB → internal algorithm 1..20, 0 = off (MSB 0x41..0x43 only).
std::uint8_t chorus_algorithm(const Tables& tb, std::uint8_t msb, std::uint8_t lsb);
/// The XG default parameters 1..16 of a reverb / chorus algorithm (all 0 for algorithm 0).
std::array<std::uint16_t, 16> reverb_defaults(const Tables& tb, std::uint8_t algorithm);
std::array<std::uint16_t, 16> chorus_defaults(const Tables& tb, std::uint8_t algorithm);

/// Internal variation type: a category (micro-program family 0..3) and a type within it
/// (the XG type → internal type map). {0, 0} = off, {0, 1} = Thru.
struct VariationType {
    std::uint8_t category = 0;
    std::uint8_t type = 0;
    bool operator==(const VariationType&) const = default;
};

/// XG variation MSB/LSB → internal type. An
/// unknown MSB is off in system connection and Thru in insertion connection.
VariationType variation_type(const Tables& tb, std::uint8_t msb, std::uint8_t lsb, bool system);
/// The type's default parameters 1..16 (both DLLs' effect controls' and both engines' tables agree).
std::array<std::uint16_t, 16> variation_defaults(const Tables& tb, VariationType t);
/// The variation output gain (/64) of a type.
std::uint8_t variation_out_gain(const Tables& tb, VariationType t);

class EffectControl {
public:
    static constexpr std::size_t kWords = 0xC0;
    static constexpr std::size_t kAddrs = 0x40;

    /// `sample_rate` only selects the filter designer's tan(πf/fs) (the DLLs keep a table per
    /// rate); delay addresses stay in 44.1 kHz samples.
    explicit EffectControl(const Tables& tables, float sample_rate = 44100.0f);

    /// The engine tables it was made with (the DSP units read them too).
    const Tables& tables() const { return tb_; }

    /// Select an algorithm (cmd 0x0a / 0x0b): load its program and default parameters and
    /// recompute every register (with 0xff).
    void set_reverb_algorithm(std::uint8_t algorithm);
    void set_chorus_algorithm(std::uint8_t algorithm);
    /// Select a variation type (cmd 0x0c): its program, its defaults (dry/wet
    /// forced to 127 in system connection) and every register.
    void set_variation_type(VariationType type);
    /// The parameter-limit tables of the variation (`kVarLimits2006LE` / `kVarLimitsSyxg50`).
    void set_variation_limits(const std::uint16_t* limits) { var_limits_ = limits; }
    /// Range of parameter `index` of `block` for the current type; false when the type
    /// does not take it (`set_parameter` would ignore it).
    bool parameter_range(Block block, std::uint8_t index, std::uint16_t& lo, std::uint16_t& hi) const;
    /// One XG parameter (index 0..15 = XG parameter 1..16) clamped to its limit table
    /// false for an unused parameter.
    bool set_parameter(Block block, std::uint8_t index, std::uint16_t value);
    /// Reverb pan and return (mixer parameters 0 and 1), folded into the output all-pass
    /// words: the control image, the DSP by the engine (which uses its own time-set table: Plate 04 07 reads the Hall row).
    void set_reverb_return(std::uint16_t pan, std::uint16_t level);

    std::uint8_t reverb_algorithm() const { return rev_algo_; }
    std::uint8_t chorus_algorithm() const { return cho_algo_; }
    VariationType variation() const { return {var_cat_, var_type_}; }
    /// Mixer parameter 9 (`02 01 5A`): 1 = system connection, 0 = insertion.
    bool system_connection() const { return param_[3][9] == 1; }
    std::uint16_t parameter(Block block, std::uint8_t index) const {
        return param_[static_cast<std::size_t>(block)][index];
    }

    /// Coefficient word `i` of the control image (10 bits, format `kCoefFormat[i]`).
    std::uint16_t word(std::size_t i) const { return w_[i]; }
    /// Delay address `i` of the control image, in 44.1 kHz samples.
    std::int16_t addr(std::size_t i) const { return a_[i]; }
    /// What the DSP holds: the control sends only the registers each handler lists
    /// so the image can run ahead of the DSP (e.g. the room
    /// reverbs' diffusion handler recomputes the output all-pass words without sending them).
    std::uint16_t dsp_word(std::size_t i) const { return dsp_w_[i]; }
    std::int16_t dsp_addr(std::size_t i) const { return dsp_a_[i]; }

private:
    void dispatch(Block block, std::uint8_t index);
    void reverb_param(std::uint8_t index);
    void room_param(std::uint8_t index);
    void chorus_param(std::uint8_t index);
    void flanger_param(std::uint8_t index);
    void load_program(Block block);
    void load_defaults(Block block);

    void comb_gains(std::uint16_t time, std::uint16_t set, std::size_t list);
    void room_comb_gain(std::uint16_t time, std::int16_t skew, std::uint16_t dim,
                        std::size_t list);
    void diffusion(std::uint16_t diffusion, std::uint16_t set);
    void room_comb(std::uint16_t diffusion, std::uint8_t spread, std::uint16_t dim,
                   std::size_t list);
    void allpasses(std::uint16_t density, std::int16_t diffusion, std::uint16_t set);
    /// words {72, 84, 71, 83} for the given time set.
    std::array<std::uint16_t, 4> output_allpass_words(std::uint16_t set) const;
    void output_allpass_gains();
    void push_words(const std::uint8_t* list, std::size_t n);
    void push_addrs(const std::uint8_t* list, std::size_t n);
    void push_block(Block block);
    void delays();
    void er_balance();
    void density();
    void feedback();
    void chorus_delay(bool celeste);
    void chorus_eq_low();
    void chorus_eq_high();
    /// First-order filter designer (order 1).
    void design_first_order(std::uint8_t kind, std::uint8_t freq, std::uint8_t gain,
                            const std::uint8_t* list);

    std::uint16_t time_set() const;
    const std::uint16_t* limits(Block block) const;

    // --- variation (fx_control_variation.cpp and the family files) ------------------------
    // Register access by index: coefficient word i, delay address i.
    std::uint16_t& wr(unsigned i) { return w_[i]; }
    std::int16_t& ar(unsigned i) { return a_[i]; }
    /// Variation parameter i (0-based XG parameter i+1).
    std::uint16_t vp(std::size_t i) const { return param_[2][i]; }
    /// Dry/wet as the handlers read it: 127 in system connection.
    std::uint16_t dry_wet() const { return system_connection() ? 127 : param_[2][9]; }
    /// with a list offset (var_list()).
    void push_wl(std::uint32_t list, std::size_t n);
    void push_al(std::uint32_t list, std::size_t n);
    /// 0.1 ms → 44.1 kHz samples.
    static std::int16_t ms(std::uint16_t v);
    /// v · output gain of the variation type / 64.
    int out_gain(int v) const;
    /// The filter designer, every kind: order 1 (kinds 0..3,
    /// 0xd..0x13) and order 2 (4, 5, 8..11). `list` = {b0, b1, a} or {b0, b1, b2, a1, a2}.
    void filter(std::uint8_t order, std::uint8_t kind, std::uint8_t freq, std::uint8_t gain,
                std::uint8_t q, std::uint32_t list);
    /// the delay feedback high-damp FIR {h, 1 − h}·gain (mode 0).
    void damp(std::uint8_t mode, std::uint8_t value, std::uint8_t gain, std::uint32_t list);
    void connection_changed();
    void var_param(std::uint8_t index);   // block 2
    // Category 0.
    void var_chorus(std::uint8_t index);       // (chorus, celeste)
    void var_flanger(std::uint8_t index);
    void var_symphonic(std::uint8_t index);
    void var_rotary(std::uint8_t index);
    void var_tremolo(std::uint8_t index);
    void var_auto_pan(std::uint8_t index);
    void var_delay_lcr(std::uint8_t index);
    void var_delay_lr(std::uint8_t index);
    void var_echo(std::uint8_t index);
    void var_cross_delay(std::uint8_t index);
    void var_type18(std::uint8_t index);       // (no XG type)
    void var_eq2(std::uint8_t index);
    // Category 1.
    void var_phaser1(std::uint8_t index);
    void var_phaser2(std::uint8_t index);
    // Category 2.
    void var_er(std::uint8_t index);           // (ER 1/2, gate, reverse)
    void var_distortion(std::uint8_t index);   // (distortion, overdrive)
    void var_amp_sim(std::uint8_t index);
    void var_eq3(std::uint8_t index);
    // Category 3.
    void var_reverb(std::uint8_t index);
    void var_auto_wah(std::uint8_t index);
    void var_karaoke(std::uint8_t index);

    const Tables& tb_;
    std::array<std::uint16_t, kWords> w_{};
    std::array<std::int16_t, kAddrs> a_{};
    std::array<std::uint16_t, kWords> dsp_w_{};
    std::array<std::int16_t, kAddrs> dsp_a_{};
    std::array<std::array<std::uint16_t, 16>, 4> param_{};
    std::uint8_t rev_algo_ = 0;
    std::uint8_t cho_algo_ = 0;
    std::uint8_t room_row_ = 0;
    std::uint8_t room_order_ = 0;
    std::uint8_t var_cat_ = 0;
    std::uint8_t var_type_ = 0;
    const std::uint16_t* var_limits_ = nullptr;
    float rate_;
};

} // namespace hvyxg::fx
