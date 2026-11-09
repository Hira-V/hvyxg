// Copyright (C) 2026 Kevin H. Patterson
// SPDX-License-Identifier: BSD-3-Clause (see LICENSE)
#include "fx/fx_system.hpp"

#include <algorithm>
#include <cmath>

#include "engine_tables.hpp"
#include "fx/fx_mu.hpp"
#include "fx/fx_tables.hpp"
#include "midi/part.hpp"
#include "midi/xg_ram.hpp"

namespace hvyxg::fx {

namespace {

// Engine mixer targets:
// reverb input −2.0, chorus input −3.219 (s10 words / 128).
constexpr int kReverbInputTarget = -256;
constexpr int kChorusInputTarget = -412;

constexpr std::uint8_t kChorusBase = 0x20;
// Variation input gain targets: −2.83 system, −1.0 insertion (/128).
constexpr int kVarInputSystem = -362;
constexpr int kVarInputInsertion = -128;

float word128(int w) { return static_cast<float>(w) * (1.0f / 128.0f); }

// RAM offset of XG effect parameter `i` (0..15) within a block.
std::uint8_t param_offset(std::size_t i) {
    return static_cast<std::uint8_t>(i < 10 ? 2 + i : 16 + (i - 10));
}

// Engine mixer word: 512 · 2^(−attenuation/16).
int mixer_word(const Tables& tb, int att) { return tb.kMixerWord[static_cast<std::size_t>(std::min(att, 127))]; }

float max_abs(const float* x, std::size_t n) {
    float m = 0;
    for (std::size_t i = 0; i < n; ++i) m = std::max(m, std::fabs(x[i]));
    return m;
}

} // namespace

bool variation_stereo_input(const Tables& tb, const midi::xg::EffectData& ram) {
    // the input class of the variation MSB (kVarInputClass).
    switch (tb.kVarInputClass[ram[0x40] & 0x7Fu]) {
    case 0: return false;
    case 2: return ram[0x74] != 0;  // chorus / celeste / tremolo: parameter 15 (input mode)
    case 3: return ram[0x41] == 8;  // phaser 2
    case 4: return ram[0x49] == 2;  // cross delay: input select L, R
    default: return true;
    }
}

bool chorus_stereo_input(const midi::xg::EffectData& ram) {
    return ram[0x20] == 0x43 || ram[0x34] != 0;
}

int control_offset(const std::uint8_t* depths, const midi::Part& part, ControlLaw law) {
    const auto term = [](std::uint8_t depth, int value, int div) {
        const int d = static_cast<int>(depth) - 64;
        return d == 0 ? 0 : (value * d) / div;
    };
    const int bend = law == ControlLaw::Syxg50 ? term(depths[1], static_cast<int>(part.pitchbend) - 0x20, 32)
                                               : term(depths[1], static_cast<int>(part.pitchbend) - 0x2000, 0x2000);
    int sum = bend + term(depths[0], part.controller.modulation, 64) + term(depths[2], part.cat_value, 64) +
              term(depths[3], part.controller.ac[0], 64) + term(depths[4], part.controller.ac[1], 64);
    // syxg50 compares the 32-bit sum's low 16 bits.
    if (law == ControlLaw::Syxg50) sum = static_cast<std::int16_t>(sum);
    return std::clamp(sum, -127, 127);
}

int controlled_parameter(std::uint8_t msb) {
    switch (msb) {
    case 0x00: case 0x53: case 0x54: return -1;
    case 0x45: case 0x46: case 0x47: case 0x49: case 0x4A: case 0x4B: case 0x71: return 0;
    case 0x4E: return 2;
    default: return 9;
    }
}

void SystemFx::Slew::step(int amount) {
    // move by `amount`, snap when closer than that.
    const int diff = target - cur;
    if (std::abs(diff) < amount) cur = target;
    else cur += diff > 0 ? amount : -amount;
}

SystemFx::SystemFx(const ModuleContext& ctx, const EngineModel& model)
    : ctx_(ctx), model_(model), tb_(ctx.tables->fx), ctrl_(tb_, ctx.sample_rate), reverb_(ctx.sample_rate), chorus_(ctx.sample_rate),
      variation_(ctx.sample_rate, model.distortion_clip_unit()), insertion_(ctx, model_),
      tick_len_(model.tick_seconds * ctx.sample_rate),
      fade_pole_(static_cast<float>(std::pow(504.0 / 512.0, 44100.0 / ctx.sample_rate))),
      reverb_hold_(static_cast<std::size_t>(model.reverb_hold_seconds * ctx.sample_rate)),
      chorus_hold_(static_cast<std::size_t>(model.chorus_hold_seconds * ctx.sample_rate)),
      var_hold_(static_cast<std::size_t>(model.variation_hold_seconds * ctx.sample_rate)) {
    ctrl_.set_variation_limits(model.variation_limits);
    // As after a reset: the input gains ramp up from 0.
    reverb_in_ = {0, kReverbInputTarget};
    chorus_in_ = {0, kChorusInputTarget};
    sync_parameters();
}

float SystemFx::variation_input_gain() const { return -word128(var_in_.cur); }
float SystemFx::variation_return(bool right) const {
    return static_cast<float>(right ? var_ret_r_.cur : var_ret_l_.cur) / 512.0f *
           (var_system_ ? model_.variation_return_scale : 1.0f);
}
float SystemFx::variation_to_reverb(bool right) const {
    return word128(right ? var_rev_r_.cur : var_rev_l_.cur) *
           (var_system_ ? model_.variation_reverb_scale : 1.0f);
}
float SystemFx::variation_to_chorus(bool right) const {
    return word128(right ? var_cho_r_.cur : var_cho_l_.cur) *
           (var_system_ ? model_.variation_chorus_scale : 1.0f);
}

float SystemFx::reverb_input_gain() const { return -word128(reverb_in_.cur); }
float SystemFx::chorus_input_gain() const { return -word128(chorus_in_.cur); }

void SystemFx::set_reverb_type(std::uint8_t msb, std::uint8_t lsb) {
    ctrl_.set_reverb_algorithm(reverb_algorithm(tb_, msb, lsb));
    reverb_.clear();
    reverb_.load(ctrl_);
    // The engine restarts the reverb inputs from 0.
    reverb_in_.cur = 0;
    chorus_send_l_.cur = chorus_send_r_.cur = 0;
    var_rev_l_.cur = var_rev_r_.cur = 0;
}

void SystemFx::set_chorus_type(std::uint8_t msb, std::uint8_t lsb) {
    ctrl_.set_chorus_algorithm(chorus_algorithm(tb_, msb, lsb));
    chorus_.clear();
    chorus_.load(ctrl_);
    chorus_in_.cur = 0;
    var_cho_l_.cur = var_cho_r_.cur = 0;
}

void SystemFx::set_variation_type() {
    const bool system = ram_[0x5A] == 1;
    ctrl_.set_variation_type(variation_type(tb_, ram_[0x40], ram_[0x41], system));
    variation_.clear();
    variation_.load(ctrl_);
    // The unit keeps running (and its fade) across a type change: the DSP only clears the
    // ring and the kernel state (measured on
    // syxg50: an LFO type set while the unit runs starts its LFO at the change).
    var_in_.cur = 0;
    var_in_.target = system ? kVarInputSystem : kVarInputInsertion;
}

const midi::Part* SystemFx::insertion_part() const {
    if (ram_[0x5A] == 1) return nullptr;
    const std::uint8_t p = ram_[0x5B];
    if (!ctx_.shared.parts || p >= ctx_.shared.part_count) return nullptr;
    return &ctx_.shared.parts[p];
}

void SystemFx::update_variation_mixer(bool immediate, bool gain_only) {
    const midi::xg::EffectData& ram = *ctx_.shared.effect1;
    const bool cho_stereo = chorus_stereo_input(ram);
    const auto set = [&](Slew& sl, int target) {
        sl.target = target;
        if (immediate) sl.cur = target;
    };
    if (ram_[0x5A] == 1) {
        // return, pan and sends of the variation block.
        const int pan = std::clamp<int>(ram_[0x57], 1, 127);
        const int pl = tb_.kMixerPan[static_cast<std::size_t>(128 - pan)];
        const int pr = tb_.kMixerPan[static_cast<std::size_t>(pan)];
        std::uint8_t ret_level = std::min<std::uint8_t>(ram_[0x56], 127);
        if (model_.mu) ret_level = mu_return_level(Block::Variation, ram_[0x40], ram_[0x41], ret_level);
        const int ret = tb_.kMixerLevel[ret_level];
        const int rev = tb_.kMixerLevel[std::min<std::uint8_t>(ram_[0x58], 127)] + 0x18;
        const int cho = tb_.kMixerLevel[std::min<std::uint8_t>(ram_[0x59], 127)] + (cho_stereo ? 0x15 : 0x25);
        set(var_ret_l_, mixer_word(tb_, ret + pl));
        set(var_ret_r_, mixer_word(tb_, ret + pr));
        set(var_rev_l_, mixer_word(tb_, rev + pl));
        set(var_rev_r_, mixer_word(tb_, rev + pr));
        set(var_cho_l_, mixer_word(tb_, cho + pl));
        set(var_cho_r_, mixer_word(tb_, cho + pr));
        return;
    }
    const midi::Part* part = insertion_part();
    if (!part) {
        // no insertion part, the variation output goes nowhere.
        for (Slew* sl : {&var_ret_l_, &var_ret_r_, &var_rev_l_, &var_rev_r_, &var_cho_l_, &var_cho_r_})
            *sl = {0, 0};
        return;
    }
    // the insertion part's master / volume / expression, pan and
    // reverb / chorus sends apply after the effect.
    const midi::xg::MultiPart& r = *part->ram;
    if (!gain_only) {
        ins_pan_ = r.pan;
        ins_rev_ = r.reverb_send;
        ins_cho_ = r.chorus_send;
    }
    const int base = model_.insertion_attenuation(*ctx_.tables, ctx_.shared, *part);
    const int pan = ins_pan_ == 0 ? 64 : ins_pan_;
    const int pl = tb_.kMixerPan[static_cast<std::size_t>(128 - pan)] + base;
    const int pr = tb_.kMixerPan[static_cast<std::size_t>(pan)] + base;
    const int rev = tb_.kMixerLevel[std::min<std::uint8_t>(ins_rev_, 127)] + 0x18;
    const int cho = tb_.kMixerLevel[std::min<std::uint8_t>(ins_cho_, 127)] + (cho_stereo ? 8 : 0x18);
    set(var_ret_l_, mixer_word(tb_, pl));
    set(var_ret_r_, mixer_word(tb_, pr));
    set(var_rev_l_, mixer_word(tb_, pl + rev));
    set(var_rev_r_, mixer_word(tb_, pr + rev));
    set(var_cho_l_, mixer_word(tb_, pl + cho));
    set(var_cho_r_, mixer_word(tb_, pr + cho));
}

void SystemFx::sync_variation(bool first) {
    const midi::xg::EffectData& ram = *ctx_.shared.effect1;
    std::array<std::uint8_t, 0x76> now = ram_;
    for (std::uint8_t i = 0x40; i <= 0x60; ++i) now[i] = ram[i];
    for (std::uint8_t i = 0x70; i <= 0x75; ++i) now[i] = ram[i];
    // A connection write makes the engine re-send the type (case 9:
    // parameters, type, ring cleared, input ramp from 0, control offset 0), on the S-YXG50
    // also when the value does not change [meas] (`connection_write_resends`). An insertion-part write does none of this: the variation keeps its
    // ring, input gain and control offset, and only the return follows the new part [meas].
    const bool conn_changed = first || now[0x5A] != ram_[0x5A] ||
                              (model_.connection_write_resends &&
                               ram.variation_connection_writes != variation_connection_writes_);
    const bool part_changed = first || now[0x5B] != ram_[0x5B];
    const bool type_changed = first || conn_changed || ram.variation_type_writes != variation_type_writes_ ||
                              now[0x40] != ram_[0x40] || now[0x41] != ram_[0x41];
    variation_type_writes_ = ram.variation_type_writes;
    variation_connection_writes_ = ram.variation_connection_writes;
    const std::array<std::uint8_t, 0x76> old = ram_;
    for (std::uint8_t i = 0x40; i <= 0x75; ++i) ram_[i] = now[i];
    if (conn_changed) ctrl_.set_parameter(Block::Mixer, 9, now[0x5A] == 1 ? 1 : 0);
    if (conn_changed) var_ctrl_ = 0;  // case 9
    if (type_changed) {
        set_variation_type();
        var_ctrl_dirty_ = true;
    }
    bool changed = type_changed || conn_changed;
    for (std::uint8_t i = 0; i < 16; ++i) {
        std::uint16_t v, before;
        if (i < 10) {
            const std::size_t a = 0x42u + 2u * i;
            v = static_cast<std::uint16_t>(now[a] * 128 + (now[a + 1] & 0x7F));
            before = static_cast<std::uint16_t>(old[a] * 128 + (old[a + 1] & 0x7F));
        } else {
            v = now[0x70u + (i - 10u)];
            before = old[0x70u + (i - 10u)];
        }
        // In system connection the engine sends dry/wet 127 whatever the RAM holds (syxg50
        // engine copy 0x6966 = 0x7f; most handlers force it again, karaoke
        // reads the stored value).
        if (i == 9 && now[0x5A] == 1) v = before = 127;
        const bool differs = type_changed ? v != ctrl_.parameter(Block::Variation, i) : v != before;
        if (differs) changed |= ctrl_.set_parameter(Block::Variation, i, v);
    }
    if (changed) variation_.load(ctrl_);
    // Return / pan / sends (cases 5..9): system connection recomputes the
    // targets (slewed); insertion connection recomputes from the part at once,
    // and with no insertion part leaves them as they are (a part write of 7F keeps the return
    // of the old part on the variation's tail [meas]); a connection write without a part
    // zeroes them as the reset does. A type change alone leaves them as they are.
    const bool block_changed = now[0x56] != old[0x56] || now[0x57] != old[0x57] ||
                               now[0x58] != old[0x58] || now[0x59] != old[0x59];
    const bool system = now[0x5A] == 1;
    var_system_ = system;
    const bool has_part = system || insertion_part() != nullptr;
    if (conn_changed || (has_part && (part_changed || block_changed || (system && type_changed))))
        update_variation_mixer(!system || conn_changed);
    // A reset sets the units up again: a system-connection return and
    // its sends then slew up from 0 (seen after a GM2 On, which keeps the effect RAM) [meas].
    if (ram.restarts != restarts_ && system)
        for (Slew* sl : {&var_ret_l_, &var_ret_r_, &var_rev_l_, &var_rev_r_, &var_cho_l_, &var_cho_r_}) sl->cur = 0;
    restarts_ = ram.restarts;
}

void SystemFx::update_chorus_mixer() {
    // (2006LE: same tables): return and chorus → reverb per side.
    const auto& r = ram_;
    const int pan = std::clamp<int>(r[kChorusBase + 0x0D], 1, 127);
    const int pan_l = tb_.kMixerPan[static_cast<std::size_t>(128 - pan)];
    const int pan_r = tb_.kMixerPan[static_cast<std::size_t>(pan)];
    const int ret = tb_.kMixerLevel[std::min<std::uint8_t>(r[kChorusBase + 0x0C], 127)];
    const int send = tb_.kMixerLevel[std::min<std::uint8_t>(r[kChorusBase + 0x0E], 127)] + 0x18;
    chorus_return_l_ = static_cast<float>(mixer_word(tb_, ret + pan_l)) / 512.0f * model_.chorus_return_scale;
    chorus_return_r_ = static_cast<float>(mixer_word(tb_, ret + pan_r)) / 512.0f * model_.chorus_return_scale;
    chorus_send_l_.target = mixer_word(tb_, send + pan_l);
    chorus_send_r_.target = mixer_word(tb_, send + pan_r);
}

void SystemFx::sync_parameters() {
    const midi::xg::EffectData& ram = *ctx_.shared.effect1;
    std::array<std::uint8_t, 0x36> now{};
    for (std::uint8_t i = 0; i <= 0x15; ++i) {
        if (i == 0x0E || i == 0x0F) continue;
        now[i] = ram[i];
    }
    for (std::uint8_t i = 0x20; i <= 0x35; ++i) {
        if (i == 0x2F) continue;
        now[i] = ram[i];
    }
    const bool first = first_sync_;
    first_sync_ = false;
    const std::uint32_t chorus_writes_before = chorus_type_writes_;

    for (const std::uint8_t base : {std::uint8_t{0}, kChorusBase}) {
        const Block block = base == 0 ? Block::Reverb : Block::Chorus;
        const std::uint32_t writes =
            block == Block::Reverb ? ram.reverb_type_writes : ram.chorus_type_writes;
        std::uint32_t& seen = block == Block::Reverb ? reverb_type_writes_ : chorus_type_writes_;
        const bool type_changed = first || writes != seen || now[base] != ram_[base] ||
                                  now[base + 1] != ram_[base + 1];
        seen = writes;
        // Reverb return / pan: the engine computes the output all-pass words and sends them
        // itself; a type change afterwards sends the effect control's own image.
        const bool return_changed =
            block == Block::Reverb && (first || now[0x0C] != ram_[0x0C] || now[0x0D] != ram_[0x0D]);
        const auto send_return = [&] {
            std::uint8_t level = std::min<std::uint8_t>(now[0x0C], 127);
            if (model_.mu) level = mu_return_level(Block::Reverb, now[0x00], now[0x01], level);
            ctrl_.set_reverb_return(static_cast<std::uint16_t>(std::clamp<int>(now[0x0D], 1, 127)), level);
        };
        if (return_changed) send_return();
        if (type_changed) {
            if (block == Block::Reverb) set_reverb_type(now[base], now[base + 1]);
            else set_chorus_type(now[base], now[base + 1]);
            // The patched DLL also runs the return update after a type change.
            if (block == Block::Reverb && model_.mu) send_return();
        }
        // A type change loads the type's defaults into the effect control (and the RAM reloads the
        // same table); RAM bytes that differ were written after it in this block.
        bool changed = type_changed || return_changed;
        for (std::size_t i = 0; i < 16; ++i) {
            const auto idx = static_cast<std::uint8_t>(i);
            const std::uint8_t a = static_cast<std::uint8_t>(base + param_offset(i));
            const bool differs =
                type_changed ? now[a] != ctrl_.parameter(block, idx) : now[a] != ram_[a];
            if (!differs) continue;
            std::uint16_t v = now[a];
            if (block == Block::Chorus && i == 3 && model_.flanger_delay_63 && now[base] == 0x43)
                v = std::min<std::uint16_t>(v, 63);
            changed |= ctrl_.set_parameter(block, idx, v);
        }
        if (changed) {
            if (block == Block::Reverb) reverb_.load(ctrl_);
            else chorus_.load(ctrl_);
        }
    }
    const bool mixer_changed = first || now[0x2C] != ram_[0x2C] || now[0x2D] != ram_[0x2D] ||
                               now[0x2E] != ram_[0x2E];
    const bool chorus_type_changed = !first && (chorus_type_writes_ != chorus_writes_before ||
                                                now[0x20] != ram_[0x20] || now[0x21] != ram_[0x21]);
    for (std::size_t i = 0; i < now.size(); ++i) ram_[i] = now[i];
    if (mixer_changed) update_chorus_mixer();
    sync_variation(first);
    // A chorus type change restarts the variation → chorus send and recomputes its target
    // with the new chorus input mode (then).
    if (chorus_type_changed && ram_[0x5A] == 1) update_variation_mixer(false);
}

float SystemFx::advance_fade(float& fade, std::size_t frames) const {
    const auto a = static_cast<float>(std::pow(fade_pole_, static_cast<double>(frames)));
    const float start = fade;
    fade = std::min(1.0f, fade * a + (1.0f - a));
    return (fade - start) / static_cast<float>(frames);
}

void SystemFx::tick() {
    reverb_in_.step(model_.reverb_input_step ? model_.reverb_input_step : model_.input_step);
    chorus_in_.step(model_.chorus_input_step ? model_.chorus_input_step : model_.input_step);
    var_in_.step(model_.variation_input_step ? model_.variation_input_step : model_.input_step);
    chorus_send_l_.step(model_.send_step);
    chorus_send_r_.step(model_.send_step);
    // The insertion part's volume / expression / pan / sends reach the return only when its
    // voices are updated, i.e. while they
    // sound: modelled as "the variation input was non-silent since the last tick".
    if (const midi::Part* part = insertion_part()) {
        if (var_input_seen_) update_variation_mixer(false);
        if (model_.variation_control_ticked) variation_control(*part);
    }
    var_input_seen_ = false;
    for (Slew* sl : {&var_ret_l_, &var_ret_r_, &var_rev_l_, &var_rev_r_, &var_cho_l_, &var_cho_r_})
        sl->step(model_.send_step);
}

void SystemFx::process(Buses& buses, std::size_t frames) {
    sync_parameters();
    if (model_.insertion_update_in_block) {
        if (!var_input_seen_ &&
            std::max(max_abs(buses.variation_l.data(), frames), max_abs(buses.variation_r.data(), frames)) > 0.0f)
            var_input_seen_ = true;
        const midi::Part* part = insertion_part();
        const int att = part ? model_.insertion_attenuation(*ctx_.tables, ctx_.shared, *part) : 0;
        if (part && part == ins_part_seen_ && att != ins_att_seen_) update_variation_mixer(false, true);
        ins_part_seen_ = part;
        ins_att_seen_ = att;
    }
    // The 2006LE applies the control depths in the block of the controller event.
    if (!model_.variation_control_ticked)
        if (const midi::Part* part = insertion_part()) variation_control(*part);
    tick_acc_ += static_cast<float>(frames);
    while (tick_acc_ >= tick_len_) {
        tick_acc_ -= tick_len_;
        tick();
    }

    const bool reverb_on = ctrl_.reverb_algorithm() != 0;
    const bool chorus_on = ctrl_.chorus_algorithm() != 0;
    const float threshold = model_.bus_unit;

    // XG2.0 insertion chains first: they add into the dry and send buses.
    if (!ctx_.dll_exact) insertion_.process(buses, frames);

    // Input gains of every unit first, then variation → chorus → reverb.
    if (reverb_on) {
        const float gain = reverb_input_gain();
        for (std::size_t n = 0; n < frames; ++n) rev_in_[n] = gain * buses.reverb[n];
    }
    if (chorus_on) {
        const float gain = chorus_input_gain();
        for (std::size_t n = 0; n < frames; ++n) {
            cho_l_[n] = gain * buses.chorus_l[n];
            cho_r_[n] = gain * buses.chorus_r[n];
        }
    }

    if (!(ctx_.dll_exact && model_.variation_inert)) process_variation(buses, frames, reverb_on, chorus_on);

    if (chorus_on) {
        if (!chorus_running_ &&
            std::max(max_abs(cho_l_.data(), frames), max_abs(cho_r_.data(), frames)) >= threshold) {
            chorus_running_ = true;
            chorus_quiet_ = 0;
        }
        if (chorus_running_) {
            chorus_.process(cho_l_.data(), cho_r_.data(), frames);
            const float g0 = chorus_fade_;
            const float step = advance_fade(chorus_fade_, frames);
            const float rl = chorus_return_l_, rr = chorus_return_r_;
            float g = g0;
            for (std::size_t n = 0; n < frames; ++n, g += step) {
                buses.dry_l[n] += g * rl * cho_l_[n];
                buses.dry_r[n] += g * rr * cho_r_[n];
            }
            if (reverb_on) {
                const float sl = word128(chorus_send_l_.cur) * model_.chorus_send_scale;
                const float sr = word128(chorus_send_r_.cur) * model_.chorus_send_scale;
                g = g0;
                if (sl != 0.0f || sr != 0.0f)
                    for (std::size_t n = 0; n < frames; ++n, g += step)
                        rev_in_[n] += g * (sl * cho_l_[n] + sr * cho_r_[n]);
            }
            if (std::max(max_abs(cho_l_.data(), frames), max_abs(cho_r_.data(), frames)) >= threshold)
                chorus_quiet_ = 0;
            else if ((chorus_quiet_ += frames) >= chorus_hold_)
                chorus_running_ = false;
        }
    }

    if (reverb_on) {
        if (!reverb_running_ && max_abs(rev_in_.data(), frames) >= threshold) {
            reverb_running_ = true;
            reverb_quiet_ = 0;
        }
        if (reverb_running_) {
            reverb_.process(rev_in_.data(), rev_r_.data(), frames);
            float g = reverb_fade_;
            const float step = advance_fade(reverb_fade_, frames);
            for (std::size_t n = 0; n < frames; ++n, g += step) {
                buses.dry_l[n] += g * rev_in_[n];
                buses.dry_r[n] += g * rev_r_[n];
            }
            if (std::max(max_abs(rev_in_.data(), frames), max_abs(rev_r_.data(), frames)) >= threshold)
                reverb_quiet_ = 0;
            else if ((reverb_quiet_ += frames) >= reverb_hold_)
                reverb_running_ = false;
        }
    }
}

void SystemFx::variation_control(const midi::Part& part) {
    const bool ticked = model_.variation_control_ticked;
    const int target = control_offset(&ram_[0x5C], part, ticked ? ControlLaw::Syxg50 : ControlLaw::Centred);
    // The 2006LE re-applies the offset after a type change (its defaults replace the
    // parameter); with no offset that changes nothing.
    const bool reapply = var_ctrl_dirty_ && target != 0;
    var_ctrl_dirty_ = false;
    if (target == var_ctrl_ && !reapply) return;
    // the offset moves by 4 per tick; the 2006LE jumps.
    var_ctrl_ = ticked ? control_step(var_ctrl_, target) : target;
    // The controlled parameter (none for "no effect"); its base is the RAM value (syxg50:
    // the low byte, added unscaled).
    const int index = controlled_parameter(ram_[0x40]);
    if (index < 0) return;
    const auto i = static_cast<std::uint8_t>(index);
    const std::size_t a = 0x42u + 2u * i;
    int v;
    if (ticked) {
        v = std::clamp(ram_[a + 1] + var_ctrl_, 0, 127);
        // The effect control rejects a value outside the parameter's range: the
        // parameter keeps the last value it accepted (dry/wet 1..127 stops at the last step
        // above 0) [meas].
        std::uint16_t lo = 0, hi = 0;
        if (!ctrl_.parameter_range(Block::Variation, i, lo, hi) || v < lo || v > hi) return;
    } else {
        std::uint16_t lo = 0, hi = 0;
        if (!ctrl_.parameter_range(Block::Variation, i, lo, hi)) return;
        v = std::clamp(ram_[a] * 128 + (ram_[a + 1] & 0x7F) + scale_control(var_ctrl_, lo, hi), int{lo}, int{hi});
    }
    if (ctrl_.set_parameter(Block::Variation, i, static_cast<std::uint16_t>(v))) variation_.load(ctrl_);
}

void SystemFx::process_variation(Buses& buses, std::size_t frames, bool reverb_on, bool chorus_on) {
    if (!variation_.present()) return;
    const float threshold = model_.bus_unit;
    const float gain = variation_input_gain();
    if (!var_input_seen_ &&
        std::max(max_abs(buses.variation_l.data(), frames), max_abs(buses.variation_r.data(), frames)) > 0.0f)
        var_input_seen_ = true;
    for (std::size_t n = 0; n < frames; ++n) {
        var_l_[n] = gain * buses.variation_l[n];
        var_r_[n] = gain * buses.variation_r[n];
    }
    if (!var_running_ &&
        std::max(max_abs(var_l_.data(), frames), max_abs(var_r_.data(), frames)) >= threshold) {
        var_running_ = true;
        var_quiet_ = 0;
    }
    if (!var_running_) return;
    variation_.process(var_l_.data(), var_r_.data(), frames);
    // Post-mix: return into the dry pair, sends into the reverb
    // input and the chorus pair, all under the unit's fade.
    const float g0 = var_fade_;
    const float step = advance_fade(var_fade_, frames);
    const float rl = variation_return(false), rr = variation_return(true);
    float g = g0;
    for (std::size_t n = 0; n < frames; ++n, g += step) {
        buses.dry_l[n] += g * rl * var_l_[n];
        buses.dry_r[n] += g * rr * var_r_[n];
    }
    if (reverb_on) {
        const float sl = variation_to_reverb(false), sr = variation_to_reverb(true);
        g = g0;
        if (sl != 0.0f || sr != 0.0f)
            for (std::size_t n = 0; n < frames; ++n, g += step)
                rev_in_[n] += g * (sl * var_l_[n] + sr * var_r_[n]);
    }
    if (chorus_on) {
        const float cl = variation_to_chorus(false), cr = variation_to_chorus(true);
        g = g0;
        if (cl != 0.0f || cr != 0.0f)
            for (std::size_t n = 0; n < frames; ++n, g += step) {
                cho_l_[n] += g * cl * var_l_[n];
                cho_r_[n] += g * cr * var_r_[n];
            }
    }
    if (std::max(max_abs(var_l_.data(), frames), max_abs(var_r_.data(), frames)) >= threshold)
        var_quiet_ = 0;
    else if ((var_quiet_ += frames) >= var_hold_)
        var_running_ = false;
}

} // namespace hvyxg::fx
