// Copyright (C) 2026 Kevin H. Patterson
// SPDX-License-Identifier: BSD-3-Clause (see LICENSE)
#include "bank_impl.hpp"

#include "bank_io.hpp"

#include "hvyxg/engine_tables.hpp"
#include "syxg2006le/bank.hpp"
#include "syxg2006le/tbl/header.hpp"
#include "syxg50/bank.hpp"
#include "syxg50/bin_table.hpp"

namespace hvyxg {

Bank::~Bank() = default;

void descramble_wave(std::uint8_t key, std::span<std::uint8_t> data) {
    for (std::size_t i = 0; i < data.size(); ++i) {
        const auto x = static_cast<std::uint8_t>(data[i] ^ key ^ (i & 0xFF));
        data[i] = static_cast<std::uint8_t>((x << 4) | (x >> 4));
        key = static_cast<std::uint8_t>(~key);
    }
}

std::optional<Model> sniff_model(std::span<const std::uint8_t> bin) {
    if (syxg2006le::tbl::check_header(bin)) return Model::Syxg2006le;
    if (syxg50::BinTable::check_header(bin)) return Model::Syxg50;
    return std::nullopt;
}

namespace {

/// The files' bytes and their padding, owned by the bank.
struct Owned {
    std::vector<std::uint8_t> bin, wave;
};

std::shared_ptr<const Bank> load(const BankMemory& memory, Model model, const BankOptions& options,
                                 std::shared_ptr<const void> owned, Error& error) {
    if (model == Model::Auto) {
        const std::span<const std::uint8_t> bin =
            memory.bin.first(memory.bin.size() >= BankMemory::kPad ? memory.bin.size() - BankMemory::kPad : 0);
        const auto m = sniff_model(bin);
        if (!m) {
            error = {ErrorKind::Header, "bin table: unknown header (neither S-YXG50 nor S-YXG2006LE)"};
            return nullptr;
        }
        model = *m;
    }
    if (options.enable_mu && model != Model::Syxg50) {
        error = {ErrorKind::Header, "--enable-mu needs an S-YXG50 (SXG-Create-NF) bin table"};
        return nullptr;
    }
    std::shared_ptr<const Bank> bank;
    switch (model) {
    case Model::Syxg50: bank = syxg50::load_bank(memory, options, std::move(owned), error); break;
    case Model::Syxg2006le: bank = syxg2006le::load_bank(memory, options, std::move(owned), error); break;
    default: error = {ErrorKind::Header, "unknown bank model"}; return nullptr;
    }
    if (!bank) return nullptr;
    // After the tables themselves: a malformed bank is reported as such, DLLs or not.
    if (!options.tables) {
        error = {ErrorKind::Header, "no engine tables (BankOptions::tables)"};
        return nullptr;
    }
    if (!check_engine_tables(*options.tables, model, error)) return nullptr;
    return bank;
}

} // namespace

std::shared_ptr<const Bank> load_bank(const BankMemory& memory, Model model, const BankOptions& options,
                                      Error& error) noexcept {
    return load(memory, model, options, nullptr, error);
}

std::shared_ptr<const Bank> load_bank(std::vector<std::uint8_t> bin, std::vector<std::uint8_t> data, Model model,
                                      const BankOptions& options, Error& error) noexcept {
    auto owned = std::make_shared<Owned>(Owned{std::move(bin), std::move(data)});
    owned->bin.resize(owned->bin.size() + BankMemory::kPad);
    if (!owned->wave.empty()) owned->wave.resize(owned->wave.size() + BankMemory::kPad);
    const BankMemory memory{owned->bin, owned->wave};
    return load(memory, model, options, std::move(owned), error);
}

} // namespace hvyxg
