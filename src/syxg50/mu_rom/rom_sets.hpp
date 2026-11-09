// Copyright (C) 2026 Kevin H. Patterson
// SPDX-License-Identifier: BSD-3-Clause (see LICENSE)
// Translated from SXG-Create-NF, Copyright (c) 2026 Soundshock, BSD-3-Clause (THIRD_PARTY.md)
#pragma once
// The MU ROM sets hvyxg converts (CRC-32 lists of SXG-Create-NF utility/dataCRCs.py) and the
// conversion of one set (convert.py's MU1000 / MU128 / MU2000 path), shared by the core
// (mu_rom.cpp: ROM images in memory) and the hosted search through files (mu_rom_files.cpp).

#include <array>
#include <cstdint>
#include <span>
#include <utility>
#include <vector>

#include "hvyxg/mu_rom.hpp"

/// The converter needs 128-bit integers (Python's big ints): the compiler's __int128, or a host's
/// own (HVYXG_INT128_HEADER, syxg50/mu_rom/py.hpp). Without either convert_mu_roms reports that
/// it is not available; give the converted tables instead.
#if defined(__SIZEOF_INT128__) || defined(HVYXG_INT128_HEADER)
#define HVYXG_MU_CONVERTER 1
#else
#define HVYXG_MU_CONVERTER 0
#endif

namespace hvyxg::mu_rom_sets {

using Bytes = std::vector<std::uint8_t>;

struct RomId {
    std::uint32_t crc;
    const char* name;
};

struct SetSpec {
    MuRomModel model;
    const char* name;
    std::array<RomId, 2> program;  ///< high, low
    std::array<RomId, 4> wave;     ///< in bus order
    const char* version;           ///< table version text
    const char* tbl_name;
    const char* upcm_name;
};

inline constexpr std::array<RomId, 4> kWaves1000{
    {{0xCDA1AFD6, "xv364a0.ic49"}, {0x10985ED0, "xv365a0.ic50"}, {0x34913E42, "xw848a0.ic53"}, {0x3728F1F2, "xw849a0.ic54"}}};

/// In the order SXG-Create-NF prefers them.
inline constexpr std::array<SetSpec, 3> kSpecs{{
    {MuRomModel::Mu1000, "MU1000 v2.01", {{{0xD0809297, "mu1000-v2.01-h.bin"}, {0x048A2750, "mu1000-v2.01-l.bin"}}},
     kWaves1000, "SXG MU1000", "SXGMU1KX.TBL", "SXGMU1KX.UPCM"},
    {MuRomModel::Mu2000, "MU2000 v2.01", {{{0xBE3668F6, "mu2000-v2.01-h.bin"}, {0x55921A50, "mu2000-v2.01-l.bin"}}},
     kWaves1000, "SXG MU2000", "SXGMU2KX.TBL", "SXGMU2KX.UPCM"},
    {MuRomModel::Mu128, "MU128 v2.00", {{{0x2891487B, "mu128-v2.00-h.bin"}, {0xCC236DC4, "mu128-v2.00-l.bin"}}},
     {{{0xCDA1AFD6, "xv364a0.ic53"}, {0x10985ED0, "xv365a0.ic54"}, {0x781DFAC6, "xv366a0.ic57"}, {0x91A7533B, "xv376a0.ic58"}}},
     "SXG MU128", "SXGMU128X.TBL", "SXGMU128X.UPCM"},
}};

/// Program ROMs SXG-Create-NF knows but that hvyxg cannot convert (yet).
inline constexpr std::array<RomId, 12> kUnsupported{{
    {0xC31074C0, "MU80 xq556a0 v1.04"},  {0x902520A4, "MU50 xr174c0 v1.05"},  {0x507168AD, "MU50 v1.04"},
    {0x6FE5BDF5, "MU50 xq332e0 v1.02"},  {0x6FC85B41, "MU90 xs519d0 v1.01"},  {0x66FE5896, "MU90B xt040c0 v1.01"},
    {0x1126A8A4, "MU100 xu50720 v1.11"}, {0xF4BA61F1, "MU128 xv217c0 v1.06"}, {0x079BFCF0, "MU128 xv224c0 v1.06"},
    {0xAFBAC33C, "MU2000 xw86920 v1.01"}, {0x79F6C158, "MU2000 xw87020 v1.01"}, {0xE4046AEF, "MU15 xv684c0 v1.01"},
}};

const char* model_name(MuRomModel m);

/// The two halves of a joined program ROM file (first, second as stored).
std::pair<Bytes, Bytes> split_program(std::span<const std::uint8_t> b, MuProgramForm form);

/// Convert one set from its program ROM halves and its wave ROMs (bus order; their CRCs were
/// checked). Its internal checks throw ConvertError (HVYXG_THROW: abort without exceptions).
MuTables convert_set(const SetSpec& spec, std::span<const std::uint8_t> high, std::span<const std::uint8_t> low,
                     std::span<const std::span<const std::uint8_t>> waves, const EngineTables& tables);

} // namespace hvyxg::mu_rom_sets
