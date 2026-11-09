// Copyright (C) 2026 Kevin H. Patterson
// SPDX-License-Identifier: BSD-3-Clause (see LICENSE)
#include "fx/fx_control.hpp"

#include <algorithm>
#include <cmath>

#include "fx/fx_tables.hpp"

namespace hvyxg::fx {

namespace {

constexpr std::uint8_t kParamInit = 0xFF;

// 0.1 ms → 44.1 kHz samples.
std::int16_t samples_100us(std::uint16_t v) {
    return static_cast<std::int16_t>((static_cast<std::uint32_t>(v) * 0x468Fu) >> 12);
}
// 10 µs → 44.1 kHz samples.
std::int16_t samples_10us(std::uint16_t v) {
    return static_cast<std::int16_t>((static_cast<std::uint32_t>(v) * 0x70E5u) >> 16);
}
// Feedback level → s10 word: ±0.97 and ±0.73.
std::uint16_t feedback_word(int p) { return static_cast<std::uint16_t>(((p - 64) * 125 >> 4) & 0x3FF); }
std::uint16_t chorus_feedback_word(int p) {
    return static_cast<std::uint16_t>(((p - 64) * 94 >> 4) & 0x3FF);
}

const std::uint8_t* list(const Tables& tb, std::size_t offset) { return tb.kControlLists.data() + offset; }
const std::uint8_t* chorus_list(const Tables& tb, std::size_t offset) { return tb.kChorusLists.data() + offset; }

std::uint16_t comb_fir(const Tables& tb, std::uint8_t row) { return tb.kCombGain[2u * row]; }
std::uint16_t comb_input(const Tables& tb, std::uint8_t row) { return tb.kCombGain[2u * row + 1]; }
// kReverbTimeBase starts one byte before the DLL table.
std::uint8_t time_base(const Tables& tb, int time) { return tb.kReverbTimeBase[static_cast<std::size_t>(time + 1)]; }

// float `_ftol`-style truncation of v · scale, the quantiser: round half
// away from zero, keep the residual.
struct Quantised {
    float x;  // v·scale ± 0.5
    float r;  // trunc(x) − v·scale
};
Quantised quantise(float v, float scale) {
    const float s = v * scale;
    const float x = v < 0.0f ? s - 0.5f : s + 0.5f;
    return {x, static_cast<float>(static_cast<int>(x)) - s};
}
std::uint16_t word_of(float x) { return static_cast<std::uint16_t>(static_cast<int>(x) & 0x3FF); }

} // namespace

bool is_celeste(std::uint8_t a) { return (a >= 4 && a <= 6) || a == 13; }

std::uint8_t reverb_algorithm(const Tables& tb, std::uint8_t msb, std::uint8_t lsb) {
    // an LSB above 9 reads as 0.
    const std::size_t l = lsb > 9 ? 0 : lsb;
    if (msb < 5) return tb.kReverbTypeMap[msb * 10u + l];
    if (msb >= 0x10 && msb <= 0x13) return tb.kReverbTypeMapRoom[(msb - 0x10u) * 10u + l];
    return 0;
}

std::uint8_t chorus_algorithm(const Tables& tb, std::uint8_t msb, std::uint8_t lsb) {
    const std::size_t l = lsb > 9 ? 0 : lsb;
    if (msb >= 0x41 && msb <= 0x43) return tb.kChorusTypeMap[(msb - 0x41u) * 10u + l];
    return 0;
}

std::array<std::uint16_t, 16> reverb_defaults(const Tables& tb, std::uint8_t algorithm) {
    std::array<std::uint16_t, 16> p{};
    if (algorithm < 19) std::copy_n(tb.kReverbDefaults.begin() + algorithm * 16, 16, p.begin());
    return p;
}

std::array<std::uint16_t, 16> chorus_defaults(const Tables& tb, std::uint8_t algorithm) {
    std::array<std::uint16_t, 16> p{};
    if (algorithm < 21) std::copy_n(tb.kChorusDefaults.begin() + algorithm * 16, 16, p.begin());
    return p;
}

EffectControl::EffectControl(const Tables& tables, float sample_rate) : tb_(tables), rate_(sample_rate) {
    for (std::size_t i = 0; i < tb_.kMixerCoefList.size(); ++i)
        w_[tb_.kMixerCoefList[i]] = static_cast<std::uint16_t>(tb_.kMixerProgram[i]) & 0x3FF;
    // Mixer defaults: reverb pan / return, chorus pan / return / to reverb.
    param_[3].fill(0);
    param_[3][0] = param_[3][1] = param_[3][2] = param_[3][3] = 64;
    load_program(Block::Variation);  // type {0, 0}
}

std::uint16_t EffectControl::time_set() const { return tb_.kReverbTimeSet[rev_algo_]; }

const std::uint16_t* EffectControl::limits(Block block) const {
    switch (block) {
    case Block::Reverb:
        switch (rev_algo_) {
        case 1: case 2: case 3: case 4: case 5: case 6: case 7: case 8:
        case 13: case 14: case 15: case 16: case 17: case 18:
            return tb_.kReverbLimits.data();
        case 9: case 10: case 11: case 12: return tb_.kRoomReverbLimits.data();
        default: return nullptr;
        }
    case Block::Chorus:
        switch (cho_algo_) {
        case 1: case 2: case 3: case 4: case 5: case 6: case 12: case 13:
        case 15: case 16: case 17: case 18: case 19: case 20:
            return tb_.kChorusLimits.data();
        case 7: case 8: case 14: return tb_.kFlangerLimits.data();
        default: return nullptr;
        }
    case Block::Mixer: return tb_.kMixerLimits.data();
    case Block::Variation: {
        // block 2: limit table n (·n) per type.
        int n = -1;
        const std::uint8_t t = var_type_;
        switch (var_cat_) {
        case 0:
            if ((t >= 2 && t <= 7) || t == 0x14 || t == 0x15) n = 0;
            else if (t == 8 || t == 9 || t == 0x16) n = 1;
            else if (t >= 10 && t <= 17) n = t - 8;
            else if (t == 0x13) n = 11;
            break;
        case 1: if (t == 1 || t == 2) n = 13 + t; break;
        case 2:
            if (t >= 1 && t <= 6) n = 16 + (t - 1) / 2;
            else if (t == 7 || t == 8) n = t + 12;
            break;
        case 3:
            if (t >= 1 && t <= 8) n = 21;
            else if (t == 9) n = 22;
            else if (t >= 10 && t <= 12) n = 23;
            break;
        default: break;
        }
        const std::uint16_t* lim = var_limits_ ? var_limits_ : tb_.kVarLimits2006LE.data();
        return n < 0 ? nullptr : lim + 32 * n;
    }
    default: return nullptr;
    }
}

bool EffectControl::parameter_range(Block block, std::uint8_t index, std::uint16_t& lo, std::uint16_t& hi) const {
    if (index >= 16) return false;
    const std::uint16_t* lim = limits(block);
    if (!lim) return false;
    lo = lim[2u * index];
    hi = lim[2u * index + 1];
    return lo != 0 || hi != 0;
}

bool EffectControl::set_parameter(Block block, std::uint8_t index, std::uint16_t value) {
    std::uint16_t lo = 0, hi = 0;
    if (!parameter_range(block, index, lo, hi)) return false;
    value = std::clamp(value, lo, hi);
    param_[static_cast<std::size_t>(block)][index] = value;
    dispatch(block, index);
    return true;
}

void EffectControl::set_reverb_return(std::uint16_t pan, std::uint16_t level) {
    param_[3][0] = pan;
    param_[3][1] = level;
    output_allpass_gains();
    if (rev_algo_ == 0) return;
    // The engine's copy of the time-set table maps
    // algorithm 18 to set 0.
    const auto v = output_allpass_words(rev_algo_ == 18 ? 0 : time_set());
    const std::uint8_t* coef = tb_.kAllpassCoefList.data();
    dsp_w_[coef[8]] = v[0];
    dsp_w_[coef[12]] = v[1];
    dsp_w_[coef[9]] = v[2];
    dsp_w_[coef[13]] = v[3];
}

void EffectControl::set_reverb_algorithm(std::uint8_t algorithm) {
    rev_algo_ = algorithm;
    load_program(Block::Reverb);
    load_defaults(Block::Reverb);
    dispatch(Block::Reverb, kParamInit);
    push_block(Block::Reverb);
}

void EffectControl::set_chorus_algorithm(std::uint8_t algorithm) {
    cho_algo_ = algorithm;
    load_program(Block::Chorus);
    load_defaults(Block::Chorus);
    dispatch(Block::Chorus, kParamInit);
    push_block(Block::Chorus);
}

void EffectControl::load_program(Block block) {
    if (block == Block::Reverb) {
        const std::uint16_t* row = &tb_.kReverbProgram[tb_.kReverbProgramIndex[rev_algo_] * 0x48u];
        for (std::size_t i = 0; i < tb_.kReverbCoefList.size(); ++i) w_[tb_.kReverbCoefList[i]] = *row++;
        for (std::size_t i = 0; i < tb_.kReverbAddrList.size(); ++i)
            a_[tb_.kReverbAddrList[i]] = static_cast<std::int16_t>(*row++);
    } else if (block == Block::Chorus) {
        const std::uint16_t* row = &tb_.kChorusProgram[tb_.kChorusProgramIndex[cho_algo_] * 0x26u];
        for (std::size_t i = 0; i < tb_.kChorusCoefList.size(); ++i) w_[tb_.kChorusCoefList[i]] = *row++;
        for (std::size_t i = 0; i < tb_.kChorusAddrList.size(); ++i)
            a_[tb_.kChorusAddrList[i]] = static_cast<std::int16_t>(*row++);
    } else if (block == Block::Variation) {
        // block 2: 79 words (kVarCoefList order), then addresses 0x24..0x3E.
        const std::uint16_t* row = nullptr;
        const std::uint8_t t = var_type_;
        switch (var_cat_) {
        case 0: row = &tb_.kVarProgram0[tb_.kVarProgramIndex0[std::min<std::size_t>(t, 23)] * 106u]; break;
        case 1: row = &tb_.kVarProgram1[tb_.kVarProgramIndex1[std::min<std::size_t>(t, 3)] * 106u]; break;
        case 2: row = &tb_.kVarProgram2[tb_.kVarProgramIndex2[std::min<std::size_t>(t, 11)] * 106u]; break;
        default: row = &tb_.kVarProgram3[tb_.kVarProgramIndex3[std::min<std::size_t>(t, 15)] * 106u]; break;
        }
        for (std::size_t i = 0; i < tb_.kVarCoefList.size(); ++i) w_[tb_.kVarCoefList[i]] = *row++;
        for (std::size_t i = 0x24; i <= 0x3E; ++i) a_[i] = static_cast<std::int16_t>(*row++);
    }
}

void EffectControl::load_defaults(Block block) {
    if (block == Block::Reverb)
        param_[0] = reverb_defaults(tb_, rev_algo_);
    else if (block == Block::Chorus)
        param_[1] = chorus_defaults(tb_, cho_algo_);
    else if (block == Block::Variation)
        param_[2] = variation_defaults(tb_, {var_cat_, var_type_});
}

void EffectControl::dispatch(Block block, std::uint8_t index) {
    if (block == Block::Reverb) {
        switch (rev_algo_) {
        case 1: case 2: case 3: case 4: case 5: case 6: case 7: case 8:
        case 13: case 14: case 15: case 16: case 17: case 18:
            reverb_param(index);
            break;
        case 9: case 10: case 12: room_param(index); break;
        default: break;
        }
    } else if (block == Block::Chorus) {
        switch (cho_algo_) {
        case 1: case 2: case 3: case 4: case 5: case 6: case 12: case 13:
        case 15: case 16: case 17: case 18: case 19:
            chorus_param(index);
            break;
        case 7: case 8: case 14: case 20: flanger_param(index); break;
        default: break;
        }
    } else if (block == Block::Variation) {
        var_param(index);
    } else if (block == Block::Mixer && index <= 1) {
        output_allpass_gains();
    } else if (block == Block::Mixer && index == 9) {
        connection_changed();
    }
}

// --- reverb --------------------------------------------------------------------------------

void EffectControl::comb_gains(std::uint16_t time, std::uint16_t set, std::size_t lst) {
    const std::uint8_t base = time_base(tb_, time);
    const std::uint8_t* l = list(tb_, lst);
    for (std::size_t k = 0; k < 4; ++k) {
        const auto row = static_cast<std::uint8_t>(tb_.kReverbTimeOffset[set * 4u + k] + base);
        w_[l[3 * k]] = comb_input(tb_, row);
        w_[l[3 * k + 1]] = comb_fir(tb_, row);
        w_[l[3 * k + 2]] = comb_fir(tb_, row);
    }
}

void EffectControl::room_comb_gain(std::uint16_t time, std::int16_t skew, std::uint16_t dim,
                              std::size_t lst) {
    const std::uint8_t d = tb_.kRoomTimeOffset[dim];
    const auto off = static_cast<std::uint8_t>(d < 0x38 ? 0 : d - 0x38);
    std::uint8_t c = 0;
    if (skew >= 0 || time != 0) {
        const std::uint8_t base = time_base(tb_, time);
        const std::uint8_t lo = skew < 0 ? time_base(tb_, time - 1) : base;
        const std::uint8_t hi = skew < 0 ? base : time_base(tb_, time + 1);
        c = static_cast<std::uint8_t>((static_cast<int>(hi - lo) * skew) / 10 + base);
    }
    const auto row = static_cast<std::uint8_t>(off + c);
    const std::uint8_t* l = list(tb_, lst);
    // Long reverb times in the smallest rooms (time ≥ 53 with dimension 0 …) give rows past the
    // table's 208, up to 235 within the XG parameter ranges: the DLL then reads whatever
    // follows its table (not measured). hvyxg holds the last row there.
    w_[l[0]] = comb_input(tb_, std::min<std::uint8_t>(row, tb_.kCombGain.size() / 2 - 1));
    w_[l[1]] = w_[l[2]] = row < 0xCB ? comb_fir(tb_, row) : 0xFF;
}

void EffectControl::diffusion(std::uint16_t diffusion, std::uint16_t set) {
    const std::uint16_t depth = tb_.kDiffusionDepth[diffusion];
    const std::uint8_t* l = list(tb_, lists::kDiffusion);
    for (std::size_t k = 0; k < 4; ++k, l += 4) {
        const std::int16_t tap = samples_10us(tb_.kDiffusionGeometry[set * 8u + 2 * k]);
        const std::int16_t len = samples_10us(tb_.kDiffusionGeometry[set * 8u + 2 * k + 1]);
        const auto spread = static_cast<std::int16_t>((tb_.kDiffusionSpread[k] * depth) >> 7);
        std::int16_t base = a_[l[0]];
        a_[l[1]] = static_cast<std::int16_t>(base + len);
        if ((k & 1) == 0) {
            a_[l[2]] = static_cast<std::int16_t>(base + tap);
            base = static_cast<std::int16_t>(base + spread);
        } else {
            a_[l[2]] = static_cast<std::int16_t>(base + spread + tap);
        }
        a_[l[3]] = static_cast<std::int16_t>(base + tap);
    }
}

void EffectControl::room_comb(std::uint16_t diffusion, std::uint8_t spread_index, std::uint16_t dim,
                         std::size_t lst) {
    const std::int16_t len = samples_10us(tb_.kRoomSize[dim]);
    const std::uint16_t depth = tb_.kDiffusionDepth[diffusion];
    const std::uint16_t spread_unit = tb_.kDiffusionSpread[spread_index];
    const std::uint8_t* l = list(tb_, lst);
    const std::int16_t base = a_[l[0]];
    const auto half = static_cast<std::int16_t>(static_cast<std::uint16_t>(len) >> 1);
    a_[l[1]] = static_cast<std::int16_t>(len + base);
    const auto spread = static_cast<std::int16_t>((spread_unit * depth) >> 7);
    if ((spread_index & 1) == 0) {
        a_[l[2]] = static_cast<std::int16_t>(half + base);
        a_[l[3]] = static_cast<std::int16_t>(base + spread + half);
    } else {
        a_[l[2]] = static_cast<std::int16_t>(spread + base + half);
        a_[l[3]] = static_cast<std::int16_t>(base + half);
    }
}

void EffectControl::allpasses(std::uint16_t density, std::int16_t diffusion, std::uint16_t set) {
    unsigned mask = tb_.kDensityMask[std::min<std::uint16_t>(density, 3)];
    unsigned doubled = 0xC;  // the two output all-passes carry ×2 coefficients
    const std::uint16_t* row = &tb_.kAllpassGeometry[set * 8u];
    const std::uint8_t* coef = tb_.kAllpassCoefList.data();
    const std::uint8_t* addr = list(tb_, lists::kAllpassAddr);
    for (std::size_t st = 0; st < 4; ++st, coef += 4, addr += 2) {
        std::int16_t len = st == 3 && diffusion == 0 ? samples_10us(row[2 * st - 2])
                                                     : samples_10us(row[2 * st]);
        if (len == 0) len = 1;
        a_[addr[1]] = static_cast<std::int16_t>(len + a_[addr[0]]);
        const auto g = static_cast<std::int16_t>(row[2 * st + 1]);
        int out_x, out_d, in_x, in_d;
        if ((mask & 1) == 0) {
            out_x = 0x80;
            out_d = 0;
            in_x = 0;
            in_d = 0;
        } else {
            out_x = static_cast<std::int16_t>(-static_cast<int>(g) >> 2);
            out_d = 0x80 - out_x;
            in_x = 0x200 - g;
            in_d = g;
        }
        if (doubled & 1) {
            out_x <<= 1;
            out_d <<= 1;
        }
        mask >>= 1;
        doubled >>= 1;
        w_[coef[0]] = static_cast<std::uint16_t>(out_x & 0x3FF);
        w_[coef[1]] = static_cast<std::uint16_t>(out_d & 0x3FF);
        w_[coef[2]] = static_cast<std::uint16_t>(in_x & 0x3FF);
        w_[coef[3]] = static_cast<std::uint16_t>(in_d & 0x3FF);
    }
}

std::array<std::uint16_t, 4> EffectControl::output_allpass_words(std::uint16_t set) const {
    // reverb return and pan folded into the output all-pass words 72/71, 84/83.
    float x1 = 0.5f, x2 = 0.5f, y1 = 0.0f, y2 = 0.0f;
    if ((param_[0][11] & 0x7FFF) != 0) {
        const std::uint16_t* row = &tb_.kAllpassGeometry[set * 8u];
        x1 = -(static_cast<float>(row[5]) / 1024.0f);
        x2 = -(static_cast<float>(row[7]) / 1024.0f);
        y1 = 0.5f - x1;
        y2 = 0.5f - x2;
    }
    const float level = static_cast<float>(param_[3][1] & 0x7FFF) / 127.0f;
    const std::uint16_t pan = param_[3][0] & 0x7FFF;
    const auto pan_l = static_cast<double>(tb_.kPanCurve[128 - pan]);
    const auto pan_r = static_cast<double>(tb_.kPanCurve[pan]);
    const auto q = [&](double p, float x) {
        return static_cast<std::uint16_t>(
            static_cast<int>(static_cast<float>(p * level * x)) & 0x3FF);
    };
    return {q(pan_l, x1), q(pan_r, x2), q(pan_l, y1), q(pan_r, y2)};
}

void EffectControl::output_allpass_gains() {
    if (rev_algo_ == 0) return;
    const auto v = output_allpass_words(time_set());
    const std::uint8_t* coef = tb_.kAllpassCoefList.data();
    w_[coef[8]] = v[0];
    w_[coef[12]] = v[1];
    w_[coef[9]] = v[2];
    w_[coef[13]] = v[3];
}

void EffectControl::push_words(const std::uint8_t* lst, std::size_t n) {
    for (std::size_t i = 0; i < n; ++i) dsp_w_[lst[i]] = w_[lst[i]];
}

void EffectControl::push_addrs(const std::uint8_t* lst, std::size_t n) {
    for (std::size_t i = 0; i < n; ++i) dsp_a_[lst[i]] = a_[lst[i]];
}

void EffectControl::push_block(Block block) {
    // every register of the block.
    if (block == Block::Reverb) {
        push_addrs(tb_.kReverbAddrList.data(), tb_.kReverbAddrList.size());
        push_words(tb_.kReverbCoefList.data(), tb_.kReverbCoefList.size());
    } else if (block == Block::Variation) {
        for (std::size_t i = 0x24; i <= 0x3E; ++i) dsp_a_[i] = a_[i];
        push_words(tb_.kVarCoefList.data(), tb_.kVarCoefList.size());
    } else {
        push_addrs(tb_.kChorusAddrList.data(), tb_.kChorusAddrList.size());
        push_words(tb_.kChorusCoefList.data(), tb_.kChorusCoefList.size());
    }
}

void EffectControl::delays() {
    const auto& p = param_[0];
    a_[63] = static_cast<std::int16_t>(samples_100us(tb_.kInitialDelay[p[2]]) + a_[17]);
    a_[8] = static_cast<std::int16_t>(samples_100us(tb_.kInitialDelay[p[10]]) + a_[63]);
}

void EffectControl::er_balance() {
    const std::uint16_t v = param_[0][12];
    const auto er = static_cast<std::uint16_t>(
        static_cast<std::int16_t>((tb_.kLevelCurve[128 - v] * tb_.kErScale[rev_algo_]) >> 8));
    const auto rev = static_cast<std::uint16_t>(tb_.kLevelCurve[v]);
    const std::uint8_t* l = list(tb_, lists::kErRev);
    // {27, 32} early tap, {28..31, 33..36} comb taps.
    w_[l[0]] = w_[l[1]] = er;
    for (std::size_t i = 2; i < 10; ++i) w_[l[i]] = rev;
    push_words(l, 10);
}

void EffectControl::density() {
    const auto& p = param_[0];
    allpasses(p[11], static_cast<std::int16_t>(p[1]), time_set());
    output_allpass_gains();
    push_words(tb_.kAllpassCoefList.data(), tb_.kAllpassCoefList.size());
}

void EffectControl::feedback() {
    w_[tb_.kControlLists[lists::kFeedback]] = feedback_word(param_[0][14]);
    push_words(list(tb_, lists::kFeedback), 1);
}

void EffectControl::reverb_param(std::uint8_t index) {
    const auto& p = param_[0];
    if (index == kParamInit) {
        for (std::uint8_t i : {0, 1, 2, 3, 4, 11, 12, 14}) reverb_param(i);
        return;
    }
    switch (index) {
    case 0:
        comb_gains(p[0], time_set(), lists::kCombCoef);
        push_words(list(tb_, lists::kCombCoef), 12);
        break;
    case 1: {
        diffusion(p[1], time_set());
        static constexpr std::array<std::uint8_t, 8> kTaps = {0, 4, 1, 5, 2, 6, 3, 7};
        push_addrs(kTaps.data(), kTaps.size());
        allpasses(p[11], static_cast<std::int16_t>(p[1]), time_set());
        output_allpass_gains();
        push_addrs(list(tb_, lists::kOutAllpassRead), 1);
        break;
    }
    case 2: case 10:
        delays();
        push_addrs(list(tb_, lists::kDelayAddr), 2);
        break;
    case 3:
        design_first_order(1, static_cast<std::uint8_t>(p[3]),
                           static_cast<std::uint8_t>(
                               static_cast<std::int8_t>(tb_.kControlLists[lists::kHpfTrim + rev_algo_]) + 0x40),
                           list(tb_, lists::kHpf));
        break;
    case 4: design_first_order(0, static_cast<std::uint8_t>(p[4]), 0x40, list(tb_, lists::kLpf)); break;
    case 11: density(); break;
    case 12: er_balance(); break;
    case 14: feedback(); break;
    default: break;
    }
}

void EffectControl::room_param(std::uint8_t index) {
    const auto& p = param_[0];
    const auto skew = [&](std::size_t k) {
        const std::uint8_t* b = list(tb_, lists::kRoomRow + 12u * room_row_ + 2 * k);
        return static_cast<std::int16_t>(b[0] | (b[1] << 8));
    };
    const auto order = [&](std::size_t k) { return tb_.kControlLists[lists::kRoomOrder + room_order_ + k]; };
    const std::uint16_t width = p[5], height = p[6], depth = p[7], wall = p[8];
    // The fourth comb pairs the wall variation with one dimension.
    std::size_t dim4 = 0;
    std::uint16_t size4 = 0;
    bool has4 = true;
    switch (rev_algo_) {
    case 9: case 12: dim4 = 1; size4 = static_cast<std::uint16_t>(wall + height); break;
    case 10: dim4 = 2; size4 = static_cast<std::uint16_t>(wall + depth); break;
    case 11: dim4 = 0; size4 = static_cast<std::uint16_t>(wall + width); break;
    default: has4 = false; break;
    }
    const std::size_t comb = lists::kCombCoef;
    const std::size_t diff = lists::kDiffusion;
    // Recompute one comb (gain + length + taps) and send its registers.
    const auto comb_k = [&](std::size_t k, std::int16_t sk, std::uint8_t spread, std::uint16_t size) {
        room_comb_gain(p[0], sk, size, comb + 3 * k);
        room_comb(p[1], spread, size, diff + 4 * k);
        push_addrs(list(tb_, diff + 4 * k + 1), 3);
        push_words(list(tb_, comb + 3 * k), 3);
    };
    const auto comb4 = [&] { comb_k(3, skew(dim4), order(2), size4); };
    if (index == kParamInit) {
        room_row_ = tb_.kControlLists[lists::kRoomRowSet + (rev_algo_ & 7)];
        room_order_ = tb_.kControlLists[lists::kRoomOrderSet + (rev_algo_ & 7)];
        for (std::uint8_t i : {0, 1, 2, 3, 4, 11, 12, 14}) room_param(i);
        return;
    }
    switch (index) {
    case 0:
        room_comb_gain(p[0], skew(0), width, comb);
        room_comb_gain(p[0], skew(1), height, comb + 3);
        room_comb_gain(p[0], skew(2), depth, comb + 6);
        if (has4) room_comb_gain(p[0], skew(dim4), size4, comb + 9);
        push_words(list(tb_, comb), 12);
        break;
    case 1:
        room_comb(p[1], 0, width, diff);
        room_comb(p[1], order(0), height, diff + 4);
        room_comb(p[1], order(1), depth, diff + 8);
        if (has4) room_comb(p[1], order(2), size4, diff + 12);
        for (std::size_t k = 0; k < 4; ++k) push_addrs(list(tb_, diff + 4 * k + 2), 2);
        // Unlike the other reverbs, the output all-pass words are recomputed here but
        // neither restored nor sent.
        allpasses(p[11], static_cast<std::int16_t>(p[1]), time_set());
        push_addrs(list(tb_, lists::kOutAllpassRead), 1);
        break;
    case 2: case 10: reverb_param(index); break;
    case 3: case 4: reverb_param(index); break;
    case 5:
        comb_k(0, skew(0), 0, width);
        if (rev_algo_ == 11) comb4();
        break;
    case 6:
        comb_k(1, skew(1), order(0), height);
        if (rev_algo_ == 9 || rev_algo_ == 12) comb4();
        break;
    case 7:
        comb_k(2, skew(2), order(1), depth);
        if (rev_algo_ == 10) comb4();
        break;
    case 8:
        if (has4) comb4();
        break;
    case 11: density(); break;
    case 12: er_balance(); break;
    case 14: feedback(); break;
    default: break;
    }
}

// --- chorus --------------------------------------------------------------------------------

void EffectControl::chorus_delay(bool celeste) {
    const auto& p = param_[1];
    const std::int16_t d = samples_100us(tb_.kChorusDelay[p[3]]);
    std::int16_t base = a_[27];
    const auto at = [&](std::size_t i, int v) { a_[i] = static_cast<std::int16_t>(v); };
    at(9, base + 1 + d);
    at(10, base + 2 + d);
    if (celeste) {
        at(19, base + 0x41 + d);
        at(20, base + 0x42 + d);
    }
    if (p[14] == 1) {
        w_[80] = 0;
        base = static_cast<std::int16_t>((0x8000 - 0x6000 - 1) / 2 + 1);  // stereo R line
        a_[22] = base;
    } else {
        w_[80] = 0x200;
        a_[22] = 0;
    }
    at(14, base + 1 + d);
    at(15, base + 2 + d);
    if (celeste) {
        at(25, base + 0x41 + d);
        at(26, base + 0x42 + d);
    }
    push_words(chorus_list(tb_, chorus_lists::kInputMode), 1);
    push_addrs(chorus_list(tb_, chorus_lists::kTapAddr), celeste ? 4 : 2);
    push_addrs(chorus_list(tb_, chorus_lists::kLineAddr), celeste ? 5 : 3);
    chorus_eq_low();
    chorus_eq_high();
}

void EffectControl::chorus_eq_low() {
    const auto& p = param_[1];
    for (const std::size_t l : {chorus_lists::kEqLowL, chorus_lists::kEqLowR})
        design_first_order(0x12, static_cast<std::uint8_t>(p[5]), static_cast<std::uint8_t>(p[6]),
                           chorus_list(tb_, l));
}

void EffectControl::chorus_eq_high() {
    const auto& p = param_[1];
    for (const std::size_t l : {chorus_lists::kEqHighL, chorus_lists::kEqHighR})
        design_first_order(3, static_cast<std::uint8_t>(p[7]), static_cast<std::uint8_t>(p[8]),
                           chorus_list(tb_, l));
}

void EffectControl::chorus_param(std::uint8_t index) {
    const auto& p = param_[1];
    if (index == kParamInit) {
        const auto g = static_cast<std::uint16_t>((tb_.kChorusOutGain[cho_algo_] * 0x200) >> 6);
        w_[105] = w_[96] = g;
        if (cho_algo_ > 3 && cho_algo_ < 15) w_[106] = w_[97] = g;
        for (std::uint8_t i : {0, 1, 2, 3}) chorus_param(i);
        return;
    }
    switch (index) {
    case 0:
        w_[9] = tb_.kLfoWord[p[0]];
        push_words(chorus_list(tb_, chorus_lists::kLfo), 1);
        break;
    case 1: {
        const auto v = static_cast<std::uint8_t>(p[1] << 1);
        w_[26] = w_[40] = w_[55] = static_cast<std::uint16_t>((v * 128u) / 127u);
        push_words(chorus_list(tb_, chorus_lists::kDepth), 3);
        break;
    }
    case 2:
        w_[66] = w_[79] = chorus_feedback_word(p[2]);
        push_words(chorus_list(tb_, chorus_lists::kFeedback), 2);
        break;
    case 3: case 14: chorus_delay(true); break;
    case 5: case 6: chorus_eq_low(); break;
    case 7: case 8: chorus_eq_high(); break;
    default: break;
    }
}

void EffectControl::flanger_param(std::uint8_t index) {
    const auto& p = param_[1];
    if (index == kParamInit) {
        w_[105] = w_[96] = static_cast<std::uint16_t>((tb_.kChorusOutGain[cho_algo_] * 0x200) >> 6);
        for (std::uint8_t i : {0, 1, 2, 3, 13}) flanger_param(i);
        return;
    }
    switch (index) {
    case 0: chorus_param(0); break;
    case 1:
        w_[26] = w_[40] = static_cast<std::uint16_t>((p[1] * 128u) / 127u);
        push_words(chorus_list(tb_, chorus_lists::kDepth), 2);
        break;
    case 2:
        w_[66] = w_[79] = feedback_word(p[2]);
        push_words(chorus_list(tb_, chorus_lists::kFeedback), 2);
        break;
    case 3: case 14: chorus_delay(false); break;
    case 5: case 6: case 7: case 8: chorus_param(index); break;
    case 13:
        w_[37] = static_cast<std::uint16_t>((((64 - static_cast<int>(p[13])) * 0x200) / 60) & 0x3FF);
        push_words(chorus_list(tb_, chorus_lists::kPhase), 1);
        break;
    default: break;
    }
}

// --- filter designer -----------------------------------------------------------------------

void EffectControl::design_first_order(std::uint8_t kind, std::uint8_t freq, std::uint8_t gain,
                                  const std::uint8_t* lst) {
    const std::size_t fi = std::min<std::size_t>(freq, tb_.kTan44100.size() - 1);
    const float k = rate_ == 44100.0f
                        ? tb_.kTan44100[fi]
                        : static_cast<float>(std::tan(3.14159265358979 *
                                                      std::min(double(kEqFrequency[fi]), 0.45 * rate_) / rate_));
    const auto g_of = [this](unsigned g) { return tb_.kShelfGain[std::clamp(g, 52u, 76u) - 52u]; };
    float b0 = 0, b1 = 0, a = 0;
    switch (kind) {
    case 0: {  // LPF
        const float d = k + 1.0f;
        b0 = b1 = (g_of(gain) * k) / d;
        a = (1.0f - k) / d;
        break;
    }
    case 1: {  // HPF
        const float d = k + 1.0f;
        b0 = g_of(gain) / d;
        b1 = -b0;
        a = (1.0f - k) / d;
        break;
    }
    case 2: case 0x12: case 0x13: {  // low shelf
        float n, dd;
        if (gain < 0x40) {
            n = k;
            dd = k * g_of(128u - gain);
        } else {
            dd = k;
            n = k * g_of(gain);
        }
        const float d = dd + 1.0f;
        b0 = (n + 1.0f) / d;
        b1 = (n - 1.0f) / d;
        a = (1.0f - dd) / d;
        if (kind != 2) {
            b0 *= 1.414199948f;
            b1 *= 1.414199948f;
        }
        break;
    }
    default: {  // 3: high shelf
        float n, dd;
        if (gain < 0x40) {
            n = 1.0f;
            dd = g_of(128u - gain);
        } else {
            dd = 1.0f;
            n = g_of(gain);
        }
        const float d = dd + k;
        b0 = (n + k) / d;
        b1 = (k - n) / d;
        a = (dd - k) / d;
        break;
    }
    }
    // b0, b1 → /128 words with their summed rounding error kept within ½ LSB; a → /512.
    Quantised q0 = quantise(b0, 128.0f);
    Quantised q1 = quantise(b1, 128.0f);
    const Quantised qa = quantise(a, 512.0f);
    const float sum = q0.r + q1.r;
    if (sum < -0.5f) {
        if (q0.r <= q1.r) q0.x += 1.0f; else q1.x += 1.0f;
    } else if (sum > 0.5f) {
        if (q0.r > q1.r) q0.x -= 1.0f; else q1.x -= 1.0f;
    }
    w_[lst[0]] = word_of(q0.x);
    w_[lst[1]] = word_of(q1.x);
    w_[lst[2]] = word_of(qa.x);
    push_words(lst, 3);
}

} // namespace hvyxg::fx
