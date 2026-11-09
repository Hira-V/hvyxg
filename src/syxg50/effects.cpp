// Copyright (C) 2026 Kevin H. Patterson
// SPDX-License-Identifier: BSD-3-Clause (see LICENSE)
#include "syxg50/effects.hpp"

#include <algorithm>
#include <cmath>

#include "engine_tables.hpp"
#include "fx/fx_system.hpp"
#include "fx/fx_tables.hpp"
#include "syxg50/dll_tables.hpp"
#include "syxg50/laws.hpp"
#include "midi/part.hpp"

namespace hvyxg::syxg50 {

namespace {

// master, part volume and expression through the volume.
int insertion_attenuation(const EngineTables& t, const EngineState& shared, const midi::Part& part) {
    const int master = effective_master_volume(shared);
    return t.fx.kMixerVolume[static_cast<std::size_t>(std::clamp(master, 0, 127))] +
           t.fx.kMixerVolume[std::min<std::uint8_t>(part.ram->volume, 127)] +
           t.fx.kMixerVolume[std::min<std::uint8_t>(part.controller.expression, 127)];
}

// The voices' part gain (laws.hpp) through the level table, which follows
// 40·log10(L / 128) (0.75 dB units, Tlev[64] = 16).
float chain_attenuation(const EngineTables&, const EngineState& shared, const midi::Part& part) {
    const int master = effective_master_volume(shared);
    const int g = law::part_gain(part.ram->volume, part.controller.expression, master);
    if (g <= 0) return 255.0f;
    return static_cast<float>(-40.0 * std::log10(g / 128.0) / (96.33 / 256.0));
}

} // namespace

/// syxg50.dll: mixer slews every
/// 10 ms, input gains by 4/128, sends by 8/128; the DSP uses
/// the mixer words as they are; sleep after 0x400 (reverb) / 0x100 (chorus) silent blocks of
/// 128; bus unit 2^-15 (output = bus · 2^-15 · master).
constexpr fx::EngineModel kModel = {
    .tick_seconds = 0.010f,
    .input_step = 4,
    .send_step = 8,
    .chorus_return_scale = 1.0f,
    .chorus_send_scale = 1.0f,
    .bus_unit = 1.0f / 32768.0f,
    .reverb_hold_seconds = 0x400 * 128 / 44100.0f,
    .chorus_hold_seconds = 0x100 * 128 / 44100.0f,
    .flanger_delay_63 = true,
    .variation_limits = nullptr,  // the tables' kVarLimitsSyxg50 (make_system_effects)
    .variation_hold_seconds = 0x400 * 128 / 44100.0f,
    .variation_control_ticked = true,
    .insertion_update_in_block = true,
    .connection_write_resends = true,
    .variation_return_scale = 1.0f,
    .variation_reverb_scale = 1.0f,
    .variation_chorus_scale = 1.0f,
    .insertion_attenuation = insertion_attenuation,
    .insertion_chain_offset = 0,
    .insertion_chain_attenuation = chain_attenuation,
    .insertion_reverb_panned = false,
    .insertion_chorus_pan_term = nullptr,  // the tables' kChorusPanAtten
};

/// The SXG-Create-NF patched syxg50.dll (docs/ENGINE.md, "MU banks").
constexpr fx::EngineModel kModelMu = [] {
    fx::EngineModel m = kModel;
    m.mu = true;
    m.reverb_input_step = 32;
    m.chorus_input_step = 52;
    m.variation_input_step = 40;
    return m;
}();

std::unique_ptr<SystemEffects> make_system_effects(const ModuleContext& ctx, bool mu) {
    fx::EngineModel m = mu ? kModelMu : kModel;
    m.variation_limits = ctx.tables->fx.kVarLimitsSyxg50.data();
    m.insertion_chorus_pan_term = ctx.tables->syxg50.kChorusPanAtten.data();
    return std::make_unique<fx::SystemFx>(ctx, m);
}

} // namespace hvyxg::syxg50
