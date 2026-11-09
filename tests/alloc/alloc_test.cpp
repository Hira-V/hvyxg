// Copyright (C) 2026 Kevin H. Patterson
// SPDX-License-Identifier: BSD-3-Clause (see LICENSE)
// Allocation check (own binary: it replaces the global operator new / delete).
// `Synth::render` must not allocate, from the first call on, also when it applies queued
// messages (notes, steals, programs, controllers, effect and insertion type changes, resets,
// SysEx) at block boundaries; `midi`, `midi_stream` and `reset` must not allocate within the
// queue capacity (synth.hpp). Runs on the synthetic banks of fuzz/ (always) and on the real
// banks when found.
#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>

#include <algorithm>
#include <atomic>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <new>
#include <span>
#include <string>
#include <vector>

#include "fuzz_support.hpp"
#include "test_support.hpp"

// GCC takes the free() in the replacement operator delete for a mismatched deallocation.
#if defined(__GNUC__) && !defined(__clang__)
#pragma GCC diagnostic ignored "-Wmismatched-new-delete"
#endif

namespace {

std::atomic<bool> g_armed{false};
std::atomic<std::uint64_t> g_count{0};

// Debugging: HVYXG_ALLOC_TRACE=N prints the first N counted allocations' call stacks.
#if defined(__APPLE__) || defined(__GLIBC__)
#include <execinfo.h>
void trace() {
    static const long limit = [] {
        const char* e = std::getenv("HVYXG_ALLOC_TRACE");
        return e ? std::atol(e) : 0L;
    }();
    static long printed = 0;
    if (printed >= limit) return;
    ++printed;
    g_armed = false;
    void* frames[24];
    const int n = backtrace(frames, 24);
    std::fprintf(stderr, "---- allocation %ld\n", printed);
    backtrace_symbols_fd(frames, n, 2);
    g_armed = true;
}
#else
void trace() {}
#endif

void* counted(std::size_t n, std::size_t align = 0) {
    if (g_armed.load(std::memory_order_relaxed)) {
        g_count.fetch_add(1, std::memory_order_relaxed);
        trace();
    }
    if (n == 0) n = 1;
    void* p = nullptr;
    if (align > alignof(std::max_align_t)) {
        if (posix_memalign(&p, align, n) != 0) p = nullptr;
    } else {
        p = std::malloc(n);
    }
    return p;
}

} // namespace

void* operator new(std::size_t n) {
    if (void* p = counted(n)) return p;
    throw std::bad_alloc();
}
void* operator new[](std::size_t n) {
    if (void* p = counted(n)) return p;
    throw std::bad_alloc();
}
void* operator new(std::size_t n, std::align_val_t a) {
    if (void* p = counted(n, static_cast<std::size_t>(a))) return p;
    throw std::bad_alloc();
}
void* operator new[](std::size_t n, std::align_val_t a) {
    if (void* p = counted(n, static_cast<std::size_t>(a))) return p;
    throw std::bad_alloc();
}
void* operator new(std::size_t n, const std::nothrow_t&) noexcept { return counted(n); }
void* operator new[](std::size_t n, const std::nothrow_t&) noexcept { return counted(n); }
void operator delete(void* p) noexcept { std::free(p); }
void operator delete[](void* p) noexcept { std::free(p); }
void operator delete(void* p, std::size_t) noexcept { std::free(p); }
void operator delete[](void* p, std::size_t) noexcept { std::free(p); }
void operator delete(void* p, std::align_val_t) noexcept { std::free(p); }
void operator delete[](void* p, std::align_val_t) noexcept { std::free(p); }
void operator delete(void* p, std::size_t, std::align_val_t) noexcept { std::free(p); }
void operator delete[](void* p, std::size_t, std::align_val_t) noexcept { std::free(p); }

