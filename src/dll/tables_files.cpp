// Copyright (C) 2026 Kevin H. Patterson
// SPDX-License-Identifier: BSD-3-Clause (see LICENSE)
// The engine tables from DLL files, and the throwing check (hosted builds), over tables.cpp.
#include <algorithm>
#include <fstream>
#include <iterator>

#include "engine_tables.hpp"
#include "hvyxg/engine_tables.hpp"

namespace hvyxg {

namespace {

std::vector<std::uint8_t> read_all(const std::filesystem::path& p) {
    std::ifstream f(p, std::ios::binary);
    if (!f) return {};
    return {std::istreambuf_iterator<char>(f), std::istreambuf_iterator<char>()};
}

} // namespace

std::shared_ptr<EngineTables> load_engine_tables(const std::vector<std::filesystem::path>& dlls) {
    auto t = std::make_shared<EngineTables>();
    init_engine_tables(t.get());
    for (const std::filesystem::path& p : dlls) {
        if (engine_tables_ready(*t, Model::Syxg50) && engine_tables_ready(*t, Model::Syxg2006le)) break;
        const std::vector<std::uint8_t> bytes = read_all(p);
        load_engine_tables(*t, bytes);
    }
    return t;
}

std::vector<std::filesystem::path> find_dlls(const std::filesystem::path& dir) {
    std::vector<std::filesystem::path> out;
    std::error_code ec;
    for (auto it = std::filesystem::recursive_directory_iterator(dir, ec);
         !ec && it != std::filesystem::recursive_directory_iterator(); it.increment(ec)) {
        if (!it->is_regular_file(ec)) continue;
        std::ifstream f(it->path(), std::ios::binary);
        std::uint8_t head[2] = {};
        if (f.read(reinterpret_cast<char*>(head), 2) && head[0] == 'M' && head[1] == 'Z') out.push_back(it->path());
    }
    std::sort(out.begin(), out.end());
    return out;
}

void require_engine_tables(const EngineTables& tables, Model model) {
    Error e;
    if (!check_engine_tables(tables, model, e)) throw BankError(e);
}

} // namespace hvyxg
