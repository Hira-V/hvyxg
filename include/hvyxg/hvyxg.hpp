// Copyright (C) 2026 Kevin H. Patterson
// SPDX-License-Identifier: BSD-3-Clause (see LICENSE)
#pragma once
// hvyxg: a software synthesizer that plays the Yamaha S-YXG50 and S-YXG2006LE banks (and
// SXG-Create-NF's MU banks) the way the original DLLs do. The whole public API:
//
//   hvyxg/types.hpp          models, options, Error / BankError
//   hvyxg/engine_tables.hpp  the engine tables, read from your DLLs into memory you own
//   hvyxg/bank.hpp           banks, from files or over memory you own
//   hvyxg/mu_rom.hpp         MU ROM sets converted to MU banks
//   hvyxg/synth.hpp          the synthesizer: MIDI / UMP in, audio out
//   hvyxg/plugin_tables.hpp  banks embedded in a plugin DLL (hosted builds)
//   hvyxg/version.hpp        the version
//
// Built with HVYXG_NO_FILES=1 (embedded hosts: no exceptions, no files) the declarations that
// read files or throw are left out. Reading MIDI files and writing WAV files is the renderer's
// (tools/render_yxg.cpp, tools/render/), not the library's.

#include "hvyxg/bank.hpp"
#include "hvyxg/engine_tables.hpp"
#include "hvyxg/mu_rom.hpp"
#include "hvyxg/plugin_tables.hpp"
#include "hvyxg/synth.hpp"
#include "hvyxg/types.hpp"
#include "hvyxg/version.hpp"
