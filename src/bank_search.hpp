// Copyright (C) 2026 Kevin H. Patterson
// SPDX-License-Identifier: BSD-3-Clause (see LICENSE)
#pragma once
// Finding a bank by content (Synth::Options::bank_paths, render_yxg --bank-path): every file
// under the given paths is looked at, whatever its name. Candidates are the stock S-YXG50
// tables (4 MB, 2 MB), the stock S-YXG2006LE tables, other S-YXG50 tables (SXG-Create-NF's MU
// conversions, custom banks) and MU ROM sets (converted in memory). Files found this way are
// accepted as they are, like bin_file / data_file. A bank embedded in a DLL is checked by
// check_embedded: one under a stock name must be the stock bank.

#include <filesystem>
#include <memory>
#include <vector>

#include "bank_impl.hpp"
#include "hvyxg/plugin_tables.hpp"
#include "hvyxg/types.hpp"

namespace hvyxg::bank_search {

struct Request {
    std::vector<std::filesystem::path> paths;
    Model model = Model::Auto;            ///< restricts the engine
    bool enable_mu = false;               ///< pick an MU bank (tables or ROMs)
    MuRomModel mu_model = MuRomModel::Auto;
    std::filesystem::path mu_cache;
    BankOptions bank_options;
};

/// Pick the bank under `paths` and load it. Within a kind the defaults follow the DLLs (the
/// 4 MB S-YXG50 bank before the 2 MB one; ROM sets MU1000, MU2000, MU128); different kinds
/// are an error that lists them, unless `model` / `enable_mu` / `mu_model` decide (an MU model
/// asks for an MU bank). An MU candidate alone implies `enable_mu`. Throws BankError.
std::shared_ptr<const Bank> load(const Request& request);

/// A bank embedded in a DLL under a stock name (SXGBIN41.TBL, SXGBIN21.TBL, SXGBNW6L.TBL) must
/// be that stock bank, byte for byte (XXH3-128); any other embedded bank (SXG-Create-NF's MU
/// conversions) is accepted as it is. Throws BankError.
void check_embedded(const PluginTables& tables);

} // namespace hvyxg::bank_search
