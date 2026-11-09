// Copyright (C) 2026 Kevin H. Patterson
// SPDX-License-Identifier: BSD-3-Clause (see LICENSE)
#include "syxg2006le/voice_setup.hpp"

#include <algorithm>

#include "midi/part.hpp"
#include "syxg2006le/tbl/dll_tables.hpp"

namespace hvyxg::syxg2006le {

using namespace tbl;

namespace {

/// an XG ±64 parameter (127 → +64).
int centered(int v) { return v >= 127 ? 64 : v - 64; }
/// 7-bit signed fine tune → cents, trunc(s7 · 25 / 32).
int fine_cents(std::uint8_t raw) { return sext7(raw) * 25 / 32; }

/// A seg5 curve term `T · scale / div` rounded toward zero (16-bit products in the DLL).
int curve_term(const BankData& bank, std::uint16_t row, int index, int scale, int div) {
    return bank.curve(row, index) * scale / div;
}

/// an EG level scaled by the block depth (64 = 100 %) and the velocity
/// depth scale `vs` (0x80 = 100 %).
int scaled_level(int depth, int level, int vs) {
    if (level == 0 || depth == 0 || vs == 0) return 0;
    if (depth == 64) return level * vs * 2 / 256;
    return depth * level * vs / 8192;
}

/// The velocity depth scale of the pitch / filter EG: 0x80
/// without a curve, else a negative curve value + 0x81, times (scale + 1) / 16.
int depth_scale(const BankData& bank, std::uint16_t row, int vel_index, int scale) {
    if (row == kNoCurve) return 0x80;
    int t = bank.curve(row, vel_index);
    if (t == 0) return 0;
    if (t < 0) t += 0x81;
    return (scale + 1) * t / 16;
}

/// XG ±64 EG-time offsets of the filter EG:
/// v < 64 raises the rate to at least the table entry, v > 64 lowers it to at most it.
int feg_part_rate(const dll::Tables& tb, int rate, int v) {
    if (v == 64) return rate;
    const int t = tb.kFegRateLimit[static_cast<std::size_t>(v & 0x7F)];
    return v > 64 ? std::min(rate, t) : std::max(rate, t);
}

/// PEG attack / release time.
int peg_part_rate(const dll::Tables& tb, int rate, int v) {
    if (v == 64) return rate;
    const int s = centered(v);
    if (s < 0) return rate - s;
    return std::min(rate, static_cast<int>(tb.kPegTimeCap[static_cast<std::size_t>(s)]));
}

/// XG EG attack / decay / release as a PEG rate offset: − (v − 64) / 2 (C division).
int half_offset(int v) { return v == 64 ? 0 : -(centered(v) / 2); }

int clamp_rate(int r) { return std::clamp(r, 0, 127); }

/// Soft pedal (CC67 ≥ 64 at note-on, part offset 0x7b): its terms are scaled by the part's
/// soft-pedal depth (part offset 0x109, default 64 = 100 %):
/// t + trunc((depth − 64)·t / 64). No MIDI message of the DLL writes the depth (only an
/// internal 4-byte message, 0x51 01, whose source was not found), so it stays at 64.
constexpr int kSoftPedalDepth = 64;
int soft_scaled(int t) {
    const int d = centered(kSoftPedalDepth);
    return d == 0 ? t : t + d * t / 64;
}
bool soft_pedal(const SetupContext& c) { return c.part->controller.soft; }
int clamp_level(int l) { return std::clamp(l, -2400, 2400); }

ModEgParams pitch_eg(const SetupContext& c) {
    const PitchEgBlock& b = c.element->peg;
    const auto& ram = *c.part->ram;
    const bool drum = c.is_drum();
    ModEgParams p;
    // inactive when the element levels are 0 and the part PEG initial level is
    // at its centre (drum voices count as centre).
    const int part_init = drum ? 64 : ram.pitch_eg_init_level;
    if (!b.active() && part_init == 64) return p;
    p.active = true;
    const auto& bank = *c.bank;
    // Rate key / velocity sensitivity.
    int key_term = 0;
    if (b.rate_key_sens != 0)
        key_term = (c.ev.scale_key - b.rate_key_center) * b.rate_key_sens * 128 / 2032;
    int vel_term = 0;
    if (b.rate_velocity_sens >= 1)
        vel_term = c.ev.vel1 * b.rate_velocity_sens / 16;
    else if (b.rate_velocity_sens < 0)
        vel_term = (0x81 - c.ev.vel1) * -b.rate_velocity_sens / 16;
    const int vs = depth_scale(bank, b.depth_velocity_curve, c.ev.vel_index(), b.depth_curve_scale);
    const auto level = [&](int raw) { return clamp_level(scaled_level(b.depth, raw, vs)); };

    int init = level(b.initial_level);
    if (!drum && ram.pitch_eg_init_level != 64)
        init += centered(ram.pitch_eg_init_level) * 75 / 8;
    p.level = {clamp_level(init), level(b.level[0]), level(b.level[1]), level(b.level[2])};
    p.delay = std::min<int>(b.delay, 1000);

    int r1 = clamp_rate(b.rate[0] + key_term + vel_term);
    if (!drum) r1 = peg_part_rate(*c.tb, r1, ram.pitch_eg_attack_time);
    r1 += half_offset(ram.eg_attack_time);
    p.rate[0] = clamp_rate(r1);
    p.rate[1] = clamp_rate(b.rate[1] + key_term + vel_term);
    p.rate[2] = clamp_rate(clamp_rate(b.rate[2] + key_term + vel_term) + half_offset(ram.eg_decay_time));

    int rr = clamp_rate(b.release_rate + key_term + vel_term);
    if (!drum) rr = peg_part_rate(*c.tb, rr, ram.pitch_eg_release_time);
    rr += half_offset(ram.eg_release_time);
    p.release_rate = clamp_rate(rr);
    int rl = level(b.release_level);
    if (!drum && ram.pitch_eg_release_level != 64)
        rl += centered(ram.pitch_eg_release_level) * 75 / 8;
    p.release_level = clamp_level(rl);
    return p;
}

ModEgParams filter_eg(const SetupContext& c) {
    const FilterBlock& d = c.element->filter;
    const auto& ram = *c.part->ram;
    ModEgParams p;
    if (!d.eg_active()) return p;
    p.active = true;
    const auto& bank = *c.bank;
    const int vi = c.ev.vel_index();
    const int key_term = curve_term(bank, d.rate_key_curve, c.ev.scale_key, (d.scales[0] & 15) + 1, 16);
    const int vel_term = curve_term(bank, d.rate_velocity_curve, vi, (d.scales[2] & 15) + 1, 16);
    const int vs = depth_scale(bank, d.depth_velocity_curve, vi, d.scales[1] >> 4);
    for (std::size_t i = 0; i < 4; ++i) p.level[i] = scaled_level(d.depth, d.level[i], vs);
    const int delay_term =
        bank.curve(d.delay_velocity_curve, vi) * ((d.scales[1] & 15) + 1) * 125 / 256;
    p.delay = std::clamp(std::min<int>(d.delay, 1000) - delay_term, 0, 1000);
    const int rate1_term = curve_term(bank, d.rate1_velocity_curve, vi, (d.scales[2] >> 4) + 1, 16);

    // Rate 1: an XG EG attack below 64 is ignored when the
    // EG falls during the attack (L1 < L0).
    int r1 = clamp_rate(d.rate[0] + key_term + rate1_term);
    const int atk = ram.eg_attack_time;
    if (!(atk < 64 && p.level[1] < p.level[0])) r1 = feg_part_rate(*c.tb, r1, atk);
    p.rate[0] = clamp_rate(r1);
    p.rate[1] = clamp_rate(d.rate[1] + vel_term + key_term);
    p.rate[2] = clamp_rate(feg_part_rate(*c.tb, clamp_rate(d.rate[2] + vel_term + key_term), ram.eg_decay_time));
    // Release: the damper selects the rate and level at note-off; the
    // XG EG release applies with the damper up only.
    p.release_rate = clamp_rate(feg_part_rate(*c.tb, clamp_rate(d.release_rate + vel_term + key_term),
                                              ram.eg_release_time));
    p.release_rate_damper = clamp_rate(d.damper_release_rate + vel_term + key_term);
    p.release_level = scaled_level(d.depth, d.release_level, vs);
    p.release_level_damper = scaled_level(d.depth, d.damper_release_level, vs);
    return p;
}

int pitch_cents(const SetupContext& c) {
    const SampleHeader& s = c.element->sample;
    const Group& g = *c.group;
    int cents = 0;
    if (!s.fixed_pitch) {
        cents = (c.ev.pitch_key - s.root_key) * 100 + c.ev.cents;
        cents += fine_cents(g.raw[5]);
        // The group pitch curve is the raw signed byte, in cents (none: 0).
        cents += c.bank->raw_curve(g.pitch_curve, c.ev.scale_key);
        const PitchEgBlock& b = c.element->peg;
        cents += b.coarse * 100 + b.fine * 25 / 32;
    }
    cents += s.tune_cents();
    const auto& ram = *c.part->ram;
    if (!c.is_drum()) {
        cents += ram.scale_tuning[c.ev.note % 12] - 64;
    } else {
        // drum coarse / fine;: velocity pitch sense.
        const auto& d = *c.drum;
        // Voice type 6 (the GS kits, MSB 118) has its own pitch class: the coarse byte counts from the note's pitch key (60) instead of 64
        // (the GS kit entries store 60) [code; meas: kit 10 at coarse 0x3C / 0x40 / 0x44 / 0x30].
        const int coarse_centre = c.voice->type == 6 ? c.ev.pitch_key : 64;
        cents += (d.pitch_coarse - coarse_centre) * 100 + (d.pitch_fine - 64);
        if (d.velocity_pitch_sense != 64)
            cents += centered(d.velocity_pitch_sense) * (c.ev.vel1 - 100) * 100 / 448;
    }
    return cents;
}

int level_units(const SetupContext& c, int* velocity_level) {
    const auto& bank = *c.bank;
    const AmpEgBlock& a = c.element->amp;
    int att = c.zone->level + c.element->sample.level + c.element->element.level;
    att -= curve_term(bank, a.level_key_curve, c.ev.scale_key, (a.scales[0] >> 4) + 1, 8);
    const int vel_term = -curve_term(bank, a.level_velocity_curve, c.ev.vel_index(), (a.scales[1] >> 4) + 1, 8);
    if (velocity_level) *velocity_level = vel_term;
    att += vel_term;
    // the soft pedal's key and velocity curves, scaled by the depth; the scaled
    // velocity part also joins output (the voice-steal score).
    if (soft_pedal(c)) {
        const int soft_key = -curve_term(bank, a.soft_key_curve, c.ev.scale_key, (a.scales[3] >> 4) + 1, 8);
        const int soft_vel = -curve_term(bank, a.soft_velocity_curve, c.ev.vel_index(), (a.scales[1] & 15) + 1, 8);
        att += soft_scaled(soft_key + soft_vel);
        if (velocity_level) *velocity_level += soft_scaled(soft_vel);
    }
    // C[0x1c] is indexed by an unknown note-event field;
    // taken as 0 (the curve is unused in this bank).
    att -= curve_term(bank, a.held_time_curve, 0, (a.scales[4] >> 4) + 1, 8);
    att += c.group->level + c.voice->level;
    if (c.is_drum())
        att += c.tb->kVolumeAtt[static_cast<std::size_t>(c.drum->level >= 127 ? 255 : 2 * c.drum->level)];
    return std::clamp(att, 0, 255);
}

AmpEgParams amp_eg(const SetupContext& c) {
    const auto& bank = *c.bank;
    const AmpEgBlock& a = c.element->amp;
    const auto& ram = *c.part->ram;
    const bool drum = c.is_drum();
    const int vi = c.ev.vel_index();
    AmpEgParams p;
    p.damper_mode = c.voice->damper_mode;
    p.type6 = c.voice->type == 6;
    // rate scaling by key and velocity (all rates but attack).
    p.rate_scaling = curve_term(bank, a.rate_key_curve, c.ev.scale_key, (a.scales[0] & 15) + 1, 16) +
                     curve_term(bank, a.rate_velocity_curve, vi, (a.scales[2] & 15) + 1, 16);
    const int ks = p.rate_scaling;
    const int xg_attack = ram.eg_attack_time, xg_decay = ram.eg_decay_time;

    int attack = curve_term(bank, a.attack_velocity_curve, vi, (a.scales[1] & 15) + 1, 16) + a.attack_rate;
    if (drum) attack += c.drum->eg_attack_rate - 64;
    p.attack_base = std::clamp(attack, 0, 0x80);
    p.drum = drum;
    const bool pre_instant = p.attack_base >= 0x80;
    p.attack = xg_attack_rate(*c.tb, p, xg_attack);
    // An instant attack (start at L1) survives only if the XG attack left the rate at its
    // maximum (measured: XG attack 100 on an instant-attack piano ramps up from L0).
    const bool instant = pre_instant && p.attack >= 0x7E;

    int d1 = ks + curve_term(bank, a.decay1_velocity_curve, vi, (a.scales[2] >> 4) + 1, 16) + a.decay1_rate;
    if (drum) d1 += c.drum->eg_decay1_rate - 64;
    p.decay1_base = std::clamp(d1, 1, 127);
    int d2 = ks + a.decay2_rate;
    if (drum) d2 += c.drum->eg_decay2_rate - 64;
    p.decay2_base = std::clamp(d2, 1, 127);
    p.decay3_base = a.decay3_rate ? std::clamp(a.decay3_rate + ks, 1, 127) : 0;
    apply_xg_decay(p, xg_attack, xg_decay);

    p.level = {instant ? a.level_1 : a.level_0, a.level_1, a.level_2, a.level_3};
    p.release_up = a.release_rate;
    p.release_down = a.damper_release_rate;
    return p;
}

LfoParams lfo(const SetupContext& c) {
    const LfoBlock& a = c.element->lfo;
    const auto& ram = *c.part->ram;
    LfoParams p;
    // (+ the part vibrato parameters).
    p.wave = a.wave;
    p.key_sync = a.key_sync;
    p.phase_deg = c.tb->kLfoPhaseDegrees[std::min<std::size_t>(a.phase, c.tb->kLfoPhaseDegrees.size() - 1)];
    int v = lfo_speed_value(*c.tb, a.speed);
    const int w = ram.vibrato_rate & 0x7F;
    if (w < 64) v = std::min<int>(v, c.tb->kVibratoRate[static_cast<std::size_t>(w)]);
    else if (w > 64) v = std::max<int>(v, c.tb->kVibratoRate[static_cast<std::size_t>(w)]);
    p.speed = std::clamp(v, 6, 1016);
    // XG vibrato depth through the ±64 table (127 → +400 cents).
    p.pitch_depth = std::clamp(std::clamp<int>(a.pitch_depth, 0, 400) + centered(ram.vibrato_depth) * 25 / 4, 0, 400);
    p.amp_depth = std::min<int>(a.amp_depth, 128);
    p.filter_depth = std::min<int>(a.filter_depth, 4800);
    int delay = std::min<int>(a.delay, 500);
    const int dw = ram.vibrato_delay & 0x7F;
    if (dw < 64) delay = std::min<int>(delay, c.tb->kVibratoDelay[static_cast<std::size_t>(dw)]);
    else if (dw > 64) delay = std::max<int>(delay, c.tb->kVibratoDelay[static_cast<std::size_t>(dw)]);
    p.delay = std::clamp(delay, 0, 500);
    p.fade = a.fade_in;
    p.fade_flags = a.fade_flags;
    p.invert = a.invert;
    return p;
}

FilterParams filter(const SetupContext& c) {
    const auto& bank = *c.bank;
    const FilterEntry& e = c.element->filter.filter[0];
    FilterParams p;
    // (identity except 10 → 2).
    p.type = e.type == 10 ? 2 : e.type;
    const int vi = c.ev.vel_index();
    int cutoff = std::clamp<int>(e.cutoff, 9600, 19200);
    cutoff += curve_term(bank, e.cutoff_key_curve, c.ev.scale_key, ((e.scale_a >> 4) + 1) * 75, 16);
    cutoff += curve_term(bank, e.cutoff_velocity_curve, vi, ((e.scale_a & 15) + 1) * 75, 16);
    // the soft pedal's velocity curve (plus the shared cutoff key
    // curve D[0x26], which is "none" throughout this bank), scaled by the depth.
    if (soft_pedal(c))
        cutoff += soft_scaled(curve_term(bank, e.cutoff_soft_curve, vi, ((e.scale_b >> 4) + 1) * 75, 16));
    if (c.is_drum()) {
        // (drum velocity LPF sense multiplies a base of 0: no effect).
        if (p.type == 0) cutoff += (c.drum->filter_cutoff_freq - 64) * 75;
        else if (p.type == 2) cutoff += (c.drum->hpf_cutoff_freq - 64) * 150;
    }
    p.cutoff = std::clamp(cutoff, 4800, 19200);
    int r = std::clamp<int>(e.resonance, 16, 140);
    r += (bank.curve(e.resonance_velocity_curve, vi) * ((e.scale_b & 15) + 1) * 124) >> 11;
    if (c.is_drum()) r += ((c.drum->filter_resonance - 64) * 124) >> 7;
    p.resonance = r;
    return p;
}

void pan(const SetupContext& c, VoiceParams& v) {
    const auto& el = c.element->element;
    int pan = c.zone->pan - 64 + c.group->pan;
    if (el.pan_curve != kNoCurve)
        pan += (c.bank->curve(el.pan_curve, c.ev.scale_key) * (el.pan_curve_depth + 1)) >> 4;
    if (c.is_drum()) pan += c.drum->pan - 64;
    v.pan = std::clamp(pan, 1, 127);
    v.random_pan = c.part->ram->pan == 0 || (c.is_drum() && c.drum->pan == 0);
}

} // namespace

std::uint8_t sensed_velocity(const BankData& bank, const syxg2006le::tbl::Voice& voice,
                             const midi::Part& part, std::uint8_t velocity) {
    int v = velocity & 0x7F;
    if (const auto r = bank.velocity_remap(voice.velocity_curve, velocity)) v = std::clamp<int>(*r, 1, 127);
    const int depth = part.ram->velocity_sense_depth;
    if (depth != 64) v = depth * v >> 6;
    v = v - 127 + 2 * part.ram->velocity_sense_offset;
    return static_cast<std::uint8_t>(std::clamp(v, 2, 128));
}

VoiceParams setup_voice(const SetupContext& c) {
    VoiceParams v;
    v.pitch_cents = pitch_cents(c);
    v.level = level_units(c, &v.velocity_level);
    v.key_on_delay = c.element->element.delay;
    v.peg = pitch_eg(c);
    v.feg = filter_eg(c);
    v.amp = amp_eg(c);
    v.lfo = lfo(c);
    v.filter = filter(c);
    pan(c, v);
    if (c.is_drum()) {
        v.drum_reverb = c.drum->reverb_send;
        v.drum_chorus = c.drum->chorus_send;
        v.drum_variation = c.drum->variation_send;
    }
    return v;
}

int amp_release_rate(const dll::Tables& tb, const AmpEgParams& amp, const midi::Part& part, bool drum, int pedal) {
    // C[5] and C[6] are each clamped to 1..127 first.
    const int up = std::clamp(amp.release_up, 1, 127), down = std::clamp(amp.release_down, 1, 127);
    int rate;
    if (amp.damper_mode == 3)
        rate = pedal < 64 ? up : down;
    else
        rate = up + (down - up) * pedal / 127;
    rate = std::clamp(rate + amp.rate_scaling, 1, 127);
    if (!drum) {
        const int v = part.ram->eg_release_time;
        if (v < 64 && rate <= 0x6E)
            rate = std::min(rate + 64 - v, 0x6E);
        else if (v > 64 && amp.type6) // a program-dependent cap
            rate = std::min<int>(rate, tb.kReleaseCap6[std::min<std::size_t>(
                                           tb.kReleaseOffset6[part.ram->program_number & 0x7F] + (v - 64u), 127)]);
        else if (v > 64)
            rate = std::min<int>(rate, tb.kReleaseCap[static_cast<std::size_t>(v - 64)]);
    }
    return rate;
}

int xg_attack_rate(const dll::Tables& tb, const AmpEgParams& p, int xg_attack) {
    int attack = p.attack_base;
    if (xg_attack < 64)
        attack += 64 - xg_attack;
    else if (xg_attack > 64)
        attack = std::min<int>(attack, (p.type6 ? tb.kAttackCap6 : tb.kAttackCap)[static_cast<std::size_t>(xg_attack - 64)]);
    return std::clamp(attack, 0, 0x7E);
}

void apply_xg_decay(AmpEgParams& p, int xg_attack, int xg_decay) {
    int d1 = p.decay1_base;
    if (!p.drum && xg_attack > 64)
        d1 = std::min(d1, p.type6 ? ((127 - xg_attack) >> 1) + 4 : (128 - xg_attack) / 2 + 4);
    p.decay1 = std::clamp(d1, 1, 127);
    // Type 6 shifts instead of dividing and leaves decay 3 alone.
    const auto xg_decay_rule = [&](int r) {
        const int s = xg_decay - 64;
        if (s < 0) return p.type6 ? r - (s >> 1) : r + (-s) / 2;
        if (s > 0) return p.type6 ? r - ((s * 112) >> 7) : r - s * 112 / 128;
        return r;
    };
    p.decay2 = std::clamp(xg_decay_rule(p.decay2_base), 1, 127);
    p.decay3 = p.decay3_base ? std::clamp(p.type6 ? p.decay3_base : xg_decay_rule(p.decay3_base), 1, 127) : 0;
}

} // namespace hvyxg::syxg2006le
