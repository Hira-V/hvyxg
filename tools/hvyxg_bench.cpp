// Copyright (C) 2026 Kevin H. Patterson
// SPDX-License-Identifier: BSD-3-Clause (see LICENSE)
// CPU cost of hvyxg's rendering (docs/PERFORMANCE.md). Times are the rendering thread's CPU
// time (CLOCK_THREAD_CPUTIME_ID), less sensitive to other load than wall time; each figure
// is the best of --repeat runs. "load" = CPU time / audio time (100 % = one core in real time).
//
// usage: hvyxg-bench [--plugin DLL]... [--plugin-path DIR] [--bin F --data F] [--enable-mu] [--rate HZ]
//                    [--seconds S] [--repeat R]
//                    [--interp linear|hermite|lanczos] MODE [ARGS]
//   voices [N…]       sustained voices (organ, effects off): load per voice count
//   effects           each reverb / chorus / variation (system, insertion) / XG2.0 insertion
//                     type over 16 sustained voices, as extra load
//   worst [N]         N voices (default: the bank's polyphony), every effect on, all four
//                     insertion blocks
//   song FILE…        whole songs as render_yxg plays them: load, worst 128-frame block
#include <time.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <exception>
#include <memory>
#include <string>
#include <vector>

#include "hvyxg/bank.hpp"
#include "hvyxg/engine_tables.hpp"
#include "render/render_host.hpp"  // the renderers' song reader (Hira-V's midifile, copied)
#include "hvyxg/synth.hpp"

