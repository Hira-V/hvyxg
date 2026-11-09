// Copyright (C) 2026 Kevin H. Patterson
// SPDX-License-Identifier: BSD-3-Clause (see LICENSE)
#include "bank_search.hpp"

#include <algorithm>
#include <cctype>
#include <fstream>
#include <optional>
#include <string>

#include "bank_io.hpp"
#include "hvyxg/mu_rom.hpp"

#define XXH_INLINE_ALL
#include "xxhash/xxhash.h"

namespace hvyxg::bank_search {

namespace fs = std::filesystem;

namespace {

struct Hash {
    std::uint64_t high, low;
    bool operator==(const Hash&) const = default;
};

Hash hash(std::span<const std::uint8_t> b) {
    const XXH128_hash_t h = XXH3_128bits(b.data(), b.size());
    return {h.high64, h.low64};
}

/// The stock banks, by the XXH3-128 of their files (no data).
struct Stock {
    const char* bin_name;  ///< as the stock installs and the 5 MB S-YXG50 name it
    const char* label;
    Hash bin, wave;
    Model model;
};
constexpr Stock kStock[] = {
    {"SXGBIN41.TBL", "S-YXG50 4 MB", {0x98e8d1b3d05d279dull, 0x29a228da47f04e47ull},
     {0x87163ea297895d9bull, 0x01f32e5514298e17ull}, Model::Syxg50},
    {"SXGBIN21.TBL", "S-YXG50 2 MB", {0xf7c23e6e87e22169ull, 0x32aa1befbec27becull},
     {0x68dc2b3c5c4660c0ull, 0xd05e5c23e7858f9cull}, Model::Syxg50},
    {"SXGBNW6L.TBL", "S-YXG2006LE", {0x4d6f8d9e9f7ffd7cull, 0x41057f8dd0a13683ull},
     {0x60113cd6e9fb136aull, 0xa25b31164308a2f6ull}, Model::Syxg2006le},
};

std::string upper(std::string s) {
    for (char& c : s) c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
    return s;
}

enum class Kind { Stock, Table, Roms };

struct Candidate {
    Kind kind;
    std::string label;
    fs::path bin, wave;
    Model model;
    int rank = 0;  ///< within a kind: lower first (the 4 MB stock bank before the 2 MB one)
    Hash bin_hash{};
};

constexpr std::uintmax_t kMaxBinTable = 2u << 20;  // the voice tables are 0.1 … 0.7 MB

/// The file in `dir` named `name` (any case), if there is one.
std::optional<fs::path> sibling(const fs::path& dir, const std::string& name) {
    std::error_code ec;
    for (const auto& e : fs::directory_iterator(dir, ec))
        if (e.is_regular_file(ec) && upper(e.path().filename().string()) == upper(name)) return e.path();
    return std::nullopt;
}

void add_tables(const fs::path& file, std::vector<Candidate>& out) {
    std::error_code ec;
    const auto size = fs::file_size(file, ec);
    if (ec || size < 32 || size > kMaxBinTable) return;
    std::vector<std::uint8_t> bin;
    try {
        bin = read_file(file, "bank");
    } catch (const BankError&) {
        return;
    }
    const std::optional<Model> m = sniff_model(bin);
    if (!m) return;
    const Hash h = hash(bin);
    Candidate c{Kind::Table, {}, file, {}, *m, 0, h};
    for (std::size_t i = 0; i < std::size(kStock); ++i)
        if (kStock[i].bin == h) {
            c.kind = Kind::Stock;
            c.label = kStock[i].label;
            c.rank = static_cast<int>(i);
        }
    if (*m == Model::Syxg50) {
        if (bin[0x1F] & 0x80) {
            c.wave.clear();  // the wave is inside the bin table
        } else {
            std::string name;
            for (std::size_t i = 0x10; i < 0x1F && bin[i] != 0; ++i) name.push_back(static_cast<char>(bin[i]));
            const auto w = sibling(file.parent_path(), name);
            if (!w) return;
            c.wave = *w;
        }
        if (c.kind == Kind::Table) {
            std::string text;
            for (std::size_t i = 0; i < 0x10 && bin[i] >= 0x20 && bin[i] < 0x7F; ++i) text.push_back(static_cast<char>(bin[i]));
            while (!text.empty() && text.back() == ' ') text.pop_back();
            c.label = text.empty() ? file.filename().string() : text;
        }
    } else {
        const auto w = sibling(file.parent_path(), "SXGDAT6L.TBL");
        if (!w) return;
        c.wave = *w;
        if (c.kind == Kind::Table) c.label = "S-YXG2006LE (" + file.filename().string() + ")";
    }
    if (std::none_of(out.begin(), out.end(), [&](const Candidate& o) { return o.bin_hash == h; })) out.push_back(c);
}

bool matches_mu_model(const Candidate& c, MuRomModel m) {
    if (m == MuRomModel::Auto || c.kind == Kind::Roms) return true;
    const char* name = m == MuRomModel::Mu1000 ? "MU1000" : m == MuRomModel::Mu2000 ? "MU2000" : "MU128";
    return c.label.find(name) != std::string::npos;
}

} // namespace

std::shared_ptr<const Bank> load(const Request& r) {
    std::vector<Candidate> found;
    for (const fs::path& p : r.paths) {
        std::error_code ec;
        if (fs::is_regular_file(p, ec)) {
            add_tables(p, found);
            continue;
        }
        for (auto it = fs::recursive_directory_iterator(p, ec); !ec && it != fs::recursive_directory_iterator();
             it.increment(ec))
            if (it->is_regular_file(ec)) add_tables(it->path(), found);
    }
    // MU ROM sets (found by CRC-32); none is not an error here.
    std::optional<MuRomSet> roms;
    if (r.model != Model::Syxg2006le) {
        try {
            roms = find_mu_roms(MuRomOptions{.roms = r.paths, .model = r.mu_model, .cache_dir = {}});
            found.push_back({Kind::Roms, roms->name + " ROMs", {}, {}, Model::Syxg50, 0, {}});
        } catch (const BankError&) {
        }
    }

    // Asking for an MU model asks for an MU bank.
    const bool want_mu = r.enable_mu || r.mu_model != MuRomModel::Auto;
    std::vector<Candidate> c;
    for (const Candidate& x : found) {
        if (r.model != Model::Auto && x.model != r.model) continue;
        const bool mu = x.kind != Kind::Stock;
        if (want_mu && !mu) continue;
        if (mu && !matches_mu_model(x, r.mu_model)) continue;
        c.push_back(x);
    }
    // Without enable_mu the stock banks come first.
    if (!want_mu && std::any_of(c.begin(), c.end(), [](const Candidate& x) { return x.kind == Kind::Stock; }))
        c.erase(std::remove_if(c.begin(), c.end(), [](const Candidate& x) { return x.kind != Kind::Stock; }), c.end());
    std::string where;
    for (const fs::path& p : r.paths) where += (where.empty() ? "" : ", ") + p.string();
    if (c.empty()) throw BankError(BankError::Kind::Io, "no bank found under " + where);
    // Within one model and kind the default is the first by rank; a choice between models or
    // kinds is the caller's.
    std::stable_sort(c.begin(), c.end(), [](const Candidate& a, const Candidate& b) { return a.rank < b.rank; });
    const auto same = [&](const Candidate& x) { return x.kind == c.front().kind && x.model == c.front().model; };
    if (!std::all_of(c.begin(), c.end(), same) || (c.front().kind == Kind::Table && c.size() > 1)) {
        std::string list;
        for (const Candidate& x : c) list += "\n  " + x.label + (x.bin.empty() ? "" : " (" + x.bin.string() + ")");
        throw BankError(BankError::Kind::Io, "several banks found under " + where + ":" + list +
                                                 "\nchoose with --model syxg50|2006le, --enable-mu, --mu-model, or --bin / --data");
    }
    const Candidate& pick = c.front();
    BankOptions bo = r.bank_options;
    if (pick.kind != Kind::Stock && pick.model == Model::Syxg50) bo.enable_mu = true;
    if (pick.kind == Kind::Roms)
        return load_mu_roms(MuRomOptions{.roms = r.paths, .model = r.mu_model, .cache_dir = r.mu_cache}, bo);
    if (pick.wave.empty()) return load_bank(read_file(pick.bin, "bin table"), {}, pick.model, bo);
    return load_bank(pick.bin, pick.wave, pick.model, bo);
}

void check_embedded(const PluginTables& t) {
    for (const Stock& s : kStock)
        if (upper(t.bin_name) == s.bin_name) {
            if (hash(t.bin) != s.bin || (!t.wave.empty() && hash(t.wave) != s.wave))
                throw BankError(BankError::Kind::Table, std::string("the embedded ") + s.bin_name + " is not the stock " +
                                                            s.label + " bank");
            return;
        }
}

} // namespace hvyxg::bank_search
