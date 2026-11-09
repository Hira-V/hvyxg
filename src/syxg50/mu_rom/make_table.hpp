// Copyright (C) 2026 Kevin H. Patterson
// SPDX-License-Identifier: BSD-3-Clause (see LICENSE)
// Translated from SXG-Create-NF, Copyright (c) 2026 Soundshock, BSD-3-Clause (THIRD_PARTY.md)
#pragma once
// The S-YXG50 table writer of SXG-Create-NF (utility/makeSYXG50.py, tableconvert.py and
// drumvel.py; BSD-3-Clause, Copyright 2026 Soundshock): the converter's object model → the
// big-layout .TBL (with the MAP2 voice-map appendix) and the .UPCM wave file.

#include <span>
#include <string>

#include "syxg50/dll_tables.hpp"
#include "syxg50/mu_rom/model.hpp"

namespace hvyxg::syxg50::mu_rom {

struct Tables {
    Bytes tbl;
    Bytes upcm;
};

/// makeSYXG50.MakeSYXG50 (big layout) + SampleMonster.Generate. `alt` is the second voice map
/// (MU Basic); the native map is the default.
Tables make_syxg50(const dll::Tables& tb, Table& table, Table& alt, std::span<const std::uint8_t> waverom,
                   const std::string& version, const std::string& wave_name);

} // namespace hvyxg::syxg50::mu_rom
