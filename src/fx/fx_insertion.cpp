// Copyright (C) 2026 Kevin H. Patterson
// SPDX-License-Identifier: BSD-3-Clause (see LICENSE)
#include "fx/fx_insertion.hpp"

#include <algorithm>
#include <cmath>

#include "engine_tables.hpp"
#include "fx/fx_dsp.hpp"
#include "fx/fx_system.hpp"
#include "fx/fx_tables.hpp"
#include "midi/part.hpp"
#include "midi/xg_ram.hpp"

namespace hvyxg::fx {

namespace {

constexpr double kPi = 3.14159265358979323846;

// XG table #8 (compressor attack, ms) and #9 (release, ms), table #10 (ratio).
constexpr std::array<float, 20> kAttackMs = {1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 12, 14, 16, 18, 20, 23, 26, 30, 35, 40};
constexpr std::array<float, 16> kReleaseMs = {10, 15, 25, 35, 45, 55, 65, 75, 85, 100, 115, 140, 170, 230, 340, 680};
constexpr std::array<float, 8> kRatio = {1.0f, 1.5f, 2.0f, 3.0f, 5.0f, 7.0f, 10.0f, 20.0f};

// XG table #18: level v → (v / 127)² (40·log10(v / 127) dB).
float level18(std::uint16_t v) {
    const float x = static_cast<float>(std::min<std::uint16_t>(v, 127)) / 127.0f;
    return x * x;
}
// One-pole coefficient for a time constant in ms.
float pole(float ms, float fs) { return 1.0f - std::exp(-1.0f / (std::max(ms, 0.01f) * 0.001f * fs)); }
// Constant-power dry/wet (table #15 shape, as the DLLs' W2 curve).
float wet_of(const Tables& tb, std::uint16_t v) { return static_cast<float>(tb.kPanCurve[std::min<std::uint16_t>(v, 127)]) / 512.0f; }
float dry_of(const Tables& tb, std::uint16_t v) {
    return static_cast<float>(tb.kPanCurve[128u - std::min<std::uint16_t>(v, 127)]) / 512.0f;
}
// XG table #22 (ring modulator carrier, Hz): linear ≈ 0.67·(v + 1) at the bottom, 2^(1/12.4)
// per step above (within 3 % of the table).
double carrier_hz(int v) {
    return std::max(0.67 * (v + 1), 1000.7 * std::pow(1.0573, v - 98));
}

using Params = std::array<std::uint16_t, 16>;

} // namespace

// --- XG2.0-only types ----------------------------------------------------------------------

std::optional<std::array<std::uint16_t, 16>> xg20_insertion_defaults(std::uint8_t msb, std::uint8_t lsb) {
    switch (Xg20Effect::kind_of(msb, lsb)) {
    case Xg20Effect::Kind::Compressor: return Params{6, 2, 100, 4, 96, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0};
    case Xg20Effect::Kind::NoiseGate: return Params{0, 11, 82, 50, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0};
    case Xg20Effect::Kind::RingMod: return Params{98, 0, 0, 0, 64, 0, 60, 0, 0, 127, 0, 0, 0, 0, 0, 0};
    case Xg20Effect::Kind::LoFi: return Params{2, 60, 6, 54, 5, 10, 1, 1, 0, 127, 0, 0, 0, 0, 1, 0};
    default: return std::nullopt;
    }
}

Xg20Effect::Kind Xg20Effect::kind_of(std::uint8_t msb, std::uint8_t lsb) {
    if (lsb != 0) return Kind::None;
    switch (msb) {
    case 0x53: return Kind::Compressor;
    case 0x54: return Kind::NoiseGate;
    case 0x71: return Kind::RingMod;
    case 0x5E: return Kind::LoFi;
    default: return Kind::None;
    }
}

Xg20Effect::Xg20Effect(const Tables& tables, float sample_rate) : tb_(tables), rate_(sample_rate) {}

Xg20Effect::Biquad Xg20Effect::lowpass(float fs, float f, float q) {
    // RBJ cookbook low-pass.
    const double w = 2 * kPi * std::min(static_cast<double>(f), 0.45 * fs) / fs;
    const double a = std::sin(w) / (2 * q), c = std::cos(w), n = 1 + a;
    Biquad b;
    b.b0 = static_cast<float>((1 - c) / 2 / n);
    b.b1 = static_cast<float>((1 - c) / n);
    b.b2 = b.b0;
    b.a1 = static_cast<float>(-2 * c / n);
    b.a2 = static_cast<float>((1 - a) / n);
    return b;
}

Xg20Effect::Biquad Xg20Effect::highpass(float fs, float f, float q) {
    const double w = 2 * kPi * std::min(static_cast<double>(f), 0.45 * fs) / fs;
    const double a = std::sin(w) / (2 * q), c = std::cos(w), n = 1 + a;
    Biquad b;
    b.b0 = static_cast<float>((1 + c) / 2 / n);
    b.b1 = static_cast<float>(-(1 + c) / n);
    b.b2 = b.b0;
    b.a1 = static_cast<float>(-2 * c / n);
    b.a2 = static_cast<float>((1 - a) / n);
    return b;
}

void Xg20Effect::set(Kind kind, const Params& p) {
    const bool changed = kind != kind_;
    kind_ = kind;
    p_ = p;
    const auto at = [&](std::size_t i, std::size_t n) { return std::min<std::size_t>(p[i], n - 1); };
    switch (kind) {
    case Kind::Compressor:
    case Kind::NoiseGate: {
        att_ = pole(kAttackMs[at(0, kAttackMs.size())], rate_);
        rel_ = pole(kReleaseMs[at(1, kReleaseMs.size())], rate_);
        thr_db_ = static_cast<float>(p[2]) - 127.0f;
        if (kind == Kind::Compressor) {
            ratio_ = kRatio[at(3, kRatio.size())];
            out_ = level18(p[4]);
        } else {
            out_ = level18(p[3]);
        }
        break;
    }
    case Kind::RingMod: {
        const int v = std::min<int>(p[0], 127);
        const double next = carrier_hz(std::min(v + 1, 127));
        carrier_hz_ = carrier_hz(v) + (next - carrier_hz(v)) * std::min<int>(p[1], 127) / 128.0;
        lfo_sine_ = p[2] != 0;
        lfo_depth_ = static_cast<float>(std::min<int>(p[3], 127)) / 127.0f;
        lfo_hz_ = static_cast<double>(decode_word(tb_.kLfoWord[std::min<std::uint16_t>(p[4], 127)], 2)) * 22050.0;
        hpf_on_ = p[5] > 0;
        lpf_on_ = p[6] < 60;
        const auto f = [](std::uint16_t i) { return kEqFrequency[std::min<std::size_t>(i, kEqFrequency.size() - 1)]; };
        for (std::size_t c = 0; c < 2; ++c) {
            const Biquad h = highpass(rate_, f(p[5]), 0.7071f), lp = lowpass(rate_, f(p[6]), 0.7071f);
            hpf_[c] = changed ? h : Biquad{h.b0, h.b1, h.b2, h.a1, h.a2, hpf_[c].x1, hpf_[c].x2, hpf_[c].y1, hpf_[c].y2};
            lpf_[c] = changed ? lp : Biquad{lp.b0, lp.b1, lp.b2, lp.a1, lp.a2, lpf_[c].x1, lpf_[c].x2, lpf_[c].y1, lpf_[c].y2};
        }
        dry_ = dry_of(tb_, p[9]);
        wet_ = wet_of(tb_, p[9]);
        break;
    }
    case Kind::LoFi: {
        hold_step_ = 44100.0 / (static_cast<double>(std::min<int>(p[0], 127)) + 1.0) / rate_;
        quant_ = std::pow(2.0f, -(static_cast<float>(std::clamp<int>(p[1], 1, 127)) * 15.0f / 127.0f + 1.0f));
        gain_out_ = std::pow(10.0f, (static_cast<float>(std::min<int>(p[2], 42)) - 6.0f) / 20.0f);
        lpf_on_ = p[4] != 0 && p[3] < 60;
        const float q = static_cast<float>(std::clamp<int>(p[5], 10, 120)) / 10.0f;
        const float fc = kEqFrequency[std::min<std::size_t>(p[3], kEqFrequency.size() - 1)];
        for (std::size_t c = 0; c < 2; ++c) {
            const Biquad lp = lowpass(rate_, fc, q * 0.7071f);
            lpf_[c] = changed ? lp : Biquad{lp.b0, lp.b1, lp.b2, lp.a1, lp.a2, lpf_[c].x1, lpf_[c].x2, lpf_[c].y1, lpf_[c].y2};
        }
        stereo_ = p[14] != 0;
        dry_ = dry_of(tb_, p[9]);
        wet_ = wet_of(tb_, p[9]);
        break;
    }
    default: break;
    }
    if (changed) clear();
}

void Xg20Effect::clear() {
    env_ = 0;
    gain_ = kind_ == Kind::NoiseGate ? 0.0f : 1.0f;
    carrier_phase_ = lfo_phase_ = 0;
    for (auto* b : {&hpf_[0], &hpf_[1], &lpf_[0], &lpf_[1]}) b->x1 = b->x2 = b->y1 = b->y2 = 0;
    hold_pos_ = 0;
    hold_l_ = hold_r_ = 0;
}

void Xg20Effect::process(float* l, float* r, std::size_t n) {
    switch (kind_) {
    case Kind::Compressor: dynamics(l, r, n, false); break;
    case Kind::NoiseGate: dynamics(l, r, n, true); break;
    case Kind::RingMod: ring(l, r, n); break;
    case Kind::LoFi: lofi(l, r, n); break;
    default: break;
    }
}

void Xg20Effect::dynamics(float* l, float* r, std::size_t n, bool gate) {
    const float thr = std::pow(10.0f, thr_db_ / 20.0f);
    const float slope = 1.0f - 1.0f / ratio_;
    for (std::size_t i = 0; i < n; ++i) {
        // Stereo-linked peak detector.
        const float x = std::max(std::fabs(l[i]), std::fabs(r[i]));
        env_ += (x - env_) * (x > env_ ? att_ : rel_);
        float g;
        if (gate) {
            const float target = env_ > thr ? 1.0f : 0.0f;
            gain_ += (target - gain_) * (target > gain_ ? att_ : rel_);
            g = gain_;
        } else {
            g = 1.0f;
            if (env_ > thr) g = std::pow(thr / env_, slope);  // gr = (1 − 1/ratio)·(level − thr) dB
        }
        g *= out_;
        l[i] *= g;
        r[i] *= g;
    }
}

void Xg20Effect::ring(float* l, float* r, std::size_t n) {
    const double dlfo = lfo_hz_ / rate_;
    for (std::size_t i = 0; i < n; ++i) {
        double m;
        if (lfo_sine_) {
            m = std::sin(2 * kPi * lfo_phase_);
        } else {
            m = lfo_phase_ < 0.5 ? 4 * lfo_phase_ - 1 : 3 - 4 * lfo_phase_;
        }
        lfo_phase_ += dlfo;
        lfo_phase_ -= std::floor(lfo_phase_);
        const double f = carrier_hz_ * (1.0 + lfo_depth_ * m);
        const auto c = static_cast<float>(std::sin(2 * kPi * carrier_phase_));
        carrier_phase_ += f / rate_;
        carrier_phase_ -= std::floor(carrier_phase_);
        float wl = l[i] * c, wr = r[i] * c;
        if (hpf_on_) {
            wl = hpf_[0].run(wl);
            wr = hpf_[1].run(wr);
        }
        if (lpf_on_) {
            wl = lpf_[0].run(wl);
            wr = lpf_[1].run(wr);
        }
        l[i] = dry_ * l[i] + wet_ * wl;
        r[i] = dry_ * r[i] + wet_ * wr;
    }
}

void Xg20Effect::lofi(float* l, float* r, std::size_t n) {
    for (std::size_t i = 0; i < n; ++i) {
        float xl = l[i], xr = r[i];
        if (!stereo_) xl = xr = 0.5f * (xl + xr);
        // Sample-and-hold at the reduced rate, then word-length quantisation.
        hold_pos_ += hold_step_;
        if (hold_pos_ >= 1.0) {
            hold_pos_ -= std::floor(hold_pos_);
            hold_l_ = std::round(xl / quant_) * quant_;
            hold_r_ = std::round(xr / quant_) * quant_;
        }
        float wl = hold_l_, wr = hold_r_;
        if (lpf_on_) {
            wl = lpf_[0].run(wl);
            wr = lpf_[1].run(wr);
        }
        l[i] = dry_ * l[i] + wet_ * gain_out_ * wl;
        r[i] = dry_ * r[i] + wet_ * gain_out_ * wr;
    }
}

// --- insertion stage -----------------------------------------------------------------------

struct InsertionFx::Unit {
    Unit(const Tables& tb, float rate, const EngineModel& m)
        : ctrl(tb, rate), var(rate, m.distortion_clip_unit()), own(tb, rate) {
        ctrl.set_variation_limits(m.variation_limits);
    }
    EffectControl ctrl;
    VariationUnit var;
    Xg20Effect own;
    Type type = Type::Thru;
    bool fresh = true;
    std::uint8_t msb = 0, lsb = 0;
    std::uint32_t writes = 0;
    Params params{};
    bool running = false;
    std::size_t quiet = 0;
};

InsertionFx::InsertionFx(const ModuleContext& ctx, const EngineModel& model)
    : ctx_(ctx), model_(model), tb_(ctx.tables->fx) {
    part_chain_.fill(-1);
    // The units of every block exist from the start (≈ 0.5 MB each), so that `render` never
    // allocates when a block first becomes active. DLL-exact mode ignores the blocks.
    if (!ctx_.dll_exact)
        for (std::size_t n = 0; n < midi::xg::kEffectInsertionSize; ++n)
            units_[n] = std::make_unique<Unit>(tb_, ctx_.sample_rate, model_);
}

InsertionFx::~InsertionFx() = default;

InsertionFx::Type InsertionFx::type_of(const midi::xg::EffectInsertion& e) const {
    if (Xg20Effect::kind_of(e.type_msb(), e.type_lsb()) != Xg20Effect::Kind::None)
        return Type::Xg20;
    const VariationType t = variation_type(tb_, e.type_msb(), e.type_lsb(), false);
    return t.category == 0 && t.type <= 1 ? Type::Thru : Type::Variation;
}

int InsertionFx::chain_of(std::size_t id) const {
    return chain_count_ == 0 || id >= part_chain_.size() ? -1 : part_chain_[id];
}

std::optional<std::uint16_t> InsertionFx::applied_parameter(std::size_t block, std::size_t i) const {
    if (block >= units_.size() || !units_[block] || units_[block]->fresh || i >= 16) return std::nullopt;
    return units_[block]->params[i];
}

void InsertionFx::prepare(Buses& buses) {
    const auto* ram = ctx_.shared.effect_insertion;
    const std::size_t parts = std::min<std::size_t>(ctx_.shared.part_count, Buses::kMaxParts);
    const std::size_t old = chain_count_;
    chain_count_ = 0;
    if (old) part_chain_.fill(-1);
    if (ram && ctx_.shared.parts) {
        for (std::size_t n = 0; n < ram->size(); ++n) {
            const auto& e = (*ram)[n];
            const std::size_t part = e.part();
            if (part >= parts || type_of(e) == Type::Thru) continue;
            int c = part_chain_[part];
            if (c < 0) {
                if (chain_count_ == chains_.size()) continue;
                c = static_cast<int>(chain_count_++);
                Chain& ch = chains_[static_cast<std::size_t>(c)];
                if (ch.part != part) ch.primed = false;
                ch.part = part;
                ch.count = 0;
                part_chain_[part] = static_cast<std::int8_t>(c);
            }
            Chain& ch = chains_[static_cast<std::size_t>(c)];
            if (ch.count == kMaxBlocksPerChain) continue;
            ch.blocks[ch.count++] = static_cast<std::uint8_t>(n);
            if (!units_[n]) units_[n] = std::make_unique<Unit>(tb_, ctx_.sample_rate, model_);
        }
    }
    buses.part_chain = part_chain_;
    buses.chains = chain_count_;
    for (std::size_t i = 0; i < chain_count_; ++i) {
        buses.chain_l[i].fill(0.0f);
        buses.chain_r[i].fill(0.0f);
    }
}

void InsertionFx::sync(Unit& u, const midi::xg::EffectInsertion& e, const midi::Part& part) {
    const bool type_changed = u.fresh || e.type_writes != u.writes || e.type_msb() != u.msb ||
                              e.type_lsb() != u.lsb;
    u.fresh = false;
    u.writes = e.type_writes;
    u.msb = e.type_msb();
    u.lsb = e.type_lsb();
    Params p{};
    for (std::size_t i = 0; i < 16; ++i) p[i] = e.param(i);
    if (type_changed) {
        u.type = type_of(e);
        u.running = false;
        u.quiet = 0;
        if (u.type == Type::Variation) {
            u.ctrl.set_variation_type(variation_type(tb_, u.msb, u.lsb, false));
            u.var.clear();
        }
    }
    apply_control(u, e, part, p);
    if (!type_changed && p == u.params) return;
    if (u.type == Type::Variation) {
        for (std::uint8_t i = 0; i < 16; ++i)
            if (type_changed ? p[i] != u.ctrl.parameter(Block::Variation, i) : p[i] != u.params[i])
                u.ctrl.set_parameter(Block::Variation, i, p[i]);
        u.var.load(u.ctrl);
    } else if (u.type == Type::Xg20) {
        u.own.set(Xg20Effect::kind_of(u.msb, u.lsb), p);
    }
    u.params = p;
}

void InsertionFx::apply_control(const Unit& u, const midi::xg::EffectInsertion& e, const midi::Part& part,
                                std::array<std::uint16_t, 16>& p) {
    // Control depths 03 nn 0D..11 (MW, bend, CAT, AC1, AC2; MU2000, measured with S-MU2000):
    // the law of the 2006LE variation (ControlLaw::Centred, offset scaled to the parameter's
    // range), applied at once in the block of the controller event and kept on the parameter
    // through later parameter writes and type changes (effective value = RAM value + offset).
    const std::array<std::uint8_t, 5> depths = {e[0x0D], e[0x0E], e[0x0F], e[0x10], e[0x11]};
    if (depths == std::array<std::uint8_t, 5>{64, 64, 64, 64, 64}) return;
    const int offset = control_offset(depths.data(), part, ControlLaw::Centred);
    if (offset == 0) return;
    const int index = controlled_parameter(u.msb);
    if (index < 0) return;
    const auto i = static_cast<std::size_t>(index);
    std::uint16_t lo = 0, hi = 0;
    if (u.type == Type::Variation) {
        if (!u.ctrl.parameter_range(Block::Variation, static_cast<std::uint8_t>(i), lo, hi)) return;
    } else if (u.type == Type::Xg20) {
        // The MU2000's ranges: ring modulator frequency coarse 0..127, Lo-Fi dry/wet 1..127.
        const Xg20Effect::Kind k = Xg20Effect::kind_of(u.msb, u.lsb);
        if (k == Xg20Effect::Kind::RingMod) lo = 0, hi = 127;
        else if (k == Xg20Effect::Kind::LoFi) lo = 1, hi = 127;
        else return;
    } else {
        return;
    }
    p[i] = static_cast<std::uint16_t>(std::clamp(p[i] + scale_control(offset, lo, hi), int{lo}, int{hi}));
}

std::array<float, 7> InsertionFx::gains(const midi::Part& part, bool& to_variation) const {
    // Gains of the chain output into dry L/R, reverb, chorus L/R, variation L/R: the part's
    // volume / pan / sends as the voices of an uninserted part get them (the chain pre-bus
    // holds the voices at unity part gain without part pan; see the engines' tone modules).
    std::array<float, 7> g{};
    const midi::xg::EffectData& fx = *ctx_.shared.effect1;
    const bool system = fx.variation_connection() == 1;
    to_variation = !system && part.id == fx.variation_part();
    if (to_variation) {
        g[5] = g[6] = 1.0f;
        return g;
    }
    const midi::xg::MultiPart& r = *part.ram;
    const float base = model_.insertion_chain_attenuation(*ctx_.tables, ctx_.shared, part);
    // Part pan 0 (CC10 = 0): hard left, as the voices of a melodic part take it and as the
    // MU2000 pans an inserted part [meas: S-MU2000, distortion block, CC10 = 0: left only];
    // on a drum part (random pan per key, which a chain cannot apply) the centre.
    const int pan = r.pan == 0 ? (r.part_mode == 0 ? 1 : 64) : std::min<int>(r.pan, 127);
    const int pl = tb_.kMixerPan[static_cast<std::size_t>(128 - pan)];
    const int pr = tb_.kMixerPan[static_cast<std::size_t>(pan)];
    const int c0 = model_.insertion_chain_offset;
    const auto lin = [](float att) { return att >= 255.0f ? 0.0f : std::exp2(-att / 16.0f); };
    const auto lvl = [this](std::uint8_t v) { return static_cast<int>(tb_.kMixerLevel[std::min<std::uint8_t>(v, 127)]); };
    const int dry = system ? lvl(r.dry_level) : 0;
    if (dry < 255) {
        g[0] = lin(base + pl + dry);
        g[1] = lin(base + pr + dry);
    }
    if (r.reverb_send > 0) {
        const int s = lvl(r.reverb_send) - c0;
        g[2] = model_.insertion_reverb_panned ? 0.5f * (lin(base + pl + s) + lin(base + pr + s)) : lin(base + s);
    }
    const int term = model_.insertion_chorus_pan_term ? model_.insertion_chorus_pan_term[64] : 0;
    if (r.chorus_send > 0) {
        const int s = lvl(r.chorus_send) + (chorus_stereo_input(fx) ? 0 : term) - c0;
        g[3] = lin(base + pl + s);
        g[4] = lin(base + pr + s);
    }
    if (system && r.variation_send > 0) {
        const int s = lvl(r.variation_send) + (variation_stereo_input(tb_, fx) ? 0 : term) - c0;
        g[5] = lin(base + pl + s);
        g[6] = lin(base + pr + s);
    }
    return g;
}

void InsertionFx::process(Buses& buses, std::size_t frames) {
    if (chain_count_ == 0) return;
    const auto& ram = *ctx_.shared.effect_insertion;
    const float threshold = model_.bus_unit;
    const auto hold = static_cast<std::size_t>(model_.variation_hold_seconds * ctx_.sample_rate);
    const auto peak = [frames](const float* x) {
        float m = 0;
        for (std::size_t i = 0; i < frames; ++i) m = std::max(m, std::fabs(x[i]));
        return m;
    };
    for (std::size_t c = 0; c < chain_count_; ++c) {
        Chain& ch = chains_[c];
        float* l = buses.chain_l[c].data();
        float* r = buses.chain_r[c].data();
        for (std::size_t k = 0; k < ch.count; ++k) {
            Unit& u = *units_[ch.blocks[k]];
            sync(u, ram[ch.blocks[k]], ctx_.shared.parts[ch.part]);
            if (u.type == Type::Thru) continue;
            // Sleep gate as the variation's: run while the input sounds and for the hold time
            // after the output went silent.
            if (!u.running && std::max(peak(l), peak(r)) >= threshold) {
                u.running = true;
                u.quiet = 0;
            }
            if (!u.running) continue;
            if (u.type == Type::Variation) u.var.process(l, r, frames);
            else u.own.process(l, r, frames);
            if (std::max(peak(l), peak(r)) >= threshold) u.quiet = 0;
            else if ((u.quiet += frames) >= hold) u.running = false;
        }
        bool to_var = false;
        const std::array<float, 7> g = gains(ctx_.shared.parts[ch.part], to_var);
        if (!ch.primed) ch.gain = g;
        ch.primed = true;
        const float inv = 1.0f / static_cast<float>(frames);
        const std::array<float*, 7> dst = {buses.dry_l.data(),    buses.dry_r.data(),
                                           buses.reverb.data(),   buses.chorus_l.data(),
                                           buses.chorus_r.data(), buses.variation_l.data(),
                                           buses.variation_r.data()};
        for (std::size_t o = 0; o < 7; ++o) {
            float a = ch.gain[o];
            if (a == 0.0f && g[o] == 0.0f) continue;
            const float d = (g[o] - a) * inv;
            float* out = dst[o];
            if (o == 2) {
                for (std::size_t i = 0; i < frames; ++i, a += d) out[i] += a * 0.5f * (l[i] + r[i]);
            } else {
                const float* x = (o == 0 || o == 3 || o == 5) ? l : r;
                for (std::size_t i = 0; i < frames; ++i, a += d) out[i] += a * x[i];
            }
        }
        ch.gain = g;
    }
}

bool InsertionFx::active() const {
    for (std::size_t c = 0; c < chain_count_; ++c)
        for (std::size_t k = 0; k < chains_[c].count; ++k)
            if (units_[chains_[c].blocks[k]]->running) return true;
    return false;
}

} // namespace hvyxg::fx
