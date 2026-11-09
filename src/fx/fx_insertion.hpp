// Copyright (C) 2026 Kevin H. Patterson
// SPDX-License-Identifier: BSD-3-Clause (see LICENSE)
#pragma once
// XG2.0 insertion effects (SysEx 03 nn ..), hvyxg superset behaviour: neither Yamaha DLL
// makes them audible (the S-YXG50 does not parse them, the 2006LE parses them and leaves
// them inert).
//
// Signal flow (the Yamaha XG 2.0 specification, "Effect routing"): the voices of a part with insertion
// blocks render their dry pair, at unity part gain and without the part pan, into a chain
// pre-bus (`Buses::chain_l/r`); the blocks assigned to the part run on it in block-number
// order; the result then takes the part's volume / expression / master and pan and goes to
// the dry, reverb, chorus and (system connection) variation buses by the part's dry level and
// sends. A part that is also the variation insertion part feeds the variation bus at unity
// instead, and the variation's own insertion return applies volume, pan and sends.
//
// Types that exist as variation types run the DLLs' decoded variation control and kernels
// (one `EffectControl` + `VariationUnit` per block, insertion connection: dry/wet respected and
// halved like the DLLs' insertion variation). A few XG2.0-only types are hvyxg's own
// designs (`Xg20Effect`); every other type is Thru.

#include <array>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>

#include "buses.hpp"
#include "fx/fx_control.hpp"
#include "fx/fx_variation.hpp"
#include "module.hpp"

namespace hvyxg::midi::xg {
struct EffectInsertion;
}

namespace hvyxg::fx {

struct EngineModel;

/// Default parameters 1..16 of the XG2.0-only insertion types hvyxg implements
/// (the Yamaha XG 2.0 specification, parameter lists), or nullopt for any other type.
std::optional<std::array<std::uint16_t, 16>> xg20_insertion_defaults(std::uint8_t msb, std::uint8_t lsb);

/// hvyxg's designs for XG2.0-only insertion types (no DLL reference exists):
/// - Compressor (53 00): stereo-linked peak detector with attack / release one-poles (tables
///   #8 / #9), hard-knee gain computer (threshold p3 − 127 dB, ratio table #10), output level
///   (table #18).
/// - Noise Gate (54 00): the same detector; the gain opens (attack) above the threshold and
///   closes (release) below it.
/// - Ring Modulator (71 00): sine carrier (table #22, fine p2) with a triangle / sine LFO on
///   its frequency (depth p4, rate table #1), HPF / LPF on the wet signal (table #3), dry/wet.
/// - Lo-Fi (5E 00): sample-and-hold to 44.1 kHz / (p1 + 1), word length p2, resonant LPF
///   (p4 / p6), output gain p3 − 6 dB, dry/wet.
class Xg20Effect {
public:
    enum class Kind : std::uint8_t { None, Compressor, NoiseGate, RingMod, LoFi };
    static Kind kind_of(std::uint8_t msb, std::uint8_t lsb);

    Xg20Effect(const Tables& tables, float sample_rate);
    void set(Kind kind, const std::array<std::uint16_t, 16>& params);
    void clear();
    void process(float* l, float* r, std::size_t frames);

private:
    struct Biquad {
        float b0 = 1, b1 = 0, b2 = 0, a1 = 0, a2 = 0;
        float x1 = 0, x2 = 0, y1 = 0, y2 = 0;
        float run(float x) {
            const float y = b0 * x + b1 * x1 + b2 * x2 - a1 * y1 - a2 * y2;
            x2 = x1;
            x1 = x;
            y2 = y1;
            y1 = y;
            return y;
        }
    };
    static Biquad lowpass(float fs, float f, float q);
    static Biquad highpass(float fs, float f, float q);
    void dynamics(float* l, float* r, std::size_t n, bool gate);
    void ring(float* l, float* r, std::size_t n);
    void lofi(float* l, float* r, std::size_t n);

    const Tables& tb_;
    float rate_;
    Kind kind_ = Kind::None;
    std::array<std::uint16_t, 16> p_{};
    // Dynamics.
    float att_ = 0, rel_ = 0, thr_db_ = 0, ratio_ = 1, out_ = 1;
    float env_ = 0, gain_ = 1;
    // Ring modulator.
    double carrier_hz_ = 1000, carrier_phase_ = 0, lfo_hz_ = 0, lfo_phase_ = 0;
    float lfo_depth_ = 0;
    bool lfo_sine_ = false;
    bool hpf_on_ = false, lpf_on_ = false;
    std::array<Biquad, 2> hpf_{}, lpf_{};
    float dry_ = 0, wet_ = 1;
    // Lo-Fi.
    double hold_step_ = 1, hold_pos_ = 0;
    float hold_l_ = 0, hold_r_ = 0, quant_ = 0, gain_out_ = 1;
    bool stereo_ = true;
};

class InsertionFx {
public:
    InsertionFx(const ModuleContext& ctx, const EngineModel& model);
    ~InsertionFx();
    InsertionFx(const InsertionFx&) = delete;
    InsertionFx& operator=(const InsertionFx&) = delete;

    /// Before the voices render: build the part → chain map from the insertion RAM. (The
    /// units are allocated at construction: neither this nor `process` allocates.)
    void prepare(Buses& buses);
    /// After the voices render, before the system effects: run each chain and mix it into
    /// the dry / reverb / chorus / variation buses.
    void process(Buses& buses, std::size_t frames);
    /// A chain's effect may still ring.
    bool active() const;

    /// Introspection for tests: the chain of part `id` (−1 = none).
    int chain_of(std::size_t id) const;
    /// Introspection for tests: parameter `i` (0..15) of insertion block `block` as its unit
    /// last applied it (RAM value plus the control-depth offset), or nullopt before the block
    /// has run.
    std::optional<std::uint16_t> applied_parameter(std::size_t block, std::size_t i) const;

    static constexpr std::size_t kMaxBlocksPerChain = 4;

private:
    struct Unit;
    struct Chain {
        std::size_t part = 0;
        std::array<std::uint8_t, kMaxBlocksPerChain> blocks{};
        std::size_t count = 0;
        // Output gains of the previous block (ramped across a block): dry L/R, reverb,
        // chorus L/R, variation L/R.
        std::array<float, 7> gain{};
        bool primed = false;
    };
    enum class Type : std::uint8_t { Thru, Variation, Xg20 };
    Type type_of(const midi::xg::EffectInsertion& e) const;
    /// Bring the unit up to the block's RAM (type, parameters) with the control depths of the
    /// block applied from `part`'s controllers.
    void sync(Unit& u, const midi::xg::EffectInsertion& e, const midi::Part& part);
    /// Add the control-depth offset (03 nn 0D..11) to the type's controlled parameter in `p`.
    static void apply_control(const Unit& u, const midi::xg::EffectInsertion& e, const midi::Part& part,
                              std::array<std::uint16_t, 16>& p);
    std::array<float, 7> gains(const midi::Part& part, bool& to_variation) const;

    ModuleContext ctx_;
    const EngineModel& model_;
    const Tables& tb_;
    std::array<std::unique_ptr<Unit>, 128> units_;
    std::array<Chain, Buses::kMaxInsertionChains> chains_{};
    std::size_t chain_count_ = 0;
    std::array<std::int8_t, Buses::kMaxParts> part_chain_{};
};

} // namespace hvyxg::fx
