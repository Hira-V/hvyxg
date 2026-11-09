// Copyright (C) 2026 Kevin H. Patterson
// SPDX-License-Identifier: BSD-3-Clause (see LICENSE)
#include "dll/tables.hpp"

#include <algorithm>
#include <cstring>
#include <map>
#include <array>
#include <string>
#include <vector>

#include "engine_tables.hpp"
#include "hvyxg/types.hpp"

#include "pe_image.hpp"

#define XXH_INLINE_ALL
#include "xxhash/xxhash.h"

namespace hvyxg::table_store {

namespace {

constexpr std::uint64_t kBase = 0x100000001b3ull;

std::size_t elem_size(Elem e) {
    switch (e) {
    case Elem::U8: case Elem::S8: return 1;
    case Elem::U16: case Elem::S16: return 2;
    default: return 4;
    }
}

/// Element i of `bytes` (little-endian), as a double (every element type fits exactly).
double read_elem(const std::uint8_t* p, Elem e) {
    const std::uint32_t u = static_cast<std::uint32_t>(p[0]) | (elem_size(e) > 1 ? static_cast<std::uint32_t>(p[1]) << 8 : 0u) |
                            (elem_size(e) > 2 ? static_cast<std::uint32_t>(p[2]) << 16 | static_cast<std::uint32_t>(p[3]) << 24 : 0u);
    switch (e) {
    case Elem::U8: return static_cast<double>(static_cast<std::uint8_t>(u));
    case Elem::S8: return static_cast<double>(static_cast<std::int8_t>(u));
    case Elem::U16: return static_cast<double>(static_cast<std::uint16_t>(u));
    case Elem::S16: return static_cast<double>(static_cast<std::int16_t>(u));
    case Elem::U32: return static_cast<double>(u);
    case Elem::S32: return static_cast<double>(static_cast<std::int32_t>(u));
    case Elem::F32: {
        float f;
        std::memcpy(&f, &u, sizeof f);
        return static_cast<double>(f);
    }
    }
    return 0.0;
}

template <class T>
bool put(void* dest, std::size_t i, double v) {
    if constexpr (std::is_floating_point_v<T>) {
        static_cast<T*>(dest)[i] = static_cast<T>(v);
        return true;
    } else {
        if (v != static_cast<double>(static_cast<T>(v))) return false;  // out of range or fractional
        static_cast<T*>(dest)[i] = static_cast<T>(v);
        return true;
    }
}

bool store(const TableDesc& d, const std::uint8_t* src, void* dest) {
    const std::size_t es = elem_size(d.in_dll);
    for (std::size_t i = 0; i < d.count; ++i) {
        const double v = read_elem(src + i * es, d.in_dll);
        bool ok = false;
        switch (d.stored) {
        case Elem::U8: ok = put<std::uint8_t>(dest, i, v); break;
        case Elem::S8: ok = put<std::int8_t>(dest, i, v); break;
        case Elem::U16: ok = put<std::uint16_t>(dest, i, v); break;
        case Elem::S16: ok = put<std::int16_t>(dest, i, v); break;
        case Elem::U32: ok = put<std::uint32_t>(dest, i, v); break;
        case Elem::S32: ok = put<std::int32_t>(dest, i, v); break;
        case Elem::F32: ok = put<float>(dest, i, v); break;
        }
        if (!ok) return false;
    }
    return true;
}

/// Every descriptor, in EngineTables::found's order.
struct Descs {
    std::array<const TableDesc*, EngineTables::kMaxDescs> all{};
    std::size_t n = 0;
    constexpr Descs() = default;
};

Descs descs() {
    Descs d;
    for (const std::span<const TableDesc> list : {fx_tables(), le_voice_tables(), syxg50_voice_tables()})
        for (const TableDesc& x : list) d.all[d.n++] = &x;
    return d;
}

std::ptrdiff_t offset_of(const TableDesc& d, EngineTables& t) {
    return static_cast<std::uint8_t*>(d.dest(t)) - reinterpret_cast<std::uint8_t*>(&t);
}

/// Whether the table descriptor `i` fills is there (by it or an alternative).
bool is_found(const Descs& ds, std::size_t i, const EngineTables& t) {
    auto& m = const_cast<EngineTables&>(t);  // for dest(); nothing is written
    const std::ptrdiff_t off = offset_of(*ds.all[i], m);
    for (std::size_t j = 0; j < ds.n; ++j)
        if (t.found[j] && offset_of(*ds.all[j], m) == off) return true;
    return false;
}

/// Fill every table not found yet from one image's data sections.
std::size_t scan(std::span<const std::uint8_t> image, EngineTables& t) {
    if (!pe::looks_like_pe(image)) return 0;
    std::vector<std::span<const std::uint8_t>> sections;
    if (!pe::data_sections(image, sections)) return 0;  // a malformed PE: nothing to take from it
    const Descs ds = descs();
    std::size_t filled = 0;
    // Group the wanted tables by byte length: one rolling pass per length.
    std::map<std::size_t, std::vector<std::size_t>> by_len;
    for (std::size_t i = 0; i < ds.n; ++i)
        if (!is_found(ds, i, t)) by_len[std::size_t{ds.all[i]->count} * elem_size(ds.all[i]->in_dll)].push_back(i);
    for (auto& [len, wanted] : by_len) {
        std::uint64_t top = 1;  // B^(len−1)
        for (std::size_t i = 1; i < len; ++i) top *= kBase;
        for (const auto sec : sections) {
            if (sec.size() < len || wanted.empty()) continue;
            std::uint64_t h = rolling_hash(sec.first(len));
            for (std::size_t at = 0;; ++at) {
                for (auto it = wanted.begin(); it != wanted.end();) {
                    const TableDesc& d = *ds.all[*it];
                    if (h == d.roll && at % elem_size(d.in_dll) == 0 && !is_found(ds, *it, t)) {
                        const XXH128_hash_t x = XXH3_128bits(sec.data() + at, len);
                        if (x.high64 == d.xxh_high && x.low64 == d.xxh_low && store(d, sec.data() + at, d.dest(t))) {
                            if (d.fix) d.fix(d.dest(t));
                            t.found[*it] = 1;
                            ++filled;
                            it = wanted.erase(it);
                            continue;
                        }
                    }
                    ++it;
                }
                if (wanted.empty() || at + len >= sec.size()) break;
                h = (h - sec[at] * top) * kBase + sec[at + len];
            }
        }
    }
    return filled;
}


} // namespace

std::uint64_t rolling_hash(std::span<const std::uint8_t> bytes) {
    std::uint64_t h = 0;
    for (const std::uint8_t b : bytes) h = h * kBase + b;
    return h;
}

} // namespace hvyxg::table_store