namespace {

using Msg = std::vector<std::uint8_t>;

Msg xg(std::initializer_list<std::uint8_t> addr_data) {
    Msg m{0xF0, 0x43, 0x10, 0x4C};
    m.insert(m.end(), addr_data);
    m.push_back(0xF7);
    return m;
}

/// A dense, effect-heavy sequence: every part sounding, controllers, the variation and all
/// insertion blocks with changing types, drums, a GS reset and back.
std::vector<Msg> workload(int round) {
    std::vector<Msg> v;
    const auto r = static_cast<std::uint8_t>(round);
    v.push_back(xg({0x02, 0x01, 0x40, static_cast<std::uint8_t>(0x05 + (r % 3) * 8), 0x00}));  // variation type
    v.push_back(xg({0x02, 0x01, 0x5A, 0x01}));                                                  // variation: system
    v.push_back(xg({0x02, 0x01, 0x00, static_cast<std::uint8_t>(0x01 + r % 4), 0x00}));        // reverb type
    v.push_back(xg({0x02, 0x01, 0x20, static_cast<std::uint8_t>(0x41 + r % 3), 0x00}));        // chorus type
    for (std::uint8_t b = 0; b < 4; ++b) {                                                      // XG2.0 insertion blocks
        v.push_back(xg({0x03, b, 0x00, static_cast<std::uint8_t>(0x40 + 8 * ((r + b) % 6)), 0x00}));
        v.push_back(xg({0x03, b, 0x0C, b}));  // part
    }
    for (std::uint8_t ch = 0; ch < 16; ++ch) {
        v.push_back({static_cast<std::uint8_t>(0xB0 | ch), 0, static_cast<std::uint8_t>(ch == 9 ? 127 : 0)});
        v.push_back({static_cast<std::uint8_t>(0xC0 | ch), static_cast<std::uint8_t>((ch * 7 + r) & 0x7F)});
        v.push_back({static_cast<std::uint8_t>(0xB0 | ch), 91, 100});
        v.push_back({static_cast<std::uint8_t>(0xB0 | ch), 93, 80});
        v.push_back({static_cast<std::uint8_t>(0xB0 | ch), 94, 90});
        v.push_back({static_cast<std::uint8_t>(0xB0 | ch), 65, 127});
        v.push_back({static_cast<std::uint8_t>(0xB0 | ch), 5, 20});
        for (std::uint8_t k = 0; k < 10; ++k)
            v.push_back({static_cast<std::uint8_t>(0x90 | ch), static_cast<std::uint8_t>(30 + 6 * k + ch), 100});
    }
    v.push_back({0xE3, 0x00, 0x50});
    v.push_back({0xB4, 1, 127});
    v.push_back({0xD5, 90});
    v.push_back({0xA6, 60, 90});
    for (std::uint8_t ch = 0; ch < 16; ch += 2)
        for (std::uint8_t k = 0; k < 10; k += 2)
            v.push_back({static_cast<std::uint8_t>(0x80 | ch), static_cast<std::uint8_t>(30 + 6 * k + ch), 0});
    if (round % 4 == 3) {
        v.push_back({0xF0, 0x41, 0x10, 0x42, 0x12, 0x40, 0x00, 0x7F, 0x00, 0x41, 0xF7});  // GS reset
        v.push_back({0xF0, 0x7E, 0x7F, 0x09, 0x01, 0xF7});                               // GM on
        v.push_back(xg({0x00, 0x00, 0x7E, 0x00}));                                        // XG on
    }
    return v;
}

/// Allocations inside `render` while it applies `round`s of the workload (queued with frame
/// offsets, so they are applied inside render), twice over.
std::uint64_t render_allocations(std::shared_ptr<const hvyxg::Bank> bank, std::uint16_t polyphony, float steal_fade) {
    hvyxg::Synth::Options o;
    o.tables = test::tables();
    o.bank = std::move(bank);
    o.max_polyphony = polyphony;
    o.steal_fade_ms = steal_fade;
    hvyxg::Synth s(o);
    std::vector<float> buf(2 * 4096);
    const auto run = [&](bool armed) {
        std::uint64_t n = 0;
        for (int round = 0; round < 8; ++round) {
            const auto msgs = workload(round);
            std::uint32_t at = 1;
            for (const Msg& m : msgs) s.midi(m, at += 37);
            g_count = 0;
            g_armed = armed;
            for (int i = 0; i < 6; ++i) s.render(buf.data(), 4096);
            s.render(buf.data(), 1);
            s.render(buf.data(), 300);
            g_armed = false;
            n += g_count;
        }
        return n;
    };
    // The first pass counts too: nothing in the engines is allocated lazily. (The message
    // queue that `midi` fills grows outside `render`.)
    return run(true) + run(true);
}

/// Allocations inside `midi` (applied at once and queued with frame offsets, within the
/// documented queue capacity), `midi_stream` and `reset`, from a fresh synth on.
std::uint64_t control_allocations(std::shared_ptr<const hvyxg::Bank> bank) {
    hvyxg::Synth::Options o;
    o.tables = test::tables();
    o.bank = std::move(bank);
    o.ports = 4;
    hvyxg::Synth s(o);
    std::vector<float> buf(2 * 4096);
    std::vector<std::uint8_t> stream;
    for (int round = 0; round < 2; ++round)  // ≈ 660 messages, all pending at once
        for (const Msg& m : workload(round)) stream.insert(stream.end(), m.begin(), m.end());
    std::uint64_t n = 0;
    for (int round = 0; round < 8; ++round) {
        const auto msgs = workload(round);
        g_count = 0;
        g_armed = true;
        for (const Msg& m : msgs) s.midi(m, 0, static_cast<std::size_t>(round) % 4);  // at once
        std::uint32_t at = 0;
        for (const Msg& m : msgs) s.midi(m, at += 11);  // queued (≈ 330 messages)
        g_armed = false;
        n += g_count;
        s.render(buf.data(), 4096);
        g_count = 0;
        g_armed = true;
        // Live input in pieces: running status, SysEx split across calls, real-time bytes.
        for (std::size_t i = 0; i < stream.size(); i += 7) {
            const std::size_t len = std::min<std::size_t>(7, stream.size() - i);
            s.midi_stream(std::span(stream).subspan(i, len), static_cast<std::uint32_t>(i % 512), 1);
            const std::uint8_t clock = 0xF8;
            s.midi_stream(std::span(&clock, 1));
        }
        if (round % 3 == 2) s.reset();
        g_armed = false;
        n += g_count;
        s.render(buf.data(), 4096);
    }
    return n;
}

} // namespace

