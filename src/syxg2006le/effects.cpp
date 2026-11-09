// Copyright (C) 2026 Kevin H. Patterson
// SPDX-License-Identifier: BSD-3-Clause (see LICENSE)
#include "syxg2006le/effects.hpp"

#include <algorithm>

#include "engine_tables.hpp"
#include "fx/fx_system.hpp"
#include "syxg2006le/controllers.hpp"
#include "fx/fx_tables.hpp"
#include "midi/part.hpp"
#include "syxg2006le/tbl/dll_tables.hpp"

namespace hvyxg::syxg2006le {

namespace {

int v256(int v) { return v >= 127 ? 256 : 2 * std::max(v, 0); }

// The voice level law: expression × volume × XG master volume.
int insertion_attenuation(const EngineTables& t, const EngineState& shared, const midi::Part& part) {
    const int idx = std::min(((v256(part.controller.expression) * v256(part.ram->volume)) >> 8) *
                                 master256(shared) >> 8,
                             255);
    return t.le.kVolumeAtt[static_cast<std::size_t>(idx)];
}

float chain_attenuation(const EngineTables& t, const EngineState& shared, const midi::Part& part) {
    return static_cast<float>(insertion_attenuation(t, shared, part));
}

} // namespace

/// S-YXG2006LE.DLL: mixer slews every ≈ 12 ms, input gains by 10/128, sends by 20/128 (dump);
/// the DSP scales the chorus returns ×0.5 and chorus → reverb ×0.3536;
/// sleep after 0x800 (reverb) / 0x200 (chorus) silent blocks of 64; bus unit 2^-13
/// (output = bus · 2^-13).
constexpr fx::EngineModel kModel = {
    .tick_seconds = 0.012f,
    .input_step = 10,
    .send_step = 20,
    .chorus_return_scale = 0.5f,
    .chorus_send_scale = 0.35355339f,
    .bus_unit = 1.0f / 8192.0f,
    .reverb_hold_seconds = 0x800 * 64 / 44100.0f,
    .chorus_hold_seconds = 0x200 * 64 / 44100.0f,
    .flanger_delay_63 = false,
    .variation_limits = nullptr,  // the tables' kVarLimits2006LE (make_system_effects)
    .variation_hold_seconds = 0x800 * 64 / 44100.0f,
    .variation_control_ticked = false,
    .connection_write_resends = false,
    // Superset: the DSP step that halves the chorus return zeroes the
    // variation words; the same ×0.5 / ×0.3536 as the chorus keeps the variation's level
    // relative to dry in line with the S-YXG50's (×1 put it ≈ 7 dB above).
    .variation_return_scale = 0.5f,
    .variation_reverb_scale = 0.35355339f,
    .variation_chorus_scale = 0.35355339f,
    .insertion_attenuation = insertion_attenuation,
    .insertion_chain_offset = 8,
    .insertion_chain_attenuation = chain_attenuation,
    .insertion_reverb_panned = true,
    .insertion_chorus_pan_term = nullptr,
    .variation_inert = true,
};

/// Full XG (`Synth::Options::full_xg_2006le`): the S-YXG50's variation parameter ranges and
/// its distortion clip level (±131071 · 2^-15, +12 dB above full scale instead of +24 dB).
/// The ranges differ in the delay times of Delay L,C,R / L,R (7200 against 7150) and Echo /
/// Cross Delay (3600 against 3550, 0.1 ms), the variation flanger's delay offset (127 against
/// 63) and the early-reflection / gate room size and initial delay (127 against 44 / 63, which
/// the effect control clamps to 44 / 63 on both). The chorus flanger's delay offset is limited
/// to 63 as the S-YXG50 engine does (`flanger_delay_63`; XG range 0..63). Every other effect
/// law stays the 2006LE's.
constexpr fx::EngineModel kModelFull = [] {
    fx::EngineModel m = kModel;
    m.flanger_delay_63 = true;
    m.clip_unit = 1.0f / 32768.0f;
    return m;
}();

std::unique_ptr<SystemEffects> make_system_effects(const ModuleContext& ctx) {
    fx::EngineModel m = ctx.full_xg ? kModelFull : kModel;
    m.variation_limits = ctx.full_xg ? ctx.tables->fx.kVarLimitsSyxg50.data() : ctx.tables->fx.kVarLimits2006LE.data();
    return std::make_unique<fx::SystemFx>(ctx, m);
}

} // namespace hvyxg::syxg2006le
