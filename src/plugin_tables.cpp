// Copyright (C) 2026 Kevin H. Patterson
// SPDX-License-Identifier: BSD-3-Clause (see LICENSE)
#include "hvyxg/plugin_tables.hpp"

#include <algorithm>
#include <cctype>
#include <optional>

#include "bank_impl.hpp"
#include "bank_io.hpp"
#include "hvyxg/types.hpp"
#include "pe_image.hpp"

namespace hvyxg {

namespace {

using pe::kRtRcdata;
using pe::kRtString;
using pe::Pe;
using pe::bad;

std::string upper(std::string s) {
    for (char& c : s) c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
    return s;
}

} // namespace

std::vector<PeResource> pe_rcdata(std::span<const std::uint8_t> bytes) {
    const Pe pe(bytes);
    std::vector<PeResource> out;
    for (const Pe::Entry& e : pe.type(kRtRcdata)) {
        const auto d = pe.leaf(e);
        out.push_back({e.named ? e.name : "#" + std::to_string(e.id), {d.begin(), d.end()}});
    }
    return out;
}

std::vector<std::string> pe_strings(std::span<const std::uint8_t> bytes) {
    const Pe pe(bytes);
    std::vector<std::string> out;
    // Each RT_STRING entry is a block of 16 counted UTF-16 strings.
    for (const Pe::Entry& e : pe.type(kRtString)) {
        const auto d = pe.leaf(e);
        std::size_t o = 0;
        for (int k = 0; k < 16 && o + 2 <= d.size(); ++k) {
            const std::size_t n = static_cast<std::size_t>(d[o] | d[o + 1] << 8);
            o += 2;
            if (o + 2 * n > d.size()) break;
            std::string s;
            for (std::size_t i = 0; i < n; ++i) {
                const int c = d[o + 2 * i] | d[o + 2 * i + 1] << 8;
                s.push_back(c < 0x80 ? static_cast<char>(c) : '?');
            }
            o += 2 * n;
            if (!s.empty()) out.push_back(std::move(s));
        }
    }
    return out;
}

PluginTables read_plugin_tables(std::span<const std::uint8_t> dll) {
    std::vector<PeResource> res = pe_rcdata(dll);
    std::vector<std::size_t> bins;
    for (std::size_t i = 0; i < res.size(); ++i)
        if (sniff_model(res[i].data)) bins.push_back(i);
    if (bins.empty()) bad("no bank table among its RCDATA resources");
    std::size_t pick = bins.front();
    if (bins.size() > 1) {
        // Several tables (the 5 MB S-YXG50 carries the 4 MB and the 2 MB one): the default
        // table named in the DLL's string table.
        const std::vector<std::string> strings = pe_strings(dll);
        for (const std::size_t i : bins)
            if (std::any_of(strings.begin(), strings.end(),
                            [&](const std::string& s) { return upper(s) == upper(res[i].name); })) {
                pick = i;
                break;
            }
    }
    PluginTables t;
    t.bin_name = res[pick].name;
    t.bin = std::move(res[pick].data);
    const bool syxg50 = sniff_model(t.bin) == Model::Syxg50;
    if (syxg50 && t.bin.size() > 0x1F && (t.bin[0x1F] & 0x80)) return t;  // the wave is inside
    std::optional<std::size_t> wave;
    if (syxg50) {
        // The S-YXG50 header names its wave file at 0x10 (15 characters, NUL-padded).
        std::string name;
        for (std::size_t i = 0x10; i < 0x1F && t.bin[i] != 0; ++i) name.push_back(static_cast<char>(t.bin[i]));
        for (std::size_t i = 0; i < res.size(); ++i)
            if (i != pick && upper(res[i].name) == upper(name)) wave = i;
        if (!wave) bad("the wave table " + name + " named by " + t.bin_name + " is not among its resources");
    } else {
        for (std::size_t i = 0; i < res.size(); ++i)
            if (i != pick && !sniff_model(res[i].data)) {
                if (wave) bad("more than one candidate wave table");
                wave = i;
            }
        if (!wave) bad("no wave table among its RCDATA resources");
    }
    t.wave_name = res[*wave].name;
    t.wave = std::move(res[*wave].data);
    return t;
}

PluginTables read_plugin_tables(const std::filesystem::path& dll) {
    const std::vector<std::uint8_t> bytes = read_file(dll, "plugin DLL");
    return read_plugin_tables(bytes);
}

} // namespace hvyxg