TEST_CASE("midi, midi_stream and reset do not allocate within the queue capacity") {
    HVYXG_NEED_TABLES();
    for (int which = 0; which < 3; ++which) {
        CAPTURE(which);
        CHECK(control_allocations(fuzz::bank(which)) == 0);
    }
    for (const bool syxg50 : {true, false}) {
        const auto bank = test::bank(syxg50);
        if (!bank) continue;
        CAPTURE(syxg50);
        CHECK(control_allocations(bank) == 0);
    }
}

TEST_CASE("render does not allocate (synthetic banks)") {
    HVYXG_NEED_TABLES();
    for (int which = 0; which < 3; ++which) {
        CAPTURE(which);
        const auto bank = fuzz::bank(which);
        CHECK(render_allocations(bank, 0, 0.0f) == 0);
        CHECK(render_allocations(bank, 256, 25.0f) == 0);
    }
}

TEST_CASE("render does not allocate (real banks)") {
    HVYXG_NEED_TABLES();
    for (const bool syxg50 : {true, false}) {
        const auto bank = test::bank(syxg50);
        if (!bank) continue;
        CAPTURE(syxg50);
        CHECK(render_allocations(bank, 0, 0.0f) == 0);
        CHECK(render_allocations(bank, 256, 25.0f) == 0);
    }
}

TEST_CASE("a queue that never drains does not grow") {
    HVYXG_NEED_TABLES();
    // A live host that always has a later message queued: before the queue was compacted,
    // its applied messages were kept until it ran empty, so it grew (and allocated) forever.
    hvyxg::Synth::Options o;
    o.tables = test::tables();
    o.bank = fuzz::bank(0);
    hvyxg::Synth s(o);
    std::vector<float> buf(2 * 256);
    const auto block = static_cast<std::uint32_t>(s.block_size());
    g_count = 0;
    g_armed = true;
    for (int i = 0; i < 20000; ++i) {
        const auto key = static_cast<std::uint8_t>(40 + i % 40);
        const std::uint8_t on[] = {0x90, key, 100}, off[] = {0x80, key, 0};
        s.midi(on, 3 * block);  // three blocks ahead: always pending
        s.midi(off, 5 * block);
        s.render(buf.data(), block);
    }
    g_armed = false;
    CHECK(g_count == 0);
}
