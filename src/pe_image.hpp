// Copyright (C) 2026 Kevin H. Patterson
// SPDX-License-Identifier: BSD-3-Clause (see LICENSE)
#pragma once
// A small, bounds-checked PE / COFF reader: sections, the resource tree, the data sections.
// Used to take embedded banks out of a plugin DLL (plugin_tables.cpp) and to find the engine
// tables in a DLL's data (dll/tables.cpp). Nothing in the DLL is executed.

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <vector>

#include "hvyxg/types.hpp"

namespace hvyxg::pe {

// PE / COFF layout (Microsoft PE format specification): the DOS header's e_lfanew at 0x3c,
// "PE\0\0", the file header (20 bytes), the optional header (magic 0x10b PE32 / 0x20b PE32+,
// resource table = data directory 2), the section table; the resource tree is type → name →
// language, each level an IMAGE_RESOURCE_DIRECTORY (16 bytes) followed by 8-byte entries.
inline constexpr std::uint32_t kRtString = 6;
inline constexpr std::uint32_t kRtRcdata = 10;
inline constexpr std::uint32_t kHighBit = 0x80000000u;

/// The raw bytes of the sections of PE image `b` that hold initialized data: not code, not the
/// resource section (where embedded banks live). This is where a DLL keeps its tables. False
/// for a malformed image. Never throws (the core uses it).
inline bool data_sections(std::span<const std::uint8_t> b, std::vector<std::span<const std::uint8_t>>& out) {
    out.clear();
    const auto u16 = [&](std::size_t o) { return static_cast<std::uint32_t>(b[o] | b[o + 1] << 8); };
    const auto u32 = [&](std::size_t o) {
        return static_cast<std::uint32_t>(b[o] | b[o + 1] << 8 | b[o + 2] << 16) | static_cast<std::uint32_t>(b[o + 3]) << 24;
    };
    if (b.size() < 0x40 || b[0] != 'M' || b[1] != 'Z') return false;
    const std::size_t pe = u32(0x3c);
    if (pe + 24 > b.size() || u32(pe) != 0x00004550) return false;
    const std::size_t nsec = u16(pe + 6), opt_size = u16(pe + 20), opt = pe + 24;
    if (opt + opt_size > b.size() || opt_size < 2) return false;
    const std::uint32_t magic = u16(opt);
    const std::size_t dirs = magic == 0x10b ? opt + 96 : magic == 0x20b ? opt + 112 : 0;
    if (dirs == 0) return false;
    std::uint32_t rsrc_rva = 0, rsrc_size = 0;
    if (dirs + 3 * 8 <= opt + opt_size) {
        rsrc_rva = u32(dirs + 2 * 8);
        rsrc_size = u32(dirs + 2 * 8 + 4);
    }
    const std::size_t sec = opt + opt_size;
    if (sec + 40 * nsec > b.size()) return false;
    constexpr std::uint32_t kCode = 0x20, kInitData = 0x40;
    for (std::size_t i = 0; i < nsec; ++i) {
        const std::size_t s = sec + 40 * i;
        const std::uint32_t va = u32(s + 12), vsize = std::max(u32(s + 8), u32(s + 16)), raw = u32(s + 20),
                            raw_size = u32(s + 16), flags = u32(s + 36);
        if ((flags & kCode) || !(flags & kInitData)) continue;
        if (rsrc_rva != 0 && rsrc_size != 0 && rsrc_rva >= va && rsrc_rva - va < vsize) continue;
        if (raw >= b.size()) continue;
        out.push_back(b.subspan(raw, std::min<std::size_t>(raw_size, b.size() - raw)));
    }
    return true;
}

#if !HVYXG_NO_FILES

[[noreturn]] inline void bad(const std::string& what) {
    throw BankError(BankError::Kind::Header, "plugin DLL: " + what);
}

class Pe {
public:
    explicit Pe(std::span<const std::uint8_t> b) : b_(b) {
        if (b_.size() < 0x40 || b_[0] != 'M' || b_[1] != 'Z') bad("not a PE file (no MZ header)");
        const std::size_t pe = u32(0x3c);
        if (pe + 24 > b_.size() || u32(pe) != 0x00004550) bad("not a PE file (no PE signature)");
        const std::size_t nsec = u16(pe + 6), opt_size = u16(pe + 20), opt = pe + 24;
        if (opt + opt_size > b_.size() || opt_size < 2) bad("truncated optional header");
        const std::uint16_t magic = u16(opt);
        const std::size_t dirs = magic == 0x10b ? opt + 96 : magic == 0x20b ? opt + 112 : 0;
        if (dirs == 0) bad("unknown optional header");
        if (dirs + 3 * 8 <= opt + opt_size) {
            rsrc_rva_ = u32(dirs + 2 * 8);
            rsrc_size_ = u32(dirs + 2 * 8 + 4);
        }
        const std::size_t sec = opt + opt_size;
        if (sec + 40 * nsec > b_.size()) bad("truncated section table");
        for (std::size_t i = 0; i < nsec; ++i) {
            const std::size_t s = sec + 40 * i;
            sections_.push_back({u32(s + 12), std::max(u32(s + 8), u32(s + 16)), u32(s + 20), u32(s + 16), u32(s + 36)});
        }
    }

