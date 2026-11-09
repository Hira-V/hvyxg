// Copyright (C) 2026 Kevin H. Patterson
// SPDX-License-Identifier: BSD-3-Clause (see LICENSE)
#pragma once
// Bank tables embedded in a plugin DLL: SXG-Create-NF `--embed` stores the converted table and
// wave file in its patched syxg50.dll as RT_RCDATA resources named after the files, as the
// 5 MB S-YXG50 does with its own SXGBIN41.TBL / SXGWAVE4.TBL. `read_plugin_tables` is a small
// PE resource reader that takes them out again, so a user can point hvyxg at such a plugin
// (`Synth::Options::plugin_file`, `render_yxg --plugin`). Nothing in the DLL is executed.

#if !HVYXG_NO_FILES  // hosted builds only

#include <cstdint>
#include <filesystem>
#include <span>
#include <string>
#include <vector>

namespace hvyxg {

/// One RT_RCDATA resource of a PE file.
struct PeResource {
    std::string name;  ///< the resource name (ASCII), or "#<id>" for a numbered one
    std::vector<std::uint8_t> data;
};

/// Every RT_RCDATA resource (first language of each), in directory order. Throws `BankError`
/// (Header) when the bytes are not a PE image or its resource directory is malformed.
std::vector<PeResource> pe_rcdata(std::span<const std::uint8_t> pe);

/// The strings of the RT_STRING table, in id order (empty entries skipped).
std::vector<std::string> pe_strings(std::span<const std::uint8_t> pe);

struct PluginTables {
    std::vector<std::uint8_t> bin;   ///< the bin (voice) table
    std::vector<std::uint8_t> wave;  ///< the wave table (empty when the bin embeds its wave)
    std::string bin_name;            ///< their resource names
    std::string wave_name;
};

/// The bank tables a plugin DLL carries. The bin table is the RT_RCDATA resource with an
/// S-YXG50 / S-YXG2006LE header (when there are several, the one the DLL's string table names
/// as its default table); the wave table is the resource named by the bin table's header.
/// Throws `BankError` when the DLL has no such tables.
PluginTables read_plugin_tables(std::span<const std::uint8_t> dll);
PluginTables read_plugin_tables(const std::filesystem::path& dll);

} // namespace hvyxg

#endif  // !HVYXG_NO_FILES