namespace {

using Msg = std::vector<std::uint8_t>;

double cpu_now() {
    timespec t{};
    clock_gettime(CLOCK_THREAD_CPUTIME_ID, &t);
    return static_cast<double>(t.tv_sec) + 1e-9 * static_cast<double>(t.tv_nsec);
}

struct Config {
    hvyxg::Synth::Options o;
    double seconds = 5.0;
    int repeat = 3;
};

Msg xg(std::initializer_list<std::uint8_t> addr_data) {
    Msg m{0xF0, 0x43, 0x10, 0x4C};
    m.insert(m.end(), addr_data);
    m.push_back(0xF7);
    return m;
}

void send(hvyxg::Synth& s, const Msg& m) { s.midi(m); }

/// Effects off: reverb / chorus / variation types "no effect", sends 0.
void effects_off(hvyxg::Synth& s) {
    send(s, xg({0x02, 0x01, 0x00, 0x00, 0x00}));
    send(s, xg({0x02, 0x01, 0x20, 0x00, 0x00}));
    send(s, xg({0x02, 0x01, 0x40, 0x00, 0x00}));
    for (std::uint8_t ch = 0; ch < 16; ++ch) {
        send(s, {static_cast<std::uint8_t>(0xB0 | ch), 91, 0});
        send(s, {static_cast<std::uint8_t>(0xB0 | ch), 93, 0});
        send(s, {static_cast<std::uint8_t>(0xB0 | ch), 94, 0});
    }
}

/// Start sustained notes (organ, program 16) over the melodic channels until `voices` sound
/// or the polyphony is full. Returns the voice count.
std::size_t hold_voices(hvyxg::Synth& s, std::size_t voices) {
    std::vector<float> buf(2 * 128);
    for (std::uint8_t ch = 0; ch < 16; ++ch) send(s, {static_cast<std::uint8_t>(0xC0 | ch), 16});
    std::size_t sent = 0;
    for (int key = 24; key < 120 && s.voice_count() < voices; ++key)
        for (std::uint8_t ch = 0; ch < 16 && s.voice_count() < voices; ++ch) {
            if (ch == 9) continue;
            send(s, {static_cast<std::uint8_t>(0x90 | ch), static_cast<std::uint8_t>(key), 100});
            ++sent;
            s.render(buf.data(), 1);  // let the note-on take effect so voice_count sees it
        }
    (void)sent;
    return s.voice_count();
}

/// Best-of-`repeat` load of rendering `seconds` from the synth's current state (each repeat
/// renders on; the state is steady for sustained voices).
double measure(hvyxg::Synth& s, const Config& c) {
    std::vector<float> buf(2 * 512);
    const auto frames = static_cast<std::size_t>(c.seconds * s.sample_rate());
    double best = 1e30;
    for (int r = 0; r < c.repeat; ++r) {
        const double t0 = cpu_now();
        for (std::size_t done = 0; done < frames; done += 512) s.render(buf.data(), 512);
        best = std::min(best, (cpu_now() - t0) / c.seconds);
    }
    return best;
}

std::unique_ptr<hvyxg::Synth> make(const Config& c, std::uint16_t polyphony) {
    hvyxg::Synth::Options o = c.o;
    o.max_polyphony = polyphony;
    auto s = std::make_unique<hvyxg::Synth>(o);
    std::vector<float> buf(2 * 128);
    s->render(buf.data(), 128);
    return s;
}

int voices_mode(Config& c, std::vector<std::size_t> counts) {
    if (counts.empty()) counts = {0, 1, 8, 16, 32, 64, 128, 256, 512, 1024};
    std::printf("%8s %8s %10s %14s\n", "voices", "sounding", "load %", "µs/voice/s");
    double base = 0.0;
    for (const std::size_t n : counts) {
        auto s = make(c, static_cast<std::uint16_t>(std::max<std::size_t>(n, 1)));
        effects_off(*s);
        const std::size_t got = n ? hold_voices(*s, n) : 0;
        const double load = measure(*s, c);
        if (n == 0) base = load;
        std::printf("%8zu %8zu %10.2f %14.2f\n", n, got, 100 * load, got ? 1e6 * (load - base) / static_cast<double>(got) : 0.0);
        std::fflush(stdout);
    }
    return 0;
}

struct NamedType {
    std::uint8_t msb, lsb;
    const char* name;
};

int effects_mode(Config& c) {
    // XG effect types (MSB, LSB) from the XG specification; the engines map unknown ones to
    // their nearest or to off.
    const std::vector<NamedType> reverbs = {{0x01, 0, "Hall 1"}, {0x02, 0, "Room 1"}, {0x03, 0, "Stage 1"}, {0x04, 0, "Plate"},
                                            {0x10, 0, "White Room"}, {0x11, 0, "Tunnel"}, {0x13, 0, "Basement"}};
    const std::vector<NamedType> choruses = {{0x41, 0, "Chorus 1"}, {0x42, 0, "Celeste 1"}, {0x43, 0, "Flanger 1"},
                                             {0x44, 0, "Symphonic"}, {0x48, 0, "Phaser 1"}};
    const std::vector<NamedType> variations = {
        {0x01, 0, "Hall 1"},       {0x02, 0, "Room 1"},      {0x03, 0, "Stage 1"},      {0x04, 0, "Plate"},
        {0x05, 0, "Delay L,C,R"},  {0x06, 0, "Delay L,R"},   {0x07, 0, "Echo"},         {0x08, 0, "Cross Delay"},
        {0x09, 0, "Early Ref 1"},  {0x0A, 0, "Gate Reverb"}, {0x0B, 0, "Reverse Gate"}, {0x14, 0, "Karaoke 1"},
        {0x41, 0, "Chorus 1"},     {0x42, 0, "Celeste 1"},   {0x43, 0, "Flanger 1"},    {0x44, 0, "Symphonic"},
        {0x45, 0, "Rotary Spk"},   {0x46, 0, "Tremolo"},     {0x47, 0, "Auto Pan"},     {0x48, 0, "Phaser 1"},
        {0x49, 0, "Distortion"},   {0x4A, 0, "Overdrive"},   {0x4B, 0, "Amp Sim"},      {0x4C, 0, "3-Band EQ"},
        {0x4D, 0, "2-Band EQ"},    {0x4E, 0, "Auto Wah"},
    };
    std::printf("%-10s %-14s %10s %10s\n", "block", "type", "load %", "extra %");
    const auto run = [&](const std::vector<Msg>& setup) {
        auto s = make(c, 64);
        effects_off(*s);
        for (const Msg& m : setup) send(*s, m);
        hold_voices(*s, 16);
        return measure(*s, c);
    };
    const double base = run({});
    std::printf("%-10s %-14s %10.2f %10s\n", "none", "(16 voices)", 100 * base, "");
    const auto row = [&](const char* block, const NamedType& t, const std::vector<Msg>& setup) {
        const double l = run(setup);
        std::printf("%-10s %-14s %10.2f %10.2f\n", block, t.name, 100 * l, 100 * (l - base));
        std::fflush(stdout);
    };
    const auto sends = [](std::uint8_t cc) {
        std::vector<Msg> v;
        for (std::uint8_t ch = 0; ch < 16; ++ch) v.push_back({static_cast<std::uint8_t>(0xB0 | ch), cc, 100});
        return v;
    };
    for (const auto& t : reverbs) {
        auto v = sends(91);
        v.push_back(xg({0x02, 0x01, 0x00, t.msb, t.lsb}));
        row("reverb", t, v);
    }
    for (const auto& t : choruses) {
        auto v = sends(93);
        v.push_back(xg({0x02, 0x01, 0x20, t.msb, t.lsb}));
        row("chorus", t, v);
    }
    for (const auto& t : variations) {
        auto v = sends(94);
        v.push_back(xg({0x02, 0x01, 0x40, t.msb, t.lsb}));
        v.push_back(xg({0x02, 0x01, 0x5A, 0x01}));  // system connection
        row("var sys", t, v);
    }
    for (const auto& t : variations) {
        std::vector<Msg> v{xg({0x02, 0x01, 0x40, t.msb, t.lsb}), xg({0x02, 0x01, 0x5A, 0x00}), xg({0x02, 0x01, 0x5B, 0x00})};
        row("var ins", t, v);  // insertion on part 1
    }
    // hvyxg's own XG2.0 insertion types (the DLLs have none of them).
    std::vector<NamedType> insertions = variations;
    insertions.insert(insertions.end(), {{0x53, 0, "Compressor"}, {0x54, 0, "Noise Gate"}, {0x5E, 0, "Lo-Fi"},
                                         {0x71, 0, "Ring Mod"}});
    for (const auto& t : insertions) {
        std::vector<Msg> v{xg({0x03, 0x00, 0x00, t.msb, t.lsb}), xg({0x03, 0x00, 0x0C, 0x00})};
        row("ins 1", t, v);  // XG2.0 insertion block 1 on part 1
    }
    return 0;
}

int worst_mode(Config& c, std::size_t voices) {
    auto s = make(c, static_cast<std::uint16_t>(voices));
    const std::size_t n = voices ? voices : 1024;
    // Every send on, the heaviest-looking types, variation as insertion on part 5, four XG2.0
    // insertion blocks on parts 1-4, multi EQ default (on).
    for (std::uint8_t ch = 0; ch < 16; ++ch)
        for (const std::uint8_t cc : {91, 93, 94}) send(*s, {static_cast<std::uint8_t>(0xB0 | ch), cc, 127});
    send(*s, xg({0x02, 0x01, 0x00, 0x01, 0x00}));
    send(*s, xg({0x02, 0x01, 0x20, 0x43, 0x00}));
    send(*s, xg({0x02, 0x01, 0x40, 0x45, 0x00}));
    send(*s, xg({0x02, 0x01, 0x5A, 0x00}));
    send(*s, xg({0x02, 0x01, 0x5B, 0x04}));
    const std::uint8_t ins[4] = {0x49, 0x48, 0x45, 0x4E};
    for (std::uint8_t b = 0; b < 4; ++b) {
        send(*s, xg({0x03, b, 0x00, ins[b], 0x00}));
        send(*s, xg({0x03, b, 0x0C, b}));
    }
    const std::size_t got = hold_voices(*s, n);
    const double load = measure(*s, c);
    // Note-on bursts at full polyphony: 64 new notes in one block, each stealing a voice; the
    // slowest such block against its real-time budget (best of --repeat rounds of 8 bursts).
    std::vector<float> buf(2 * 128);
    const double budget = 128.0 / static_cast<double>(s->sample_rate());
    double burst = 1e30;
    for (int r = 0; r < c.repeat; ++r) {
        double worst = 0.0;
        for (int k = 0; k < 8; ++k) {
            for (int i = 0; i < 64; ++i) {
                const auto ch = static_cast<std::uint8_t>((i + k) % 15 + ((i + k) % 15 >= 9 ? 1 : 0));
                const Msg on{static_cast<std::uint8_t>(0x90 | ch), static_cast<std::uint8_t>(24 + (i * 7 + k * 13 + r) % 96), 100};
                s->midi(on, 1);  // queued: applied inside the timed render
            }
            const double t0 = cpu_now();
            s->render(buf.data(), 128);
            worst = std::max(worst, cpu_now() - t0);
            for (int i = 0; i < 20; ++i) s->render(buf.data(), 128);
        }
        burst = std::min(burst, worst);
    }
    std::printf("worst case: %zu voices, all effects and 4 insertion blocks: load %.2f %%; "
                "block with a 64-note-on burst: %.1f %% of its time\n",
                got, 100 * load, 100 * burst / budget);
    return 0;
}

int song_mode(Config& c, const std::vector<std::string>& files) {
    std::printf("%-22s %8s %9s %10s %12s %12s\n", "song", "audio s", "voices≤", "load %", "worst blk %", "p99 blk %");
    for (const std::string& f : files) {
        std::string why;
        const auto song = render::read_song(f, why);
        if (!song) {
            std::fprintf(stderr, "hvyxg-bench: %s\n", why.c_str());
            return 1;
        }
        double best = 1e30, worst_block = 0.0, p99 = 0.0;
        std::size_t peak = 0;
        double audio = 0.0;
        for (int r = 0; r < c.repeat; ++r) {
            hvyxg::Synth s(c.o);
            const double rate = s.sample_rate();
            const std::size_t blk = 128;
            std::vector<float> buf(2 * blk);
            std::vector<double> blocks;
            const auto end = static_cast<std::uint64_t>((static_cast<double>(song->length_us()) / 1e6 + 3.0) * rate);
            blocks.reserve(static_cast<std::size_t>(end / blk + 1));
            std::size_t next = 0;
            double total = 0.0;
            for (std::uint64_t at = 0; at < end; at += blk) {
                for (; next < song->count(); ++next) {
                    const midifile::Event& e = song->event(static_cast<std::uint32_t>(next));
                    const std::uint64_t fr = render::frame_of(e.us, static_cast<std::uint32_t>(rate));
                    if (fr >= at + blk) break;
                    s.ump(std::span(e.ump, e.words), static_cast<std::uint32_t>(fr > at ? fr - at : 0));
                }
                const double t0 = cpu_now();
                s.render(buf.data(), blk);
                const double dt = cpu_now() - t0;
                total += dt;
                blocks.push_back(dt);
                peak = std::max(peak, s.voice_count());
            }
            audio = static_cast<double>(end) / rate;
            if (total / audio < best) {
                best = total / audio;
                std::sort(blocks.begin(), blocks.end());
                const double per_block = static_cast<double>(blk) / rate;
                worst_block = blocks.back() / per_block;
                p99 = blocks[blocks.size() * 99 / 100] / per_block;
            }
        }
        std::string name = f.substr(f.find_last_of('/') + 1);
        std::printf("%-22s %8.1f %9zu %10.2f %12.1f %12.1f\n", name.c_str(), audio, peak, 100 * best, 100 * worst_block, 100 * p99);
        std::fflush(stdout);
    }
    return 0;
}

int usage() {
    std::fprintf(stderr,
                 "usage: hvyxg-bench [--plugin DLL]... [--plugin-path DIR] [--bin F --data F] [--enable-mu] [--rate HZ]\n"
                 "                   [--seconds S] [--repeat R]\n"
                 "                   [--interp linear|hermite|lanczos] voices [N…] | effects | worst [N] | song FILE…\n");
    return 2;
}

} // namespace

