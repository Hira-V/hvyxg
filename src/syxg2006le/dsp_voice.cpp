// Copyright (C) 2026 Kevin H. Patterson
// SPDX-License-Identifier: BSD-3-Clause (see LICENSE)
#include "syxg2006le/dsp_voice.hpp"
#include "accel/kernels.hpp"

#include <algorithm>
#include <cmath>

#include "syxg2006le/tbl/dll_tables.hpp"
#include "syxg2006le/tbl/param_blocks.hpp"

namespace hvyxg::syxg2006le {

namespace {

constexpr std::int32_t kSilent = 0x100000; // 256 level units
constexpr std::int32_t kHoldStart = 0x80000;
constexpr std::int32_t kPeakPeriod = 4096;
constexpr int kSustainGlideRate = 80;      // incD[80] (LFO AMod glide)
constexpr int kCutoffGlideRate = 0x44;

/// move `p` by `inc` toward `target`; true once it is there. Landing exactly
/// on the target reports "not done" (the next call reports done without moving).
bool step(std::int32_t& p, std::int32_t target, std::int32_t inc) {
    if (p == target) return true;
    if (p < target) {
        p += inc;
        if (p > target) {
            p = target;
            return true;
        }
    } else {
        p -= inc;
        if (p < target) {
            p = target;
            return true;
        }
    }
    return false;
}

/// upward-only step.
/// A time in 10 ms units → samples: fild · fs · 0.01f in x87 precision, truncated (the DLL's
/// conversions; in float 80 ms would round up to 3528 samples, the DLL gets
/// 3527).
std::int32_t samples_10ms(int v, float fs) {
    return static_cast<std::int32_t>(static_cast<double>(v) * static_cast<double>(fs) * static_cast<double>(0.01f));
}

bool step_up(std::int32_t& p, std::int32_t target, std::int32_t inc) {
    if (p >= target) return true;
    p += inc;
    if (p > target) {
        p = target;
        return true;
    }
    return false;
}

/// EG position → gain.
float eg_gain(std::int32_t x) {
    if (x < 0) return 1.0f;
    if (x >= 0xFFFFF) return 0.0f;
    return attenuation_gain(static_cast<std::uint32_t>(x));
}

/// gain → EG position.
std::int32_t eg_position(float gain) {
    auto t = static_cast<std::int32_t>(gain * 131071.0f);
    if (t <= 0) return 0xFFFFF;
    t = std::min<std::int32_t>(t, 0x1FFFF);
    int n = 0;
    while (!(t & 0x10000)) {
        t <<= 1;
        ++n;
    }
    return (n << 16) | (~t & 0xFFFF);
}

std::int32_t cents_to_log(int cents) { return cents * 4096 / 75; }

/// LFO depths and limits: `(cents << 12) / 75` truncated in
/// integers, then converted to float.
float lfo_cents(int cents) { return static_cast<float>(cents_to_log(cents)); }

/// __ftol of an x87 product of two floats: exact in double, then truncated.
std::int32_t ftol(double x) { return static_cast<std::int32_t>(x); }

} // namespace

RateTables::RateTables(const tbl::dll::Tables& tables, float fs_) : tb(tables), fs(fs_) {
    using namespace tbl;
    const double k = 44100.0 / fs * 67108864.0;
    for (std::size_t r = 0; r < 128; ++r) {
        inc_attack[r] = static_cast<std::int32_t>(k / tb.kAttackTime[r]);
        inc_decay[r] = static_cast<std::int32_t>(k / tb.kDecayTime[r]);
        inc_release[r] = r == 0 ? 0 : static_cast<std::int32_t>(k / tb.kReleaseTime[r - 1]);
    }
    const float ratio = 44100.0f / fs;
    for (int i = 0; i < 1024; ++i) {
        const auto mant = static_cast<std::int32_t>(4096.0 * std::exp2(i / 1024.0));
        pitch[static_cast<std::size_t>(i)] = ratio * static_cast<float>(mant) * (1.0f / 4096.0f);
    }
    cutoff_base = static_cast<std::int32_t>(65536.0 * std::log2(static_cast<double>(ratio)));
    gate_samples = static_cast<std::int32_t>(fs * 0.03f);
    // Engine: inc = 2^26·(44100/fs) / 2^(20 − k).
    inc_pan = static_cast<std::int32_t>(k / std::ldexp(1.0, 20 - 7));
    inc_send = static_cast<std::int32_t>(k / std::ldexp(1.0, 20 - 9));
    for (std::size_t r = 0; r < 128; ++r)
        inc_portamento[r] =
            std::max<std::int32_t>(1, static_cast<std::int32_t>(4194304.0f / (fs * tb.kPortamentoTime[r])));
}

void DspVoice::set_portamento(const RateTables& t, int start_cents, int rate) {
    porta_pos_ = cents_to_log(std::clamp(start_cents, -12700, 12700));
    porta_step_ = t.inc_portamento[static_cast<std::size_t>(std::clamp(rate, 0, 127))];
}

double RateTables::random() {
    // A 35-bit maximal-length LFSR read as value / 2^35.
    const std::uint64_t bit = ((lfsr >> 34) ^ (lfsr >> 32)) & 1u;
    lfsr = ((lfsr << 1) | bit) & ((std::uint64_t{1} << 35) - 1);
    if (lfsr == 0) lfsr = 1;
    return static_cast<double>(lfsr) / static_cast<double>(std::uint64_t{1} << 35);
}

void DspVoice::init_object(int index) {
    // index·65·2^20 in a 32-bit signed product (it wraps from object 32 on),
    // divided by 360 towards zero.
    lfo_.phase = 0;
    const auto x = static_cast<std::int32_t>(static_cast<std::uint32_t>(index) * 0x4100000u);
    lfo_offset_ = static_cast<std::uint32_t>(x / 360) & 0xFFFFF;
}

void DspVoice::start(RateTables& t, const VoiceParams& p, const ElementData& el,
                     std::span<const std::int16_t> wave, const VoiceControls& c) {
    state_ = State::Delay;
    ended_ = false;
    off_.pending = false;
    xg_freeze_ = 0;
    steal_fade_ = {};
    released_ = false;
    ended_released_ = false;
    started_ = false;
    peak_ = 0.0f;
    peak_timer_ = 0;
    sample_ended_ = false;
    gate_ = 0;
    delay_counter_ = 0;
    delay_target_ = static_cast<std::int32_t>(static_cast<float>(p.key_on_delay) * t.fs * 0.01f);

    // Sample.
    const auto& h = el.sample;
    base_ = wave.data() + el.sample_base;
    pre_loop_ = h.pre_loop;
    loop_len_ = h.loop_length;
    one_shot_ = h.flag_b6;
    pos_ = h.reverse_loop ? loop_len_ - 1 : -pre_loop_;
    frac_ = 0;
    loop_tune_ = h.loop_fine * 64;
    pitch_base_ = cents_to_log(c.pitch_cents);
    porta_pos_ = 0;
    porta_step_ = 0;

    // Filter: the cutoff starts at its target.
    svf_ = {};
    cutoff_target_ = cents_to_log(c.cutoff_cents) + t.cutoff_base;
    cutoff_cur_ = cutoff_target_;
    filter_type_ = p.filter.type;
    f_prev_ = -1.0f;

    // Output stage: level, pan and sends start at their targets.
    level_x_ = c.level << 12;
    pan_x_ = {c.pan_l << 12, c.pan_r << 12};
    for (std::size_t i = 0; i < 4; ++i) send_x_[i] = std::min(c.send[i], 255) << 12;

    // Amp EG.
    amp_init(t, p.amp);

    // Pitch / filter EG.
    const auto init_mod = [&](ModEg& eg, const ModEgParams& mp) {
        eg = {};
        eg.params = mp;
        eg.inc_sustain = t.inc_decay[kSustainGlideRate];
        if (!mp.active) {
            eg.state = 7;
            return;
        }
        for (std::size_t i = 0; i < 3; ++i)
            eg.inc[i] = t.inc_decay[static_cast<std::size_t>(mp.rate[i])];
        for (std::size_t i = 0; i < 4; ++i) eg.level[i] = cents_to_log(mp.level[i]);
        eg.pos = eg.level[0];
        eg.delay_target = static_cast<std::int32_t>(static_cast<float>(mp.delay) * t.fs * 0.01f);
        eg.state = 0;
        if (mp.delay == 0) mod_advance(eg);
    };
    init_mod(peg_, p.peg);
    init_mod(feg_, p.feg);

    // LFO.
    const auto& l = p.lfo;
    lfo_.wave = l.wave;
    lfo_.invert = l.invert;
    lfo_.fade_flags = l.fade_flags;
    lfo_.inc = syxg2006le::tbl::lfo_increment(l.speed, t.fs);
    lfo_.pitch_depth = lfo_cents(l.pitch_depth);
    lfo_.amp_depth = static_cast<float>(l.amp_depth << 12);
    lfo_.filter_depth = lfo_cents(l.filter_depth);
    lfo_.delay = samples_10ms(l.delay, t.fs);
    lfo_.fade = samples_10ms(l.fade, t.fs);
    lfo_.state = lfo_.delay ? 0 : (lfo_.fade ? 1 : 2);
    lfo_.counter = 0;
    lfo_.amod = static_cast<float>(c.amod << 12);
    lfo_.sh_flag = true;
    if (l.key_sync)
        lfo_.phase = static_cast<std::uint32_t>(std::int64_t{l.phase_deg} * 0x100000 / 360);
    else
        lfo_.phase = (lfo_.phase + lfo_offset_) & 0xFFFFF;
    mixed_ = false;
    // The LFO ticks once at note-on.
    lfo_tick(t, c, true);
    // The filter's first coefficient: the cutoff with the
    // filter EG at its start level; the first block ramps from it.
    f_prev_ = filter_coefficient(t, cutoff_cur_ + (feg_.params.active ? feg_.pos : 0), c.resonance);
    if (delay_target_ == 0) state_ = State::Running;
}

void DspVoice::amp_init(const RateTables& t, const AmpEgParams& a) {
    amp_ = a;
    eg_ = {};
    stage_ticks_ = 0;
    amp_monotonic_ = a.level[1] <= a.level[2] && a.level[2] <= a.level[3];
    eg_.attack_rate = a.attack;
    eg_.decay1_rate = a.decay1;
    eg_.decay2_rate = a.decay2;
    eg_.decay3_rate = a.decay3;
    eg_.inc = {t.inc_attack[static_cast<std::size_t>(a.attack)],
               t.inc_decay[static_cast<std::size_t>(a.decay1)],
               t.inc_decay[static_cast<std::size_t>(a.decay2)],
               a.decay3 < 1 ? 0 : t.inc_decay[static_cast<std::size_t>(a.decay3)]};
    for (std::size_t i = 0; i < 4; ++i) eg_.level[i] = a.level[i] << 12;
    eg_.x = eg_.level[0];
    eg_.hold = kHoldStart;
    eg_.gain_prev = eg_.gain = eg_gain(eg_.x);
    if (eg_.level[0] != eg_.level[1]) {
        eg_.state = 1;
        eg_.recompute = true;
    } else if (a.attack == 127) {
        eg_.state = 3;
        amp_advance();
    } else {
        eg_.state = 0;
    }
}

void DspVoice::take_over_amp(const RateTables& t, const DspVoice& prev) {
    // the previous voice's EG class
    // selects how its rates and levels carry over; a released or idle voice gives a fresh
    // note-on.
    if (prev.state_ == State::Idle || prev.released_) return;
    AmpEgParams a = prev.amp_;
    a.damper_mode = amp_.damper_mode;
    if (prev.state_ == State::Delay || !prev.started_) {
        amp_init(t, a); // everything as the previous voice had it
        return;
    }
    const auto& e = prev.eg_;
    // the current level in units; on the linear stages from the gain at the
    // start of the last tick (the tick moves the current gain there first).
    const int cur = ((e.state == 1 || e.state == 9) ? eg_position(e.gain_prev) : e.x) >> 12 & 0xFF;
    const auto lv = [&](int i) { return prev.amp_.level[static_cast<std::size_t>(i)]; };
    a.level[0] = 0xFF;
    switch (e.state) {
    case 0:
    case 1: {
        // the rest of the attack, over the time it still had left.
        int r = 0x70;
        if (lv(0) != lv(1)) {
            const auto d = static_cast<std::int64_t>(std::abs(lv(1) - cur)) *
                           t.tb.kAttackTime[static_cast<std::size_t>(prev.amp_.attack)] >> 8;
            r = 0x7F;
            for (int i = 0; i < 0x7F; ++i)
                if (t.tb.kAttackTime[static_cast<std::size_t>(i)] <= d) {
                    r = i;
                    break;
                }
            r = std::clamp(r, 0, 0x70);
        }
        a.attack = r;
        break;
    }
    case 2:
    case 3:
        a.attack = 0x70;
        if (lv(1) != lv(2)) a.level[1] = cur;
        break;
    case 4:
    case 5:
        a.attack = 0x70;
        a.decay1 = 0x7F;
        a.level[1] = a.level[2] = lv(2) != lv(3) ? cur : lv(2);
        break;
    case 6:
    case 7:
        a.attack = 0x70;
        a.decay1 = a.decay2 = 0x7F;
        a.level[1] = a.level[2] = a.level[3] = prev.amp_.decay3 == 0 ? lv(3) : cur;
        break;
    default: return;
    }
    amp_init(t, a);
    // The stages a legato takeover fixed ignore later XG EG changes.
    xg_freeze_ = e.state <= 3 ? 0x80 : e.state <= 5 ? 0xC0 : 0xE0;
}

void DspVoice::set_xg_attack(const RateTables& t, int rate) {
    // the attack increment of a running EG.
    if (xg_freeze_ & 0x80) return;
    amp_.attack = rate;
    eg_.attack_rate = rate;
    eg_.inc[0] = t.inc_attack[static_cast<std::size_t>(std::clamp(rate, 0, 127))];
    if (eg_.state == 1) eg_.recompute = true;
}

void DspVoice::set_xg_decay(const RateTables& t, int d1, int d2, int d3) {
    if (!(xg_freeze_ & 0x40)) {
        amp_.decay1 = eg_.decay1_rate = d1;
        eg_.inc[1] = t.inc_decay[static_cast<std::size_t>(d1)];
    }
    if (!(xg_freeze_ & 0x20)) {
        amp_.decay2 = eg_.decay2_rate = d2;
        eg_.inc[2] = t.inc_decay[static_cast<std::size_t>(d2)];
    }
    amp_.decay3 = eg_.decay3_rate = d3;
    eg_.inc[3] = d3 < 1 ? 0 : t.inc_decay[static_cast<std::size_t>(d3)];
}

void DspVoice::mod_take_over(const RateTables& t, ModEg& eg, const ModEg& prev, bool prev_started) {
    // Pitch EG and filter EG: by the previous EG's stage; a released EG gives a fresh note-on. The levels are kept in
    // cents, as the DSP parameters are.
    if (prev.state == 8) return;
    ModEgParams p = prev.params;
    const int cur = (prev.pos * 75) >> 12;
    const auto start = [&](int delay_10ms) {
        eg = {};
        eg.params = p;
        eg.params.active = true;
        eg.inc_sustain = t.inc_decay[kSustainGlideRate];
        for (std::size_t i = 0; i < 3; ++i) eg.inc[i] = t.inc_decay[static_cast<std::size_t>(p.rate[i])];
        for (std::size_t i = 0; i < 4; ++i) eg.level[i] = cents_to_log(p.level[i]);
        eg.pos = eg.level[0];
        eg.delay_target = static_cast<std::int32_t>(static_cast<float>(delay_10ms) * t.fs * 0.01f);
        eg.state = 0;
        if (delay_10ms == 0) mod_advance(eg);
    };
    if (!prev_started) { // as the previous voice had it
        if (p.active) {
            start(p.delay);
        } else {
            eg = {};
            eg.params = p;
            eg.state = 7;
        }
        return;
    }
    switch (prev.params.active ? prev.state : 7) {
    case 0: { // the rest of the delay (10 ms units)
        const auto done = static_cast<int>(static_cast<float>(prev.delay) * 100.0f / t.fs);
        start(std::clamp(p.delay - done, 0, 2000));
        return;
    }
    case 1:
    case 2:
        p.level[0] = cur;
        break;
    case 3:
    case 4:
        p.level[0] = p.level[1] = cur;
        p.rate[0] = 0x7F;
        break;
    case 5:
    case 6:
        p.level[0] = p.level[1] = p.level[2] = cur;
        p.rate[0] = p.rate[1] = 0x7F;
        break;
    default: // 7
        p.level = {cur, cur, cur, cur};
        p.rate = {0x7F, 0x7F, 0x7F};
        break;
    }
    start(0);
}

void DspVoice::take_over_mod_egs(const RateTables& t, const DspVoice& prev) {
    if (prev.state_ == State::Idle) return;
    const bool started = prev.state_ != State::Delay && prev.started_;
    mod_take_over(t, peg_, prev.peg_, started);
    mod_take_over(t, feg_, prev.feg_, started);
}

void DspVoice::retarget(int pitch_cents) {
    pitch_base_ = cents_to_log(pitch_cents);
    porta_pos_ = 0;
    porta_step_ = 0;
}

void DspVoice::mod_advance(ModEg& eg) {
    // equal consecutive levels are skipped without a hold.
    if (eg.state == 0 && eg.pos != eg.level[1]) {
        eg.state = 1;
        return;
    }
    if (eg.state <= 1 && eg.pos != eg.level[2]) {
        eg.state = 3;
        return;
    }
    if (eg.state <= 3 && eg.pos != eg.level[3]) {
        eg.state = 5;
        return;
    }
    eg.state = 7;
}

bool DspVoice::mod_tick(ModEg& eg, bool gated) {
    switch (eg.state) {
    case 0:
        if (step_up(eg.delay, eg.delay_target, kBlock) && !gated) mod_advance(eg);
        break;
    case 1:
        if (step(eg.pos, eg.level[1], eg.inc[0]) && !gated) mod_advance(eg);
        break;
    case 3:
        if (step(eg.pos, eg.level[2], eg.inc[1]) && !gated) mod_advance(eg);
        break;
    case 5:
        if (step(eg.pos, eg.level[3], eg.inc[2]) && !gated) mod_advance(eg);
        break;
    case 7: step(eg.pos, eg.level[3], eg.inc_sustain); break;
    case 8: step(eg.pos, eg.release_level, eg.inc_release); break;
    default: break;
    }
    return true;
}

void DspVoice::amp_advance() {
    auto& e = eg_;
    int s = e.state;
    if (s <= 1) {
        if (e.level[1] != e.level[2]) {
            e.state = 3;
        } else if (e.decay1_rate != 127) {
            e.state = 2;
        } else {
            s = 2;
        }
    }
    if (s == 2 || s == 3) {
        if (e.level[2] != e.level[3]) {
            e.state = 5;
        } else if (e.decay2_rate != 127) {
            e.state = 4;
        } else {
            s = 4;
        }
    }
    if (s == 4 || s == 5) e.state = e.decay3_rate == 0 ? 7 : 6;
    e.hold = kHoldStart;
}

void DspVoice::amp_tick(const RateTables& t) {
    auto& e = eg_;
    e.gain_prev = e.gain;
    const int state_before = e.state;
    ++stage_ticks_;
    bool linear = false;
    switch (e.state) {
    case 0: {
        if (e.attack_rate == 127) {
            amp_advance();
            break;
        }
        const auto inc = static_cast<std::int32_t>(static_cast<std::int64_t>(e.inc[0]) * 128 / 255);
        if (step(e.hold, 0, std::max<std::int32_t>(inc, 1))) amp_advance();
        break;
    }
    case 1: {
        if (e.recompute) {
            e.recompute = false;
            const std::int32_t p = eg_position(e.gain_prev);
            e.x = p;
            const float d = static_cast<float>(p - e.level[1]);
            e.slope = d != 0.0f ? (eg_gain(e.level[1]) - e.gain_prev) / d : 0.0f;
            e.intercept = static_cast<float>(p) * e.slope + e.gain_prev;
            e.xstep = e.slope != 0.0f
                          ? std::abs(static_cast<std::int32_t>(static_cast<float>(e.inc[0]) *
                                                               (1.0f / 1048576.0f) / e.slope))
                          : 0x100000;
            e.xstep = std::max<std::int32_t>(e.xstep, 1);
        }
        linear = true;
        if (step(e.x, e.level[1], e.xstep)) {
            amp_advance();
            linear = false;
        }
        break;
    }
    case 2:
        if (e.decay1_rate == 127 || step(e.hold, 0, e.inc[1])) amp_advance();
        break;
    case 3:
        if (step(e.x, e.level[2], e.inc[1])) amp_advance();
        break;
    case 4:
        if (e.decay2_rate == 127 || step(e.hold, 0, e.inc[2])) amp_advance();
        break;
    case 5:
        if (step(e.x, e.level[3], e.inc[2])) amp_advance();
        break;
    case 6: step(e.x, kSilent, e.inc[3]); break;
    case 7: step(e.x, e.level[3], t.inc_decay[kSustainGlideRate]); break;
    case 8: step(e.x, kSilent, e.inc_release); break;
    case 9:
        if (e.recompute) {
            e.recompute = false;
            const std::int32_t p = eg_position(e.gain_prev);
            e.x = p;
            e.slope = e.gain_prev / static_cast<float>(kSilent - p);
            e.intercept = static_cast<float>(p) * e.slope + e.gain_prev;
            e.xstep = e.slope > 0.0f ? static_cast<std::int32_t>(static_cast<float>(e.inc_release) *
                                                                 (1.0f / 1048576.0f) / e.slope)
                                     : kSilent;
            e.xstep = std::max<std::int32_t>(e.xstep, 1);
        }
        linear = true;
        step(e.x, kSilent, e.xstep);
        break;
    default: break;
    }
    e.gain = linear ? e.intercept - static_cast<float>(e.x) * e.slope : eg_gain(e.x);
    if (e.gain <= 0.0f || (e.state == 9 && e.x >= kSilent)) {
        e.gain = 0.0f;
        e.finished = true;
    }
    if (e.state != state_before) stage_ticks_ = 0;
    // the steal-score peak restarts every 4096 samples (the timer fires on the
    // tick after its count reaches the period).
    if (peak_timer_ >= kPeakPeriod) {
        peak_timer_ = 0;
        peak_ = 0.0f;
    } else {
        peak_timer_ += kBlock;
    }
}

void DspVoice::amp_to_release() {
    auto& e = eg_;
    const bool was_attack = e.state == 1;
    if (e.linear_release) {
        e.state = 9;
        e.recompute = true;
        return;
    }
    if (was_attack) e.x = eg_position(e.gain_prev);
    e.state = 8;
}

void DspVoice::set_release_rate(const RateTables& t, int release_rate) {
    // rate ≥ 1; ≥ 0x60 releases linearly. Ignored once the linear release runs.
    if (off_.pending) {
        off_.rate = release_rate;
        return;
    }
    auto& e = eg_;
    if (e.state == 9) return;
    const int r = std::clamp(release_rate, 1, 127);
    e.inc_release = t.inc_release[static_cast<std::size_t>(r)];
    e.linear_release = r >= 0x60;
    if (released_ && e.linear_release && e.state == 8) {
        e.state = 9;
        e.recompute = true;
    }
}

void DspVoice::note_off(const RateTables& t, int release_rate, bool damper_down) {
    // The renderer acts on a note-off when the block renders, after the block's timer ticks:
    // a score refresh in the same block still sees the voice keyed on.
    (void)t;
    if (state_ == State::Idle || released_) return;
    released_ = true;
    off_ = {true, release_rate, damper_down};
}

void DspVoice::apply_note_off(const RateTables& t) {
    if (!off_.pending) return;
    off_.pending = false;
    const int release_rate = off_.rate;
    const bool damper_down = off_.damper;
    // A note released during its key-on delay or before its first block is never heard.
    if (state_ == State::Delay || !started_) {
        state_ = State::Idle;
        return;
    }
    set_release_rate(t, release_rate);
    amp_to_release();
    stage_ticks_ = 0;
    const auto release_mod = [&](ModEg& eg, bool damper) {
        if (!eg.params.active) return;
        const int rate = damper ? eg.params.release_rate_damper : eg.params.release_rate;
        const int level = damper ? eg.params.release_level_damper : eg.params.release_level;
        eg.inc_release = t.inc_decay[static_cast<std::size_t>(std::clamp(rate, 0, 127))];
        eg.release_level = cents_to_log(level);
        eg.state = 8;
    };
    release_mod(peg_, false);
    release_mod(feg_, damper_down);
}

void DspVoice::kill(const RateTables& t, int rate) {
    off_.pending = false;
    if (state_ == State::Idle) return;
    if (state_ == State::Delay || !started_) {
        state_ = State::Idle;
        return;
    }
    released_ = true;
    if (eg_.state == 9) return;
    const int r = std::max(rate, 0x60);
    eg_.inc_release = t.inc_release[static_cast<std::size_t>(std::min(r, 127))];
    eg_.linear_release = true;
    eg_.state = 9;
    eg_.recompute = true;
}

void DspVoice::lfo_tick(RateTables& t, const VoiceControls& c, bool gated) {
    auto& l = lfo_;
    float f = 1.0f;
    switch (l.state) {
    case 0:
        f = 0.0f;
        if (step_up(l.counter, l.delay, kBlock) && !gated) {
            l.state = l.fade ? 1 : 2;
            l.counter = 0;
        }
        break;
    case 1:
        // the fade-in's timer runs against the *delay* length, not the
        // fade length, while the factor is counter·(1/fade): with a delay longer than
        // the fade the factor overshoots past 1 until the delay length is reached, with a
        // shorter one it jumps to 1 there (no delay: at the end of the 30 ms gate).
        if (step_up(l.counter, l.delay, kBlock) && !gated) {
            l.state = 2;
            break;
        }
        f = static_cast<float>(l.counter) * (1.0f / static_cast<float>(l.fade));
        break;
    default: break;
    }
    // AMod glides toward the controller depth.
    const float amod_target = static_cast<float>(c.amod << 12);
    const auto glide = static_cast<float>(t.inc_decay[kSustainGlideRate]);
    if (l.amod < amod_target) l.amod = std::min(l.amod + glide, amod_target);
    else if (l.amod > amod_target) l.amod = std::max(l.amod - glide, amod_target);

    // (fade ·) element depth + controller depth in x87 precision; the pitch and
    // filter depths are stored as floats, the amp depth stays in the x87 register; each is
    // clamped to 0 .. its limit (400 / 128 / 4800 as `lfo_cents`).
    const auto faded = [&](int flag, float depth) {
        return (l.fade_flags & flag) ? static_cast<double>(f) * depth : static_cast<double>(depth);
    };
    const float dp = std::clamp(static_cast<float>(faded(4, l.pitch_depth) + lfo_cents(c.pmod)), 0.0f,
                                lfo_cents(400));
    const double da = std::clamp(faded(2, l.amp_depth) + static_cast<double>(l.amod), 0.0,
                                 static_cast<double>(128 << 12));
    const float df = std::clamp(static_cast<float>(faded(1, l.filter_depth) + lfo_cents(c.fmod)), 0.0f,
                                lfo_cents(4800));

    // Wave shapes (switch); each gives pitch / amp / filter values.
    const int w = l.wave;
    const bool inv_p = l.invert & 4, inv_a = l.invert & 2, inv_f = l.invert & 1;
    const auto sgn = [](bool inv) { return inv ? -1.0f : 1.0f; };
    float wp = 0, wa = 0, wf = 0;
    const std::uint32_t ph = l.phase;
    if (w == 0 || w == 5 || w == 9) {
        const auto saw = [&](std::uint32_t off) {
            return static_cast<float>(static_cast<std::int32_t>((ph + off) & 0xFFFFF) - 0x80000) *
                   (1.0f / 1048576.0f);
        };
        const std::uint32_t off = w == 0 ? 0x80000 : 0;
        wp = sgn(inv_p) * saw(0x80000);
        wa = (w == 0 ? -sgn(inv_a) : sgn(inv_a)) * saw(off) + 0.5f;
        wf = sgn(inv_f) * saw(off) + (w == 5 ? 0.5f : 0.0f);
    } else if (w == 1 || w == 6 || w == 10) {
        const auto tri = [&](std::uint32_t off) {
            std::uint32_t u = (ph + off) & 0xFFFFF;
            if (u & 0x80000) u ^= 0xFFFFF;
            return static_cast<float>(static_cast<std::int32_t>(u) - 0x40000) * (1.0f / 524288.0f);
        };
        const std::uint32_t off = w == 1 ? 0x40000 : 0;
        wp = sgn(inv_p) * tri(0x40000);
        wa = (w == 1 ? -sgn(inv_a) : sgn(inv_a)) * tri(off) + 0.5f;
        wf = sgn(inv_f) * tri(off) + (w == 6 ? 0.5f : 0.0f);
    } else if (w == 2 || w == 7 || w == 11) {
        const bool second = ph & 0x80000;
        wp = (second ? -0.5f : 0.5f) * sgn(inv_p);
        wa = (second != inv_a) ? 1.0f : 0.0f;
        if (w == 7) wf = (second != inv_f) ? 1.0f : 0.0f;
        else wf = (second ? 0.5f : -0.5f) * sgn(inv_f);
    } else if (w == 3 || w == 8 || w == 12) {
        if (l.sh_flag) {
            l.sh = static_cast<float>(t.random());
            l.sh_flag = false;
        }
        wp = 0.5f * l.sh * sgn(inv_p);
        wa = 0.5f - 0.5f * l.sh * sgn(inv_a);
        wf = -0.5f * l.sh * sgn(inv_f) + (w == 8 ? 0.5f : 0.0f);
    } else {
        const float s = static_cast<float>(std::sin(static_cast<double>(ph) * 6.283185307179586 / 1048576.0));
        wp = 0.5f * s * sgn(inv_p);
        wa = 0.5f - 0.5f * s * sgn(inv_a);
        wf = -0.5f * s * sgn(inv_f);
    }
    l.out_pitch = ftol(static_cast<double>(dp) * wp);
    l.out_amp = ftol(da * wa);
    l.out_filter = ftol(static_cast<double>(df) * wf);
    const bool bias = !(w >= 5 && w <= 8) && w != 12;
    l.out_bias = bias ? ftol(static_cast<double>(df) * 0.5) : 0;

    const std::uint32_t next = (ph + static_cast<std::uint32_t>(l.inc)) & 0xFFFFF;
    if ((next ^ ph) & 0xC0000) l.sh_flag = true;
    l.phase = next;
}

void DspVoice::render_samples(std::span<float, kBlock> dst, Interpolation interp, const RateTables& t) {
    // The walk and the interpolation: the resample kernel (accel/kernels.hpp).
    accel::WalkLE w;
    w.base = base_;
    w.pre_loop = pre_loop_;
    w.loop_len = loop_len_;
    w.one_shot = one_shot_;
    w.pos = pos_;
    w.frac = frac_;
    w.ended = sample_ended_;
    const auto il = interp == Interpolation::Linear ? accel::InterpLE::Linear
                    : interp == Interpolation::Hermite ? accel::InterpLE::Hermite : accel::InterpLE::Lanczos;
    accel::resample_le(w, inc_, il, t.lanczos.data(), dst.data(), dst.size());
    pos_ = w.pos;
    frac_ = w.frac;
    sample_ended_ = w.ended;
}

float DspVoice::filter_coefficient(const RateTables& t, std::int32_t cutoff, int resonance) const {
    std::int32_t lc = std::clamp<std::int32_t>(cutoff, 0x80000, 0x100000);
    lc = std::clamp<std::int32_t>(lc + lfo_.out_bias, 0x80000, 0x100000);
    // a resonance-dependent ceiling per filter type class.
    static constexpr std::array<int, 10> kTypeClass = {0, 1, 2, 2, 2, 1, 3, 3, 0, 2};
    const std::int32_t ceiling =
        filter_type_ < 10 ? std::int32_t{t.tb.kFilterCeiling
                                             [static_cast<std::size_t>(kTypeClass[static_cast<std::size_t>(filter_type_)])]
                                             [static_cast<std::size_t>(std::clamp(resonance, 0, 255))]}
                                << 8
                          : 0xFFF;
    lc = std::min(lc, ceiling);
    lc -= lfo_.out_filter;
    lc = std::max<std::int32_t>(lc, 0);
    return std::min(1.0f, static_cast<float>(256 + ((lc >> 8) & 255)) / std::ldexp(1.0f, 24 - (lc >> 16)));
}

bool DspVoice::render_begin(RateTables& t, const VoiceControls& c, Interpolation interp, float* x, SvfStep& svf) {
    apply_note_off(t);
    if (state_ == State::Idle) return false;
    // 30 ms start gate.
    const bool gated = !step_up(gate_, t.gate_samples, kBlock);
    if (state_ == State::Delay) {
        if (!(step_up(delay_counter_, delay_target_, kBlock) && !gated)) return false;
        state_ = State::Running;
    }
    started_ = true;

    lfo_tick(t, c, gated);

    // Pitch: once per block, before rendering.
    mod_tick(peg_, gated);
    pitch_base_ = cents_to_log(c.pitch_cents);
    step(porta_pos_, 0, porta_step_);
    std::int32_t b = std::clamp<std::int32_t>(pitch_base_ + porta_pos_, -0x80000, 0x80000) + peg_.pos + lfo_.out_pitch +
                     (pos_ >= 0 ? loop_tune_ : 0);
    b = std::clamp<std::int32_t>(b, -0x80000, 0x80000);
    const std::int32_t v = b >> 6;
    inc_ = static_cast<std::int32_t>(std::ldexp(t.pitch[static_cast<std::size_t>(v & 0x3FF)], (v >> 10) + 15));

    // Filter EG → filter.
    mod_tick(feg_, gated);
    cutoff_target_ = cents_to_log(c.cutoff_cents) + t.cutoff_base;
    step(cutoff_cur_, cutoff_target_, t.inc_decay[kCutoffGlideRate]);
    const float f = filter_coefficient(t, cutoff_cur_ + feg_.pos, c.resonance);
    q_ = filter_type_ == 6 || filter_type_ == 7
             ? 2.0f
             : static_cast<float>(syxg2006le::tbl::filter_damping(c.resonance));
    if (f_prev_ < 0.0f) f_prev_ = f;

    amp_tick(t);

    // Samples, then (by the caller, across a batch of voices) the filter.
    render_samples(std::span<float, kBlock>(x, kBlock), interp, t);
    // the peak of |EG gain × sample| over the block (gain ramped from the
    // previous block's value), in full-scale units; read by the steal score.
    {
        constexpr float k = 1.0f / 32768.0f;
        const float a = eg_.gain_prev * k;
        accel::peak_ramp(x, a, (eg_.gain * k - a) * (1.0f / kBlock), &peak_, kBlock);
    }
    svf.lp = &svf_.lp;
    svf.bp = &svf_.bp;
    svf.f = f_prev_;
    svf.df = (f - f_prev_) / kBlock;
    svf.q = q_;
    f_prev_ = f;
    return true;
}

void DspVoice::render_end(RateTables& t, const VoiceControls& c, std::array<float*, kOutputs> out, float* x) {
    eq.process(x, kBlock);
    const bool faded = steal_fade_.apply(x, kBlock);

    // Output stage: glide level / pan / sends, then the per-output gains
    // g(pan + send + level + amp LFO); outputs whose send + level + LFO reach 255 units are
    // muted. The reverb bus is mono: the mean of the left and right gains.
    step(level_x_, c.level << 12, t.inc_decay[kSustainGlideRate]);
    step(pan_x_[0], c.pan_l << 12, t.inc_pan);
    step(pan_x_[1], c.pan_r << 12, t.inc_pan);
    for (std::size_t i = 0; i < 4; ++i) step(send_x_[i], std::min(c.send[i], 255) << 12, t.inc_send);
    std::array<float, kOutputs> gain{};
    for (std::size_t i = 0; i < 4; ++i) {
        const std::int32_t tt = send_x_[i] + level_x_ + lfo_.out_amp;
        if (tt >= 0xFF000) continue;
        const float gl = eg_gain(pan_x_[0] + tt), gr = eg_gain(pan_x_[1] + tt);
        switch (i) {
        case 0: gain[0] = gl; gain[1] = gr; break;
        case 1: gain[2] = 0.5f * (gl + gr); break;
        case 2: gain[3] = gl; gain[4] = gr; break;
        default: gain[5] = gl; gain[6] = gr; break;
        }
    }

    // Mix: EG × gain ramps linearly from the previous block's end value. Bus
    // scale: the DLL's dry-bus → output factor −2^-13 (samples in 16-bit units).
    constexpr float kBusScale = -1.0f / 8192.0f;
    if (!mixed_) gain_prev_ = gain; // the first block starts at its own output gains
    mixed_ = true;
    // The outputs that receive anything, mixed in one pass (their ramps are independent add
    // chains, which then overlap; the same operations per output as one loop each).
    std::array<float*, kOutputs> bus{};
    std::array<float, kOutputs> ga{}, gd{};
    std::size_t k = 0;
    for (std::size_t o = 0; o < kOutputs; ++o) {
        const float a = eg_.gain_prev * gain_prev_[o] * kBusScale;
        const float target = eg_.gain * gain[o] * kBusScale;
        if (out[o] && (a != 0.0f || target != 0.0f)) {
            bus[k] = out[o];
            ga[k] = a;
            gd[k++] = (target - a) / kBlock;
        }
    }
    accel::mix_ramps(x, bus.data(), ga.data(), gd.data(), k, kBlock);
    gain_prev_ = gain;

    if (eg_.finished || sample_ended_ || faded) {
        state_ = State::Idle;
        ended_released_ = released_;
        ended_ = true;
    }
}

void DspVoice::render(RateTables& t, const VoiceControls& c, std::array<float*, kOutputs> out, Interpolation interp) {
    std::array<float, kBlock> x{};
    SvfStep svf;
    if (!render_begin(t, c, interp, x.data(), svf)) return;
    float* xp = x.data();
    accel::svf_le_lanes(xp, kBlock, &svf.lp, &svf.bp, &svf.f, &svf.df, &svf.q, 1, kBlock);
    render_end(t, c, out, xp);
}

int DspVoice::score_state() const {
    switch (state_) {
    case State::Delay: return 0;
    case State::Running: return released_ && !off_.pending ? 2 : 1;
    default: return ended_ ? (ended_released_ ? 2 : 1) : 3;
    }
}

int DspVoice::score_stage() const {
    if (state_ == State::Idle) return ended_released_ ? 5 : 3;
    if (state_ == State::Delay) return 0;
    switch (eg_.state) {
    case 0: case 1: return 0;
    case 2: case 3: return 1;
    case 4: case 5: return 2;
    case 6: return 3;
    case 7: return 4;
    default: return 5;
    }
}

int DspVoice::score_units() const {
    if (state_ == State::Idle) return 255;
    if (state_ == State::Delay) return 0;
    // gain → EG position, (pos >> 12) & 0xff.
    const auto v = static_cast<std::int64_t>(static_cast<double>(peak_) * 131071.0);
    if (v <= 0) return 255;
    if (v >= 0x20000) return 0;
    std::int64_t m = v;
    int c = 0;
    while (!(m & 0x10000)) {
        m <<= 1;
        ++c;
    }
    const auto pos = static_cast<std::int32_t>((c << 16) | (~m & 0xFFFF));
    return (pos >> 12) & 0xFF;
}

bool DspVoice::may_die(float fs) const {
    if (amp_monotonic_) return true;
    if (state_ == State::Idle) return true;
    // by stage (m34 = stage + 1): up to decay 1 the levels L1..L3 must not
    // rise, in decay 2 L2..L3; then the time in the stage (10 ms units) > 9.
    const int stage = score_stage() + (state_ == State::Delay ? 0 : 1);
    const auto& l = amp_.level;
    bool r = true;
    if (stage <= 2) r = l[1] <= l[2] && l[2] <= l[3];
    else if (stage == 3) r = l[2] <= l[3];
    if (!r) return false;
    const auto x = static_cast<std::int64_t>(static_cast<double>(stage_ticks_) * 6400.0 / static_cast<double>(fs));
    return x > 9;
}

} // namespace hvyxg::syxg2006le
