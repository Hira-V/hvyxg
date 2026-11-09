// Copyright (C) 2026 Kevin H. Patterson
// SPDX-License-Identifier: BSD-3-Clause (see LICENSE)
#include "syxg50/chip.hpp"
#include "accel/kernels.hpp"

#include <algorithm>
#include <climits>
#include <cstdint>
#include <cmath>
#include <numbers>

#include "cx_math.hpp"
#include "syxg50/dll_tables.hpp"

namespace hvyxg::syxg50 {

namespace {

/// float → int32 as the DLL's x86 conversion (cvttss2si): truncation toward zero, and
/// 0x80000000 for NaN, ±inf and anything out of range, where a plain cast is undefined
/// behaviour (an EG slope of 0 makes the step ±inf).
std::int32_t x86_trunc(float v) {
    if (!(v >= -2147483648.0f && v < 2147483648.0f)) return INT32_MIN;
    return static_cast<std::int32_t>(v);
}

/// move `cur` toward `target` by `step`; true once it is there.
bool glide(std::int32_t& cur, std::int32_t target, std::int32_t step) {
    if (cur == target) return true;
    if (cur > target) {
        cur -= step;
        if (cur >= target) return false;
    } else {
        cur += step;
        if (cur <= target) return false;
    }
    cur = target;
    return true;
}

// LFO quadrant tables, value = T[phase >> 18] ^ phase.
// Pitch/filter: saw, triangle; amplitude: saw (zero-filled),
// triangle.
constexpr std::array<std::uint32_t, 4> kLfoSaw{0, 0, 0xfff00000u, 0xfff00000u};
constexpr std::array<std::uint32_t, 4> kLfoTri{0, 0x0007ffffu, 0xfff7ffffu, 0xfff00000u};
constexpr std::array<std::uint32_t, 4> kLfoSawUni{0, 0, 0, 0};
constexpr std::array<std::uint32_t, 4> kLfoTriUni{0, 0, 0x000fffffu, 0x000fffffu};

/// 2048·2^(-i/1200), i = 0..1200.
/// Built at compile time (the same values as lround(2048 · exp2(−i / 1200)): the nearest a value
/// comes to a half is 5·10⁻⁵, far above the compile-time maths' error).
constexpr std::array<std::uint16_t, 1201> kPitchMantissa = [] {
    std::array<std::uint16_t, 1201> a{};
    for (std::size_t i = 0; i < a.size(); ++i)
        a[i] = static_cast<std::uint16_t>(cx::round_int(2048.0 * cx::exp(-static_cast<double>(i) / 1200.0 * cx::kLn2)));
    return a;
}();
static_assert(cx::table_hash(kPitchMantissa) == 0xc59b5eef0280b569ull);

/// Level glide step: decay-table entry 0x58.
constexpr int kLevelGlideRate = 0x58;

} // namespace

std::int32_t inv_lin(float gain) {
    auto v = static_cast<std::int32_t>(gain * 131071.0f);
    if (v <= 0) return kAttenOff;
    std::int32_t oct = 0;
    while ((v & 0x10000) == 0) {
        v <<= 1;
        ++oct;
    }
    return (~v & 0xffff) | (oct << 16);
}

void ChipVoice::set_pitch(int c, bool cap3) {
    if (locked_) return;
    pending_cents_ = c;
    pending_cap3_ = cap3;
    if (rt_ == nullptr) return;
    const auto& t = kPitchMantissa;
    int oct, m;
    if (c >= 0) {
        oct = c / 1200 + 1;
        m = t[static_cast<std::size_t>(1200 - c % 1200)];
    } else {
        oct = c / 1200;
        m = t[static_cast<std::size_t>((-c) % 1200)];
    }
    while (oct < -8) {
        m /= 2;
        ++oct;
    }
    oct = std::min(oct, cap3 ? 3 : 7);
    const int sh = oct + 4;
    const std::int64_t v = sh >= 0 ? static_cast<std::int64_t>(m) << sh : m >> -sh;
    inc_ = static_cast<std::int32_t>(static_cast<float>(v) * rt_->rate_ratio);
}

RateTables::RateTables(const dll::Tables& tb, float fs) : sample_rate(fs) {
    const float scale = 44100.0f / fs;
    for (std::size_t i = 0; i < 128; ++i) {
        attack[i] = static_cast<std::int32_t>(67108864.0f / tb.kAttackTime[i] * scale);
        decay[i] = static_cast<std::int32_t>(134217728.0f / tb.kDecayTime[i] * scale);
        release[i] = static_cast<std::int32_t>(134217728.0f / tb.kReleaseTime[i] * scale);
    }
    rate_ratio = scale;
}

void ChipVoice::set_wave(std::span<const std::uint8_t> wave, const WaveRef& w,
                         std::uint32_t skip) {
    wave_ = wave.data();
    wave_size_ = wave.size();
    loop_start_ = w.loop_start;
    pcm8_ = w.pcm8();
    pre_ = static_cast<std::int32_t>(w.pre > skip ? w.pre - skip : 0);
    loop_len_ = static_cast<std::int32_t>(w.loop_len);
    loop_frac_ = static_cast<std::uint32_t>(w.format & 0x3f) << 26;
    const std::size_t width = pcm8_ ? 1 : 2;
    // The wave must cover [loop_start - pre, loop_start + loop_len + 2) samples.
    ended_ = width * static_cast<std::size_t>(pre_) > loop_start_ ||
             loop_start_ + width * (static_cast<std::size_t>(loop_len_) + 2) > wave_size_;
}

void ChipVoice::set_filter(int cutoff11, int resonance5) {
    if (locked_) return;
    reg_cut_ = cutoff11 & 0x7ff;
    reg_res_ = resonance5 & 0x1f;
    q_ = 2.0f * lin((reg_res_ + 4) << 13);
    std::int32_t c = reg_cut_;
    if (c != 0) {
        c = std::min<std::int32_t>(c, 0x7ff);
        if (reg_res_ < 4) c = std::min<std::int32_t>(c, 0x7c0);
        c += 0x800;
    }
    cut_tgt_ = c << 8;
}

void ChipVoice::set_lfo(int amd, int fmd, int speed, bool triangle, int pmd) {
    if (locked_) return;
    amd_ = amd & 0x1f;
    fmd_ = fmd & 7;
    lfo_speed_ = speed & 0x3f;
    lfo_tri_ = triangle;
    pmd_ = pmd & 0xff;
    if (rt_ != nullptr) lfo_step_ = rt_->decay[32 + static_cast<std::size_t>(lfo_speed_)];
}

int ChipVoice::status() const {
    // the attack-from-level and linear-release states (0, 4) report the
    // attenuation of the envelope amplitude at the start of the last block.
    if (!active_) return 0xff;
    const std::int32_t x = (state_ == 0 || state_ == 4) ? inv_lin(env_prev_) : x_;
    int s = (x >> 14) & 0x3f;
    if (reached_) s |= 0x40;
    if (state_ == 3 || state_ == 4) s |= 0x80;
    return s;
}

void ChipVoice::set_decay(int reg500, int reg501) {
    reg500_ = reg500 & 0xff;
    reg501_ = reg501 & 0xff;
    if (active_ && !locked_) decay_setup(*rt_);
}

void ChipVoice::set_levels(int level, int pan_l, int pan_r, int dry, int reverb, int chorus,
                           int variation) {
    if (locked_) return;
    const auto atten = [](int reg) {
        const std::int32_t x = (reg & 0xff) << 12;
        return x >= 0xff000 ? kAttenOff : x;
    };
    const auto pan = [](int nib) {
        const std::int32_t x = (nib & 0xf) << 14;
        return x >= 0x3c000 ? kAttenOff : x;
    };
    level_tgt_ = atten(level);
    pan_l_tgt_ = pan(pan_l);
    pan_r_tgt_ = pan(pan_r);
    dry_ = atten(dry);
    rev_ = atten(reverb);
    cho_ = atten(chorus);
    var_ = atten(variation);
}

void ChipVoice::start(const RateTables& rt) {
    rt_ = &rt;
    set_pitch(pending_cents_, pending_cap3_);
    active_ = true;
    locked_ = false;
    done_ = false;
    pos_ = -pre_;
    frac_ = 0;
    lfo_phase_ = 0;
    lfo_step_ = rt.decay[32 + static_cast<std::size_t>(lfo_speed_)];
    cut_cur_ = cut_tgt_;
    band_ = low_ = 0.0f;
    f_new_ = f_prev_ = svf_f(cut_cur_);
    attack_setup(rt);
    level_cur_ = level_tgt_;
    pan_l_cur_ = pan_l_tgt_;
    pan_r_cur_ = pan_r_tgt_;
    update_gains(0);
    g_prev_ = g_new_;
}

float ChipVoice::svf_f(std::int32_t v) const {
    // f = ((v & 0xffff) | 0x10000) / (2^(24 - (v >> 16)) · 255).
    v = std::max<std::int32_t>(v, 0);
    const float f44 = static_cast<float>((v & 0xffff) | 0x10000) *
                      std::ldexp(1.0f, (v >> 16) - 24) / 255.0f;
    // (Bounded below 2 so an extreme sweep cannot make the SVF diverge.)
    if (rt_ == nullptr || rt_->rate_ratio == 1.0f) return std::min(f44, 1.99f);
    // Other output rates: the same pole frequency as at 44.1 kHz.
    const float fc = 44100.0f / std::numbers::pi_v<float> * std::asin(std::min(f44 * 0.5f, 1.0f));
    return 2.0f * std::sin(std::numbers::pi_v<float> * std::min(fc / rt_->sample_rate, 0.25f));
}

void ChipVoice::attack_setup(const RateTables& rt) {
    hold_ = 0x80000;
    init_level_ = (reg4c1_ & 0xff) << 12;
    const int r = reg4c0_ & 0x7f;
    if (reg4c0_ & 0x80) {
        x_ = init_level_;
        step_a_ = rt.attack[static_cast<std::size_t>(r)];
        env_prev_ = lin(x_);
        if (init_level_ == 0) {
            state_ = 3;
            env_ = env_prev_;
        } else {
            state_ = 0;
            slope_ = (1.0f - env_prev_) / static_cast<float>(init_level_);
            offset_ = 1.0f;
            step_a_ = x86_trunc((1.0f - lin(0x80000)) * 1.9073486328125e-06f * (static_cast<float>(step_a_) / slope_));
            env_ = env_prev_;
        }
    } else {
        x_ = 0;
        state_ = init_level_ != 0 ? 2 : 1;
        step_a_ = rt.decay[static_cast<std::size_t>(r)];
        env_prev_ = env_ = 1.0f;
    }
    decay_setup(rt);
}

void ChipVoice::decay_setup(const RateTables& rt) {
    target_ = reg501_ << 12;
    const int r = reg500_ & 0x7f;
    if ((reg500_ & 0x80) == 0) {
        step_d_ = rt.decay[static_cast<std::size_t>(r)];
        return;
    }
    if (state_ == 0 || state_ == 4) x_ = inv_lin(env_prev_);
    step_d_ = rt.release[static_cast<std::size_t>(r)];
    if (r < 0x60) {
        state_ = 3;
        return;
    }
    // Linear-amplitude release; the voice no longer takes register writes.
    locked_ = true;
    state_ = 4;
    if (x_ >= target_) {
        // Already at the target (the DLL divides by zero here): the voice is silent.
        x_ = target_;
        slope_ = 0.0f;
        offset_ = 0.0f;
        step_d_ = 1;
        return;
    }
    slope_ = (lin(target_) - env_prev_) / static_cast<float>(x_ - target_);
    offset_ = static_cast<float>(x_) * slope_ + env_prev_;
    step_d_ = x86_trunc((1.0f - lin(0x100000)) * 9.5367431640625e-07f * (static_cast<float>(step_d_) / slope_));
}

void ChipVoice::aeg_step() {
    env_prev_ = env_;
    reached_ = false;
    switch (state_) {
    case 0:
        if (glide(x_, 0, step_a_)) state_ = 3;
        env_ = offset_ - static_cast<float>(x_) * slope_;
        break;
    case 1:
        if (glide(hold_, 0, step_a_)) state_ = 3;
        env_ = 1.0f;
        break;
    case 2:
        if (glide(x_, init_level_, step_a_)) state_ = 3;
        env_ = lin(x_);
        break;
    case 3:
        reached_ = glide(x_, target_, step_d_);
        env_ = lin(x_);
        break;
    case 4:
        reached_ = glide(x_, target_, step_d_);
        env_ = offset_ - static_cast<float>(x_) * slope_;
        break;
    default: break;
    }
    if (state_ >= 2 && state_ <= 4 && lin(0x80000) >= env_) {
        // -48 dB: the voice ends.
        x_ = target_ = init_level_ = kAttenOff;
        env_ = 0.0f;
        ended_ = true;
        done_ = true;
    }
}

void ChipVoice::update_gains(std::int32_t am) {
    const std::int32_t lv = am + level_cur_;
    g_new_[0] = dry_ == kAttenOff ? 0.0f : lin(pan_l_cur_ + dry_ + lv);
    g_new_[1] = dry_ == kAttenOff ? 0.0f : lin(dry_ + pan_r_cur_ + lv);
    g_new_[2] = rev_ == kAttenOff ? 0.0f : lin(rev_ + lv);
    g_new_[3] = cho_ == kAttenOff ? 0.0f : lin(pan_l_cur_ + cho_ + lv);
    g_new_[4] = cho_ == kAttenOff ? 0.0f : lin(cho_ + pan_r_cur_ + lv);
    g_new_[5] = var_ == kAttenOff ? 0.0f : lin(pan_l_cur_ + var_ + lv);
    g_new_[6] = var_ == kAttenOff ? 0.0f : lin(pan_r_cur_ + var_ + lv);
}

void ChipVoice::fetch(const RateTables& rt, Interpolation interp, float* out) {
    // Fetch routines (linear, the DLL), plus a 4-point Hermite option.
    // Pitch: inc += ((PMD · lfo) >> shift) · inc >> 11, once per block.
    const std::int32_t pm =
        static_cast<std::int32_t>((lfo_tri_ ? kLfoTri : kLfoSaw)[lfo_phase_ >> 18] ^ lfo_phase_);
    const int shift = lfo_tri_ ? 18 : 19;
    const std::int64_t inc15 = std::clamp<std::int64_t>(
        inc_ + ((static_cast<std::int64_t>((pmd_ * pm) >> shift) * inc_) >> 11), 0, 1 << 22);
    (void)rt;
    const auto inc = static_cast<std::uint64_t>(inc15) << 17;
    const auto inc_int = static_cast<std::int32_t>(inc >> 32);
    const auto inc_frac = static_cast<std::uint32_t>(inc);
    // The walk and the interpolation: the resample kernel (accel/kernels.hpp).
    accel::Walk50 w;
    w.wave = wave_;
    w.wave_size = wave_size_;
    w.loop_start = loop_start_;
    w.pcm8 = pcm8_;
    w.pre = pre_;
    w.loop_len = loop_len_;
    w.loop_frac = loop_frac_;
    w.pos = pos_;
    w.frac = frac_;
    w.ended = ended_;
    accel::resample50(w, inc_int, inc_frac, interp != Interpolation::Linear, out, kBlock);
    pos_ = w.pos;
    frac_ = w.frac;
    ended_ = w.ended;
}

bool ChipVoice::render_begin(const RateTables& rt, Interpolation interp, float* x, SvfStep& svf) {
    if (!active_) return false;
    // LFO: values from the current phase, then advance.
    const std::uint32_t q = lfo_phase_ >> 18;
    const auto am = static_cast<std::int32_t>((lfo_tri_ ? kLfoTriUni : kLfoSawUni)[q] ^ lfo_phase_);

    fetch(rt, interp, x);
    lfo_phase_ = (lfo_phase_ + static_cast<std::uint32_t>(lfo_step_)) & 0xfffff;

    // Cutoff.
    glide(cut_cur_, cut_tgt_, rt.decay[static_cast<std::size_t>(cut_glide_rate_)]);
    f_prev_ = f_new_;
    // The filter LFO uses the unipolar wave (`[0x44]`), so it only lowers the cutoff.
    f_new_ = svf_f(cut_cur_ - ((fmd_ * am) >> (lfo_tri_ ? 4 : 5)));

    aeg_step();

    // Levels.
    const std::int32_t lstep = rt.decay[kLevelGlideRate];
    glide(level_cur_, level_tgt_, lstep);
    glide(pan_l_cur_, pan_l_tgt_, lstep);
    glide(pan_r_cur_, pan_r_tgt_, lstep);
    g_prev_ = g_new_;
    update_gains((amd_ * am) >> (lfo_tri_ ? 5 : 6));

    // The filter's block: a Chamberlin low-pass, f ramping linearly over the block (run by the caller,
    // across a batch of voices: accel::svf50_lanes).
    svf.band = &band_;
    svf.low = &low_;
    svf.f = f_prev_;
    svf.df = (f_new_ - f_prev_) * (1.0f / 128.0f);
    svf.q = q_;
    return true;
}

void ChipVoice::render_end(Buses& buses, float* x) {
    eq.process(x, kBlock);
    const bool faded = steal_fade_.apply(x, kBlock);

    // Mix: gain × envelope ramps linearly from the block start to its end.
    const bool chained = chain_ >= 0 && static_cast<std::size_t>(chain_) < buses.chains;
    const auto c = static_cast<std::size_t>(chained ? chain_ : 0);
    std::array<Buses::Channel*, 7> bus{chained ? &buses.chain_l[c] : &buses.dry_l,
                                       chained ? &buses.chain_r[c] : &buses.dry_r,
                                       &buses.reverb,   &buses.chorus_l, &buses.chorus_r,
                                       &buses.variation_l, &buses.variation_r};
    // The buses that receive anything, mixed in one pass: their gain ramps are independent
    // add chains, which then overlap (the same operations per bus as one loop per bus).
    std::array<float*, 7> dst{};
    std::array<float, 7> g{}, dg{};
    std::size_t k = 0;
    for (std::size_t b = 0; b < bus.size(); ++b) {
        const float g0 = g_prev_[b] * env_prev_;
        const float g1 = g_new_[b] * env_;
        if (g0 == 0.0f && g1 == 0.0f) continue;
        dst[k] = bus[b]->data();
        g[k] = g0;
        dg[k++] = (g1 - g0) * (1.0f / 128.0f);
    }
    accel::mix_ramps(x, dst.data(), g.data(), dg.data(), k, kBlock);
    if (done_ || ended_ || faded) active_ = false;
}

void ChipVoice::render(Buses& buses, const RateTables& rt, Interpolation interp) {
    std::array<float, kBlock> x{};
    SvfStep svf;
    if (!render_begin(rt, interp, x.data(), svf)) return;
    accel::svf50(*svf.band, *svf.low, svf.f, svf.df, svf.q, x.data(), kBlock);
    render_end(buses, x.data());
}

} // namespace hvyxg::syxg50
