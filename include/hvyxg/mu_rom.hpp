// Copyright (C) 2026 Kevin H. Patterson
// SPDX-License-Identifier: BSD-3-Clause (see LICENSE)
// Translated from SXG-Create-NF, Copyright (c) 2026 Soundshock, BSD-3-Clause (THIRD_PARTY.md)
#pragma once
// Yamaha MU ROM sets → an S-YXG50 MU bank, without SXG-Create-NF (docs/ENGINE.md, "MU banks",
// "MU ROMs"). The converter is a C++ port of SXG-Create-NF's default conversion (commit
// 0086663, THIRD_PARTY.md): it builds in memory the same big-layout .TBL (with its MAP2
// voice-map appendix) and .UPCM that SXG-Create-NF's main.py writes, byte for byte, and loads
// them as `enable_mu` does. Only data tables are read from the program ROM; nothing in it is
// executed.
//
//   hvyxg::MuRomOptions r{.roms = {"roms/mu1000", "roms/waves"}};
//   auto bank = hvyxg::load_mu_roms(r);           // or Synth::Options::bank_paths

#include <cstdint>
#if !HVYXG_NO_FILES
#include <filesystem>
#endif
#include <memory>
#include <span>
#include <string>
#include <vector>

#include "hvyxg/types.hpp"

namespace hvyxg {

class Bank;

/// How the program ROM's high and low halves are stored: two files, or one joined file that
/// holds both (found by splitting it and matching the halves' CRC-32s).
enum class MuProgramForm : std::uint8_t {
    Pair,                ///< two files, high then low
    Concatenated,        ///< one file: one half, then the other
    Interleaved,         ///< one file: 16-bit words alternating between the halves (the CPU's view)
    InterleavedSwapped,  ///< the same with each word byte-swapped (the S-MU2000 flash image)
};

/// The converted tables, as SXG-Create-NF names them (e.g. SXGMU1KX.TBL / SXGMU1KX.UPCM).
struct MuTables {
    std::vector<std::uint8_t> tbl, upcm;
    std::string tbl_name, upcm_name;
};

/// Find a complete, supported ROM set among ROM images in memory (any order: the program ROM's
/// halves or one joined program file, the wave ROMs; other images are ignored) by CRC-32 and
/// convert it with the S-YXG50 engine tables (about 1.5 s and ~0.2 GB for the MU1000). False
/// and `error` when no complete set is there or the tables are missing. Load the result with
/// `load_bank` (BankMemory, or the vector overload) and `BankOptions::enable_mu`.
bool convert_mu_roms(std::span<const std::span<const std::uint8_t>> images, MuRomModel model,
                     const EngineTables& tables, MuTables& out, Error& error);

#if !HVYXG_NO_FILES
// ---- From files, throwing `BankError` (hosted builds) ----------------------------------------

struct MuRomOptions {
    /// ROM files and / or directories (searched recursively); files are recognised by CRC-32.
    std::vector<std::filesystem::path> roms;
    /// Which set to use when the files hold several complete ones (`Auto`: MU1000, then
    /// MU2000, then MU128, the order SXG-Create-NF prefers).
    MuRomModel model = MuRomModel::Auto;
    /// Optional cache directory for the converted tables (created if needed); empty = none.
    std::filesystem::path cache_dir;
};

/// A complete ROM set found among `MuRomOptions::roms`.
struct MuRomSet {
    MuRomModel model = MuRomModel::Auto;
    std::string name;  ///< e.g. "MU1000 v2.01"
    std::vector<std::filesystem::path> program;  ///< high, low (Pair) or the joined file
    MuProgramForm program_form = MuProgramForm::Pair;
    bool low_first = false;                      ///< a joined file with the low half first
    std::vector<std::filesystem::path> wave;     ///< in bus order
    std::vector<std::uint32_t> crcs;             ///< program (high, low) then wave ROMs
};

/// Find a complete, supported ROM set. Throws `BankError` naming missing or unrecognised ROMs.
MuRomSet find_mu_roms(const MuRomOptions& options);
/// Convert a ROM set (about 1.5 s and ~0.2 GB for the MU1000) with the S-YXG50 engine tables
/// (hvyxg/engine_tables.hpp). Throws `BankError`.
MuTables convert_mu_roms(const MuRomSet& set, const EngineTables& tables);
/// Convert (or read `cache_dir`) and load as an `enable_mu` bank; `bank_options.enable_mu`
/// is implied. Throws `BankError`.
std::shared_ptr<const Bank> load_mu_roms(const MuRomOptions& options, const BankOptions& bank_options = {});

#endif

} // namespace hvyxg
