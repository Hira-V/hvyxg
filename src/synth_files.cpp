// Copyright (C) 2026 Kevin H. Patterson
// SPDX-License-Identifier: BSD-3-Clause (see LICENSE)
// Synth(Options): the bank and the engine tables from files (hosted builds), over synth.cpp.
#include <stdexcept>
#include <string>

#include "bank_search.hpp"
#include "bank_impl.hpp"
#include "hvyxg/engine_tables.hpp"
#include "hvyxg/plugin_tables.hpp"
#include "hvyxg/synth.hpp"

namespace hvyxg {

namespace {

/// Every DLL the options name, in order: `plugin_file`, `plugin_files`, then `plugin_path`.
std::vector<std::filesystem::path> option_dlls(const Synth::Options& o) {
    std::vector<std::filesystem::path> dlls;
    if (!o.plugin_file.empty()) dlls.push_back(o.plugin_file);
    dlls.insert(dlls.end(), o.plugin_files.begin(), o.plugin_files.end());
    if (!o.plugin_path.empty()) {
        const auto found = find_dlls(o.plugin_path);
        dlls.insert(dlls.end(), found.begin(), found.end());
    }
    return dlls;
}

/// The bank: `bank`, else the explicit files, else one found under `bank_paths`, else the
/// first DLL that carries one (a stock bank there must be the stock bank).
std::shared_ptr<const Bank> open_bank(Synth::Options& o, const std::vector<std::filesystem::path>& dlls) {
    if (o.bank) return o.bank;
    const BankOptions bo{.tables = o.tables ? o.tables : load_engine_tables(dlls),
                         .enable_mu = o.enable_mu,
                         .mu_layout = o.mu_layout,
                         .mu_voice_map = o.mu_voice_map};
    if (!o.bin_data.empty()) return load_bank(std::move(o.bin_data), std::move(o.wave_data), o.model, bo);
    if (!o.bin_file.empty() || !o.data_file.empty() || (o.bank_paths.empty() && dlls.empty()))
        return load_bank(o.bin_file, o.data_file, o.model, bo);
    if (!o.bank_paths.empty())
        return bank_search::load({.paths = o.bank_paths, .model = o.model, .enable_mu = o.enable_mu,
                                  .mu_model = o.mu_rom_model, .mu_cache = o.mu_cache, .bank_options = bo});
    std::string why;
    for (const std::filesystem::path& p : dlls) {
        try {
            PluginTables t = read_plugin_tables(p);
            bank_search::check_embedded(t);
            return load_bank(std::move(t.bin), std::move(t.wave), o.model, bo);
        } catch (const BankError& e) {
            if (why.empty()) why = e.what();
        }
    }
    throw BankError(BankError::Kind::Header,
                    "no bank: give --bin / --data, --bank-path, or a DLL that carries one (" + why + ")");
}

} // namespace

namespace {

/// The options checked, and the bank they name with its engine tables. Throws.
std::shared_ptr<const Bank> open_checked(Synth::Options& options) {
    std::string why;
    if (!Synth::check(options, why)) throw std::invalid_argument(why);
    const std::vector<std::filesystem::path> dlls = option_dlls(options);
    auto bank = open_bank(options, dlls);
    if (!bank->tables()) throw BankError(BankError::Kind::Header, "the bank has no engine tables");
    require_engine_tables(*bank->tables(), bank->type());
    return bank;
}

} // namespace

Synth::Synth(Options options) : Synth(open_checked(options), options) {}

} // namespace hvyxg