    bool has_resources() const { return rsrc_rva_ != 0 && rsrc_size_ != 0; }

    /// The raw bytes of the sections that hold initialized data: not code, not the resource
    /// section (where embedded banks live). This is where a DLL keeps its tables.
    std::vector<std::span<const std::uint8_t>> data_sections() const {
        std::vector<std::span<const std::uint8_t>> out;
        pe::data_sections(b_, out);
        return out;
    }

    /// File offset of `size` bytes at `rva` (bad() when they are not in the file).
    std::size_t at(std::uint32_t rva, std::size_t size) const {
        for (const Section& s : sections_) {
            if (rva < s.va || rva - s.va >= s.vsize) continue;
            const std::size_t off = rva - s.va;
            if (off + size > s.raw_size || std::size_t{s.raw} + off + size > b_.size()) break;
            return s.raw + off;
        }
        bad("resource data outside the file");
    }

    /// An entry of the resource tree: its id (or name) and its target.
    struct Entry {
        std::uint32_t id;      // numbered entries
        std::string name;      // named entries
        bool named;
        std::uint32_t target;  // offset from the resource root (high bit: a subdirectory)
    };

    std::vector<Entry> dir(std::uint32_t off) const {
        const std::size_t d = at(rsrc_rva_ + off, 16);
        const std::size_t n = std::size_t{u16(d + 12)} + u16(d + 14);
        const std::size_t e0 = at(rsrc_rva_ + off + 16, 8 * n);
        std::vector<Entry> out;
        for (std::size_t i = 0; i < n; ++i) {
            const std::uint32_t name = u32(e0 + 8 * i), target = u32(e0 + 8 * i + 4);
            Entry e{name, {}, (name & kHighBit) != 0, target};
            if (e.named) e.name = utf16(rsrc_rva_ + (name & ~kHighBit));
            out.push_back(std::move(e));
        }
        return out;
    }

    /// The data of a leaf (the first language under `e` when `e` is a subdirectory).
    std::span<const std::uint8_t> leaf(const Entry& e, int depth = 0) const {
        if (e.target & kHighBit) {
            if (depth > 2) bad("resource tree too deep");
            const auto sub = dir(e.target & ~kHighBit);
            if (sub.empty()) return {};
            return leaf(sub.front(), depth + 1);
        }
        const std::size_t d = at(rsrc_rva_ + e.target, 16);
        const std::uint32_t rva = u32(d), size = u32(d + 4);
        return b_.subspan(at(rva, size), size);
    }

    std::vector<Entry> type(std::uint32_t id) const {
        if (!has_resources()) return {};
        for (const Entry& t : dir(0))
            if (!t.named && t.id == id && (t.target & kHighBit)) return dir(t.target & ~kHighBit);
        return {};
    }

    std::uint16_t u16(std::size_t o) const {
        if (o + 2 > b_.size()) bad("truncated");
        return static_cast<std::uint16_t>(b_[o] | b_[o + 1] << 8);
    }
    std::uint32_t u32(std::size_t o) const {
        if (o + 4 > b_.size()) bad("truncated");
        return static_cast<std::uint32_t>(b_[o] | b_[o + 1] << 8 | b_[o + 2] << 16) |
               static_cast<std::uint32_t>(b_[o + 3]) << 24;
    }

    /// A counted UTF-16 string at `rva` (non-ASCII characters become '?').
    std::string utf16(std::uint32_t rva) const {
        const std::size_t n = u16(at(rva, 2));
        const std::size_t o = at(rva + 2, 2 * n);
        std::string s;
        for (std::size_t i = 0; i < n; ++i) {
            const std::uint16_t c = u16(o + 2 * i);
            s.push_back(c < 0x80 ? static_cast<char>(c) : '?');
        }
        return s;
    }

private:
    struct Section {
        std::uint32_t va, vsize, raw, raw_size, flags;
    };
    std::span<const std::uint8_t> b_;
    std::vector<Section> sections_;
    std::uint32_t rsrc_rva_ = 0, rsrc_size_ = 0;
};

#endif  // !HVYXG_NO_FILES

/// True when `b` starts like a PE file (MZ header and a PE signature where e_lfanew points).
inline bool looks_like_pe(std::span<const std::uint8_t> b) {
    if (b.size() < 0x40 || b[0] != 'M' || b[1] != 'Z') return false;
    const std::size_t pe = static_cast<std::size_t>(b[0x3c] | b[0x3d] << 8 | b[0x3e] << 16) |
                           static_cast<std::size_t>(b[0x3f]) << 24;
    return pe + 4 <= b.size() && b[pe] == 'P' && b[pe + 1] == 'E' && b[pe + 2] == 0 && b[pe + 3] == 0;
}

} // namespace hvyxg::pe