int main(int argc, char** argv) {
    Config c;
    if (const char* e = std::getenv("HVYXG_TBL_BIN")) c.o.bin_file = e;
    if (const char* e = std::getenv("HVYXG_TBL_DATA")) c.o.data_file = e;
    std::vector<std::string> pos;
    for (int i = 1; i < argc; ++i) {
        const std::string a = argv[i];
        const auto next = [&]() -> std::string {
            if (i + 1 >= argc) std::exit(usage());
            return argv[++i];
        };
        if (a == "--bin") c.o.bin_file = next();
        else if (a == "--plugin") c.o.plugin_files.emplace_back(next());
        else if (a == "--plugin-path") c.o.plugin_path = next();
        else if (a == "--data") c.o.data_file = next();
        else if (a == "--enable-mu") c.o.enable_mu = true;
        else if (a == "--rate") c.o.sample_rate = std::stof(next());
        else if (a == "--seconds") c.seconds = std::stod(next());
        else if (a == "--repeat") c.repeat = std::max(1, std::stoi(next()));
        else if (a == "--interp") {
            const std::string m = next();
            c.o.interpolation = m == "hermite" ? hvyxg::Interpolation::Hermite
                              : m == "lanczos" ? hvyxg::Interpolation::Lanczos3
                                               : hvyxg::Interpolation::Linear;
        } else if (!a.empty() && a[0] == '-') return usage();
        else pos.push_back(a);
    }
    if (pos.empty()) return usage();
    try {
        // The engine tables from the DLLs, and the bank, loaded once; every synth shares them.
        std::vector<std::filesystem::path> dlls = c.o.plugin_files;
        if (!c.o.plugin_path.empty())
            for (const auto& p : hvyxg::find_dlls(c.o.plugin_path)) dlls.push_back(p);
        hvyxg::BankOptions bo;
        bo.tables = hvyxg::load_engine_tables(dlls);
        bo.enable_mu = c.o.enable_mu;
        c.o.bank = hvyxg::load_bank(c.o.bin_file, c.o.data_file, hvyxg::Model::Auto, bo);
        std::vector<std::size_t> nums;
        if (pos[0] != "song")
            for (std::size_t i = 1; i < pos.size(); ++i) nums.push_back(std::stoul(pos[i]));
        if (pos[0] == "voices") return voices_mode(c, nums);
        if (pos[0] == "effects") return effects_mode(c);
        if (pos[0] == "worst") return worst_mode(c, nums.empty() ? 0 : nums[0]);
        if (pos[0] == "song") return song_mode(c, std::vector<std::string>(pos.begin() + 1, pos.end()));
        return usage();
    } catch (const std::exception& e) {
        std::fprintf(stderr, "hvyxg-bench: %s\n", e.what());
        return 1;
    }
}
