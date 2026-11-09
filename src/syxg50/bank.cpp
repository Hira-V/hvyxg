// Copyright (C) 2026 Kevin H. Patterson
// SPDX-License-Identifier: BSD-3-Clause (see LICENSE)
#include "syxg50/bank.hpp"

#include "bank_impl.hpp"
#include "engine_tables.hpp"
#include "syxg50/bank_data.hpp"
#include "syxg50/effects.hpp"
#include "syxg50/tone_module.hpp"
#include "syxg50/bin_table.hpp"

namespace hvyxg::syxg50 {

namespace {

class Syxg50Bank final : public BankImpl {
public:
    Syxg50Bank(BinTable tbl, MuTable mu, std::shared_ptr<const EngineTables> tables, std::shared_ptr<const void> owned)
        : BankImpl(std::move(tables), std::move(owned)), data_(std::make_shared<const BankData>(std::move(tbl), mu)) {}

    Model type() const override { return Model::Syxg50; }
    bool mu() const override { return data_->mu().on; }
    std::uint16_t default_polyphony() const override { return 128; }
    bool xg_parameter_table() const override { return true; }
    /// The part template gives every part multi (1); the part reset
    /// sets the drum part (receive channel 10) to single in GS mode
    /// (= 2) [code; meas: drum voice FIFO logs of whole songs].
    std::uint8_t drum_part_key_assign(ResetMode mode) const override {
        return mode == ResetMode::GS ? 0 : 1;
    }

    std::optional<DrumKitSetup> drum_setup(std::uint8_t bank_msb,
                                           std::uint8_t program) const override {
        return syxg50::drum_setup(tables()->syxg50, *data_, bank_msb, program);
    }

    std::unique_ptr<ToneModule> make_tone_module(const ModuleContext& ctx) const override {
        return std::make_unique<ToneModule50>(data_, ctx);
    }
    std::unique_ptr<SystemEffects> make_system_effects(const ModuleContext& ctx) const override {
        return syxg50::make_system_effects(ctx, data_->mu().on);
    }

private:
    std::shared_ptr<const BankData> data_;
};

} // namespace

std::shared_ptr<const Bank> load_bank(const BankMemory& memory, const BankOptions& options,
                                      std::shared_ptr<const void> owned, Error& error) {
    BinTable tbl;
    if (!BinTable::parse(memory, tbl, error)) return nullptr;
    const bool big = tbl.looks_big();
    MuTable mu;
    if (options.enable_mu) {
        mu.on = true;
        mu.big = options.mu_layout == MuLayout::Big || (options.mu_layout == MuLayout::Auto && big);
        // The appendix byte +4 names the default map (0 native, 1 MU Basic).
        const bool table_basic = tbl.appendix != 0 && tbl.bin[tbl.appendix + 4] == 1;
        mu.basic = options.mu_voice_map == MuVoiceMap::Basic ||
                   (options.mu_voice_map == MuVoiceMap::Default && table_basic);
    } else if (big) {
        error = {ErrorKind::Table,
                 "S-YXG50 bin table: SXG-Create-NF big-layout MU bank (MU100 and later); "
                 "load it with enable_mu (render_yxg --enable-mu)"};
        return nullptr;
    }
    return std::make_shared<Syxg50Bank>(std::move(tbl), mu, options.tables, std::move(owned));
}

} // namespace hvyxg::syxg50
