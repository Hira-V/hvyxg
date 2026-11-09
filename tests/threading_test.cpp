// Copyright (C) 2026 Kevin H. Patterson
// SPDX-License-Identifier: BSD-3-Clause (see LICENSE)
// The threading contract (include/hvyxg/synth.hpp): a loaded Bank is immutable and may serve
// synths on several threads at once; each Synth is used by one thread. Several synths on one
// bank, each on its own thread, render exactly what one synth renders alone. (Also run under
// ThreadSanitizer: -DHVYXG_SANITIZE=thread.)
#include <doctest/doctest.h>

#include <cstdint>
#include <thread>
#include <vector>

#include "fuzz_support.hpp"
#include "test_support.hpp"

namespace {

std::vector<float> perform(const std::shared_ptr<const hvyxg::Bank>& bank, int variant) {
    hvyxg::Synth::Options o;
    o.tables = test::tables();
    o.bank = bank;
    hvyxg::Synth s(o);
    std::vector<float> out(2 * 44100), buf(2 * 441);
    for (std::size_t at = 0, step = 0; at < 44100; at += 441, ++step) {
        const auto ch = static_cast<std::uint8_t>(step % 16);
        const std::uint8_t pc[] = {static_cast<std::uint8_t>(0xC0 | ch), static_cast<std::uint8_t>((step * 5 + variant) & 0x7F)};
        const std::uint8_t on[] = {static_cast<std::uint8_t>(0x90 | ch), static_cast<std::uint8_t>(36 + step % 48), 100};
        s.midi(pc, 7);
        s.midi(on, 9);
        s.render(buf.data(), 441);
        std::copy(buf.begin(), buf.end(), out.begin() + static_cast<std::ptrdiff_t>(2 * at));
    }
    return out;
}

void check_shared(const std::shared_ptr<const hvyxg::Bank>& bank) {
    constexpr int kThreads = 4;
    std::vector<std::vector<float>> alone(kThreads), together(kThreads);
    for (int t = 0; t < kThreads; ++t) alone[static_cast<std::size_t>(t)] = perform(bank, t);
    std::vector<std::thread> threads;
    for (int t = 0; t < kThreads; ++t)
        threads.emplace_back([&, t] { together[static_cast<std::size_t>(t)] = perform(bank, t); });
    for (auto& th : threads) th.join();
    for (int t = 0; t < kThreads; ++t) CHECK(together[static_cast<std::size_t>(t)] == alone[static_cast<std::size_t>(t)]);
}

} // namespace

TEST_CASE("synths on one bank on several threads render as alone") {
    HVYXG_NEED_TABLES();
    for (int which = 0; which < 3; ++which) {
        CAPTURE(which);
        check_shared(fuzz::bank(which));
    }
    for (const bool syxg50 : {true, false})
        if (const auto bank = test::bank(syxg50)) check_shared(bank);
}
