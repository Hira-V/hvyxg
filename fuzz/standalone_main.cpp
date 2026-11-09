// Copyright (C) 2026 Kevin H. Patterson
// SPDX-License-Identifier: BSD-3-Clause (see LICENSE)
// Driver for compilers without libFuzzer (Apple clang, GCC): replays every file given (or
// every file in the directories given), then, with -runs=N, mutates corpus entries at random
// for N more runs (bit flips, byte writes, inserts, erases, splices; -seed=S, -max_len=L).
// A crash leaves the input in ./crash-standalone (written before each mutated run).
#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <random>
#include <string>
#include <vector>

extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t* data, std::size_t size);

namespace {

std::vector<std::uint8_t> read(const std::filesystem::path& p) {
    std::ifstream in(p, std::ios::binary);
    return {std::istreambuf_iterator<char>(in), {}};
}

void mutate(std::vector<std::uint8_t>& d, const std::vector<std::vector<std::uint8_t>>& corpus, std::mt19937_64& rng,
            std::size_t max_len) {
    const int n = 1 + static_cast<int>(rng() % 8);
    for (int k = 0; k < n; ++k) {
        const std::size_t pos = d.empty() ? 0 : rng() % d.size();
        switch (rng() % 7) {
        case 0:
            if (!d.empty()) d[pos] ^= static_cast<std::uint8_t>(1u << (rng() % 8));
            break;
        case 1:
            if (!d.empty()) d[pos] = static_cast<std::uint8_t>(rng());
            break;
        case 2: {
            static constexpr std::uint8_t kMagic[] = {0x00, 0x01, 0x7F, 0x80, 0xFF, 0xF0, 0xF7, 0x40};
            if (!d.empty()) d[pos] = kMagic[rng() % sizeof kMagic];
            break;
        }
        case 3:
            if (d.size() < max_len) d.insert(d.begin() + static_cast<std::ptrdiff_t>(pos), static_cast<std::uint8_t>(rng()));
            break;
        case 4:
            if (!d.empty()) {
                const std::size_t len = std::min<std::size_t>(d.size() - pos, 1 + rng() % 16);
                d.erase(d.begin() + static_cast<std::ptrdiff_t>(pos), d.begin() + static_cast<std::ptrdiff_t>(pos + len));
            }
            break;
        case 5:
            if (d.size() >= 4) {  // overwrite a little-endian u32 with an interesting value
                static constexpr std::uint32_t kVals[] = {0, 1, 0x7FFFFFFF, 0x80000000, 0xFFFFFFFF, 0xFFFF, 0x10000};
                const std::uint32_t v = kVals[rng() % 7];
                const std::size_t at = rng() % (d.size() - 3);
                for (int i = 0; i < 4; ++i) d[at + static_cast<std::size_t>(i)] = static_cast<std::uint8_t>(v >> (8 * i));
            }
            break;
        case 6:
            if (!corpus.empty()) {  // splice
                const auto& o = corpus[rng() % corpus.size()];
                if (o.empty()) break;
                const std::size_t from = rng() % o.size();
                const std::size_t len = std::min<std::size_t>(o.size() - from, 1 + rng() % 64);
                if (d.size() + len <= max_len)
                    d.insert(d.begin() + static_cast<std::ptrdiff_t>(pos), o.begin() + static_cast<std::ptrdiff_t>(from),
                             o.begin() + static_cast<std::ptrdiff_t>(from + len));
            }
            break;
        }
    }
}

} // namespace

int main(int argc, char** argv) {
    std::uint64_t runs = 0, seed = 1;
    std::size_t max_len = 1 << 20;
    std::vector<std::vector<std::uint8_t>> corpus;
    for (int i = 1; i < argc; ++i) {
        const std::string a = argv[i];
        if (a.rfind("-runs=", 0) == 0) runs = std::stoull(a.substr(6));
        else if (a.rfind("-seed=", 0) == 0) seed = std::stoull(a.substr(6));
        else if (a.rfind("-max_len=", 0) == 0) max_len = std::stoull(a.substr(9));
        else if (a[0] == '-') continue;  // other libFuzzer flags
        else if (std::filesystem::is_directory(a)) {
            std::vector<std::filesystem::path> files;
            for (const auto& e : std::filesystem::directory_iterator(a))
                if (e.is_regular_file()) files.push_back(e.path());
            std::sort(files.begin(), files.end());
            for (const auto& f : files) corpus.push_back(read(f));
        } else {
            corpus.push_back(read(a));
        }
    }
    for (const auto& c : corpus) LLVMFuzzerTestOneInput(c.data(), c.size());
    std::printf("replayed %zu inputs\n", corpus.size());
    if (corpus.empty()) corpus.emplace_back();
    std::mt19937_64 rng(seed);
    for (std::uint64_t r = 0; r < runs; ++r) {
        std::vector<std::uint8_t> d = corpus[rng() % corpus.size()];
        mutate(d, corpus, rng, max_len);
        {
            std::ofstream out("crash-standalone", std::ios::binary | std::ios::trunc);
            out.write(reinterpret_cast<const char*>(d.data()), static_cast<std::streamsize>(d.size()));
        }
        LLVMFuzzerTestOneInput(d.data(), d.size());
        if (rng() % 64 == 0 && corpus.size() < 4096) corpus.push_back(std::move(d));
    }
    if (runs) {
        std::filesystem::remove("crash-standalone");
        std::printf("%llu mutated runs, no crash\n", static_cast<unsigned long long>(runs));
    }
    return 0;
}
