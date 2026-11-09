// Copyright (C) 2026 Kevin H. Patterson
// SPDX-License-Identifier: BSD-3-Clause (see LICENSE)
#pragma once
// The engine tables object (public side: hvyxg/engine_tables.hpp). The caller owns its memory;
// init_engine_tables() constructs it, load_engine_tables() fills it from DLL images, and from
// then on it is read only: any number of synths, banks and threads may share it.

#include <array>
#include <cstdint>
#include <type_traits>

#include "fx/fx_tables.hpp"
#include "hvyxg/engine_tables.hpp"
#include "syxg2006le/tbl/dll_tables.hpp"
#include "syxg50/dll_tables.hpp"

namespace hvyxg {

struct EngineTables {
    static constexpr std::uint32_t kMagic = 0x54475848u;  // "HXGT"
    /// Most descriptors (dll/tables.cpp checks the generated lists against it).
    static constexpr std::size_t kMaxDescs = 256;

    std::uint32_t magic;
    /// Per descriptor, in the order of fx_tables(), le_voice_tables(), syxg50_voice_tables():
    /// whether it filled its table.
    std::array<std::uint8_t, kMaxDescs> found;
    fx::Tables fx;
    syxg50::dll::Tables syxg50;
    syxg2006le::tbl::dll::Tables le;
};

static_assert(std::is_standard_layout_v<EngineTables> && std::is_trivially_copyable_v<EngineTables>);

} // namespace hvyxg
