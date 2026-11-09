// Copyright (C) 2026 Kevin H. Patterson
// SPDX-License-Identifier: BSD-3-Clause (see LICENSE)
#pragma once
// The engine tables, read at runtime from your own DLLs. hvyxg ships no table data: each table
// is found in a DLL image by its hashes, wherever it sits, so any DLL that holds it will do (the
// stock plugins, the VEG release's 5 MB S-YXG50 (its bank in the DLL), SXG-Create-NF's patched
// plugins).
//
// The caller owns the object:
//
//   void* mem = aligned_alloc(hvyxg::kEngineTablesAlign, hvyxg::engine_tables_size());
//   hvyxg::EngineTables* t = hvyxg::init_engine_tables(mem);
//   hvyxg::load_engine_tables(*t, dll_bytes);   // once per DLL image; the image may go after
//   if (!hvyxg::engine_tables_ready(*t, hvyxg::Model::Syxg50)) ...
//
// Once filled it is read only: one object serves any number of banks and synths, on any
// threads. It holds no pointers and needs no destruction (free the memory when every user is
// gone). The functions here neither throw nor touch files.

#include <cstddef>
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

struct EngineTables;

inline constexpr std::size_t kEngineTablesAlign = 16;

/// Bytes of an EngineTables object.
std::size_t engine_tables_size() noexcept;

/// Construct an empty EngineTables in `memory` (engine_tables_size() bytes, aligned to
/// kEngineTablesAlign).
EngineTables* init_engine_tables(void* memory) noexcept;

/// Fill every table not found yet from one DLL image (the file's bytes). Anything that is not a
/// PE image is ignored. Returns the number of tables filled by this image.
std::size_t load_engine_tables(EngineTables& tables, std::span<const std::uint8_t> dll_image) noexcept;

/// Whether every table `model`'s engine needs is there.
bool engine_tables_ready(const EngineTables& tables, Model model) noexcept;

/// The tables `model`'s engine needs that are missing: `each(name, user)` for each, in a stable
/// order. Returns how many.
std::size_t engine_tables_missing(const EngineTables& tables, Model model,
                                  void (*each)(const char* name, void* user) = nullptr,
                                  void* user = nullptr) noexcept;

/// False and `error` naming the missing tables and the DLL that supplies them, unless every
/// table `model`'s engine needs is there.
bool check_engine_tables(const EngineTables& tables, Model model, Error& error);

#if !HVYXG_NO_FILES
// ---- Files and exceptions (hosted builds) ----------------------------------------------------

/// A new EngineTables filled from the DLL files (files that are not PE images are skipped).
std::shared_ptr<EngineTables> load_engine_tables(const std::vector<std::filesystem::path>& dlls);

/// Every PE file under `dir` (recursively), in a stable order.
std::vector<std::filesystem::path> find_dlls(const std::filesystem::path& dir);

/// Throw a BankError naming the missing tables and the DLLs that supply them, unless ready.
void require_engine_tables(const EngineTables& tables, Model model);
#endif

} // namespace hvyxg