namespace hvyxg {

using namespace table_store;

std::size_t engine_tables_size() noexcept { return sizeof(EngineTables); }

EngineTables* init_engine_tables(void* memory) noexcept {
    auto* t = static_cast<EngineTables*>(std::memset(memory, 0, sizeof(EngineTables)));
    t->magic = EngineTables::kMagic;
    return t;
}

std::size_t load_engine_tables(EngineTables& tables, std::span<const std::uint8_t> dll_image) noexcept {
    return scan(dll_image, tables);
}

std::size_t engine_tables_missing(const EngineTables& tables, Model model, void (*each)(const char*, void*),
                                  void* user) noexcept {
    const unsigned need = model == Model::Syxg2006le ? kSyxg2006le : kSyxg50;
    const Descs ds = descs();
    auto& m = const_cast<EngineTables&>(tables);
    std::size_t n = 0;
    for (std::size_t i = 0; i < ds.n; ++i) {
        if (!(ds.all[i]->need & need) || is_found(ds, i, tables)) continue;
        // once per table: skip alternatives of an earlier descriptor
        bool earlier = false;
        for (std::size_t j = 0; j < i && !earlier; ++j)
            earlier = (ds.all[j]->need & need) && offset_of(*ds.all[j], m) == offset_of(*ds.all[i], m);
        if (earlier) continue;
        ++n;
        if (each) each(ds.all[i]->name, user);
    }
    return n;
}

bool engine_tables_ready(const EngineTables& tables, Model model) noexcept {
    return engine_tables_missing(tables, model) == 0;
}

bool check_engine_tables(const EngineTables& tables, Model model, Error& error) {
    std::vector<std::string> m;
    engine_tables_missing(tables, model,
                          [](const char* name, void* u) { static_cast<std::vector<std::string>*>(u)->emplace_back(name); }, &m);
    if (m.empty()) return true;
    std::string list;
    for (std::size_t i = 0; i < m.size() && i < 4; ++i) list += (i ? ", " : "") + m[i];
    if (m.size() > 4) list += ", …";
    error = {ErrorKind::Header,
             std::to_string(m.size()) + " engine tables not found (" + list + "): give the " +
                 (model == Model::Syxg2006le ? "S-YXG2006LE DLL (S-YXG2006LE.DLL / SGP2.DLL)"
                                             : "S-YXG50 DLL (syxg50.dll or an SXG-Create-NF plugin)") +
                 " with --plugin or --plugin-path"};
    return false;
}

} // namespace hvyxg
