// Copyright (C) 2026 Kevin H. Patterson
// SPDX-License-Identifier: BSD-3-Clause (see LICENSE)
// Banks over the caller's memory (hvyxg::BankMemory): the same sound as a bank that owns its
// files, one bank shared by several synths, and the 2006LE wave buffer's alignment.
#include <doctest/doctest.h>

#include <cstring>
#include <vector>

#include "hvyxg/bank.hpp"
#include "bank_io.hpp"
#include "test_support.hpp"

using namespace hvyxg;

namespace {

/// A file's bytes and BankMemory's spare bytes, starting `offset` bytes into the buffer.
struct Buffer {
    std::vector<std::uint8_t> bytes;
    std::size_t offset = 0;
    Buffer(const std::string& file, std::size_t at) : offset(at) {
        const std::vector<std::uint8_t> f = read_file(file, "bank");
        bytes.assign(at + f.size() + BankMemory::kPad, 0);
        std::memcpy(bytes.data() + at, f.data(), f.size());
    }
    std::span<std::uint8_t> span() { return std::span(bytes).subspan(offset); }
};

std::vector<float> play(std::shared_ptr<const Bank> bank) {
    Synth::Options o;
    o.bank = std::move(bank);
    Synth s(o);
    std::vector<float> out(2 * 44100);
    for (std::uint8_t ch = 0; ch < 4; ++ch) {
        const std::uint8_t on[] = {static_cast<std::uint8_t>(0xC0 | ch), static_cast<std::uint8_t>(ch * 20)};
        s.midi(on);
        const std::uint8_t note[] = {static_cast<std::uint8_t>(0x90 | ch), static_cast<std::uint8_t>(48 + 7 * ch), 100};
        s.midi(note);
    }
    s.render(out.data(), out.size() / 2);
    return out;
}

} // namespace

TEST_CASE("bank memory: a bank over the caller's memory plays as one that owns its files") {
    HVYXG_NEED_TABLES();
    for (const bool syxg50 : {true, false}) {
        const auto f = test::bank_files(syxg50);
        if (!f) {
            MESSAGE("bank files not found: skipped");
            continue;
        }
        Buffer bin(f->bin, 0), wave(f->data, 0);
        const auto in_place = load_bank(BankMemory{bin.span(), wave.span()}, Model::Auto, test::bank_options());
        const auto owning = load_bank(f->bin, f->data, Model::Auto, test::bank_options());
        const std::vector<float> a = play(in_place);
        CHECK(a == play(owning));
        // One bank, several synths.
        CHECK(a == play(in_place));
    }
}

TEST_CASE("bank memory: the S-YXG2006LE wave buffer must be 2-byte aligned") {
    HVYXG_NEED_TABLES();
    const auto f = test::bank_files(false);
    if (!f) {
        MESSAGE("S-YXG2006LE bank files not found: skipped");
        return;
    }
    Buffer bin(f->bin, 0), wave(f->data, 1);
    CHECK_THROWS_AS(load_bank(BankMemory{bin.span(), wave.span()}, Model::Auto, test::bank_options()), BankError);
}

TEST_CASE("bank memory: buffers without the padding are refused") {
    std::vector<std::uint8_t> tiny(4, 0);
    CHECK_THROWS_AS(load_bank(BankMemory{tiny, tiny}, Model::Syxg50, test::bank_options()), BankError);
    CHECK_THROWS_AS(load_bank(BankMemory{tiny, tiny}, Model::Syxg2006le, test::bank_options()), BankError);
}
