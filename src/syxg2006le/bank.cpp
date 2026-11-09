// Copyright (C) 2026 Kevin H. Patterson
// SPDX-License-Identifier: BSD-3-Clause (see LICENSE)
#include "syxg2006le/bank.hpp"

#include <vector>

#include "bank_impl.hpp"
#include "syxg2006le/bank_data.hpp"
#include "syxg2006le/effects.hpp"
#include "syxg2006le/programs.hpp"
#include "syxg2006le/tone_module.hpp"
#include "syxg2006le/tbl/decrypt.hpp"
#include "syxg2006le/tbl/header.hpp"

namespace hvyxg::syxg2006le {

namespace {

class Syxg2006leBank final : public BankImpl {
public:
    Syxg2006leBank(std::shared_ptr<const BankData> data, std::shared_ptr<const EngineTables> tables,
                   std::shared_ptr<const void> owned)
        : BankImpl(std::move(tables), std::move(owned)), data_(std::move(data)) {}

    Model type() const override { return Model::Syxg2006le; }
    std::uint16_t default_polyphony() const override { return 32; }
    bool xg_parameters_in_gs_mode() const override { return false; }
    std::optional<GsBanks> gs_banks() const override { return GsBanks{119, 118}; }
    bool is_kit_bank(std::uint8_t msb) const override { return is_drum_bank(msb); }
    /// Part descriptors default every part, the drum part included, to multi.
    std::uint8_t drum_part_key_assign(ResetMode) const override { return 1; }

    std::optional<DrumKitSetup> drum_setup(std::uint8_t bank_msb,
                                           std::uint8_t program) const override {
        const auto kit = drum_kit(*data_, bank_msb, program);
        if (!kit) return std::nullopt;
        return kit_setup(*data_, *kit);
    }

    std::unique_ptr<ToneModule> make_tone_module(const ModuleContext& ctx) const override {
        return std::make_unique<ToneModule2006>(data_, ctx);
    }
    std::unique_ptr<SystemEffects> make_system_effects(const ModuleContext& ctx) const override {
        return syxg2006le::make_system_effects(ctx);
    }

private:
    std::shared_ptr<const BankData> data_;
};

} // namespace

std::shared_ptr<const Bank> load_bank(const BankMemory& memory, const BankOptions& options,
                                      std::shared_ptr<const void> owned, Error& error) {
    const std::span<std::uint8_t> bin =
        memory.bin.size() >= BankMemory::kPad ? memory.bin.first(memory.bin.size() - BankMemory::kPad) : memory.bin.first(0);
    const std::span<std::uint8_t> wave =
        memory.wave.size() >= BankMemory::kPad ? memory.wave.first(memory.wave.size() - BankMemory::kPad) : memory.wave.first(0);
    if (!tbl::check_header(bin)) {
        error = {ErrorKind::Header, "S-YXG2006LE voice table: missing \"UTG VPRM\" header"};
        return nullptr;
    }
    if (wave.size() < 2) {
        error = {ErrorKind::Table, "S-YXG2006LE wave table: empty"};
        return nullptr;
    }
    if (reinterpret_cast<std::uintptr_t>(wave.data()) % alignof(std::int16_t) != 0) {
        error = {ErrorKind::Table, "S-YXG2006LE wave table: the buffer is not 2-byte aligned"};
        return nullptr;
    }
    tbl::decrypt(wave);
    auto data = std::make_shared<const BankData>(bin, wave);
    if (data->error()) {
        error = {ErrorKind::Table, std::string("S-YXG2006LE voice table: ") + data->error()};
        return nullptr;
    }
    return std::make_shared<Syxg2006leBank>(std::move(data), options.tables, std::move(owned));
}

} // namespace hvyxg::syxg2006le
