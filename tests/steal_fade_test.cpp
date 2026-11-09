// Copyright (C) 2026 Kevin H. Patterson
// SPDX-License-Identifier: BSD-3-Clause (see LICENSE)
// Superset steal fade (Synth::Options::steal_fade_ms): a voice stolen for a new note fades out
// linearly over the chosen time instead of the DLLs' fast steal release; 0 keeps the DLLs'.

#include <doctest/doctest.h>

#include <algorithm>
#include <cmath>
#include <string>
#include <vector>

#include "hvyxg/synth.hpp"
#include "test_support.hpp"

namespace {

constexpr std::size_t kHalf = 22050;

/// 100 sustained organ notes on part 1, then 100 more on part 2 (each steals a voice), as
/// interleaved stereo. Sends 0 so only the voices sound.
std::vector<float> steal_burst(bool syxg50, float fade_ms) {
    hvyxg::Synth::Options o;
    o.tables = test::tables();
    o.bank = test::bank(syxg50);
    o.max_polyphony = 100;
    o.steal_fade_ms = fade_ms;
    hvyxg::Synth s(o);
    const auto send = [&](std::initializer_list<std::uint8_t> m) { s.midi(std::vector<std::uint8_t>(m)); };
    send({0xF0, 0x43, 0x10, 0x4C, 0x00, 0x00, 0x7E, 0x00, 0xF7});
    for (std::uint8_t ch = 0; ch < 2; ++ch) {
        send({static_cast<std::uint8_t>(0xC0 | ch), 16});
        for (std::uint8_t cc : {91, 93, 94}) send({static_cast<std::uint8_t>(0xB0 | ch), cc, 0});
        send({static_cast<std::uint8_t>(0xB0 | ch), 7, 20});
    }
    std::vector<float> out(4 * kHalf);
    for (std::uint8_t k = 20; k < 120; ++k) send({0x90, k, 100});
    s.render(out.data(), kHalf);
    for (std::uint8_t k = 20; k < 120; ++k) send({0x91, k, 100});
    s.render(out.data() + 2 * kHalf, kHalf);
    return out;
}

/// RMS of a − b over frames [f0, f1).
double diff_rms(const std::vector<float>& a, const std::vector<float>& b, std::size_t f0, std::size_t f1) {
    double e = 0;
    for (std::size_t i = 2 * f0; i < 2 * f1; ++i) e += std::pow(static_cast<double>(a[i]) - b[i], 2);
    return std::sqrt(e / static_cast<double>(2 * (f1 - f0)));
}

void check_engine(bool syxg50) {
    const auto dll = steal_burst(syxg50, 0.0f);
    const auto fade = steal_burst(syxg50, 20.0f);
    // Identical until the steals.
    CHECK(std::equal(dll.begin(), dll.begin() + 2 * kHalf, fade.begin()));
    // During the fade the old chord is still audible on top of the new one.
    const double during = diff_rms(dll, fade, kHalf + 220, kHalf + 660);  // 5 … 15 ms
    MESSAGE(std::string(syxg50 ? "S-YXG50" : "2006LE") << " stolen-sound rms 5-15 ms after the burst: " << during);
    CHECK(during > 1e-4);
    // 20 ms later every faded voice has ended: the output is the DLL-mode output again.
    const std::size_t after = kHalf + 1024 + 128;  // fade end + one block
    CHECK(std::equal(dll.begin() + 2 * after, dll.end(), fade.begin() + 2 * after));
}

} // namespace

TEST_CASE("steal_fade_2006le") {
    HVYXG_NEED_TABLES();
    if (!test::bank(false)) return;
    check_engine(false);
}

TEST_CASE("steal_fade_syxg50") {
    HVYXG_NEED_TABLES();
    if (!test::bank(true)) return;
    check_engine(true);
}
