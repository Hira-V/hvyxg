// Copyright (C) 2026 Kevin H. Patterson
// SPDX-License-Identifier: BSD-3-Clause (see LICENSE)
#pragma once
// hvyxg's public API: a Yamaha S-YXG50 / S-YXG2006LE software synthesizer.
//
//   hvyxg::Synth::Options o;
//   o.bin_file = "sxgbin41.tbl";
//   o.data_file = "sxgwave4.tbl";
//   hvyxg::Synth synth(o);                 // throws hvyxg::BankError
//   synth.midi(bytes);                     // one complete message (or SysEx F0 … F7)
//   synth.render(buffer, frames);          // interleaved stereo float
//
// Threads (docs/ENGINE.md, "Threads and real-time use"):
// - Loading (`load_bank`, `load_mu_roms`, …), constructing and destroying a `Synth` may run on
//   any thread; they read files, allocate and may throw. Not real-time safe.
// - A loaded `Bank` is immutable: one `std::shared_ptr<const Bank>` may serve any number of
//   synths on any threads at the same time.
// - A `Synth` is not synchronised: never call its members concurrently. Call `midi`,
//   `midi_stream`, `render`, `reset` and the queries from one thread (normally the audio
//   thread) or serialise them yourself; a host that receives MIDI on another thread hands it
//   over (e.g. through its own lock-free FIFO) and calls `midi` before `render`. Different
//   synths are independent.
// - On that thread nothing blocks: `render`, `midi`, `midi_stream` and `reset` take no locks,
//   do no I/O, do not throw, and do not allocate (`render` never; `midi` while at most 1024
//   messages / 16 KB wait for a later block; `midi_stream` for SysEx up to 4 KB). Any MIDI
//   input is safe (fuzzed).

#include <cstddef>
#include <cstdint>
#if !HVYXG_NO_FILES
#include <filesystem>
#endif
#include <memory>
#include <span>
#include <string>
#include <vector>

#include "hvyxg/types.hpp"

namespace hvyxg {

class Bank;
struct SynthAccess;  // internal (src/synth_access.hpp)

class Synth {
public:
    /// How a synth plays (no files: an embedded host builds these alone).
    struct Settings {
        float sample_rate = 44100.0f;
        /// Voices; 0 = the modelled DLL's (S-YXG50 128, S-YXG2006LE 32). At most
        /// `kMaxPolyphony` (else `Synth(Options)` throws std::invalid_argument; `check`): cost grows
        /// linearly, about 0.035 % of one core per sounding voice on an Apple M1 Max core
        /// (docs/PERFORMANCE.md). Ignored with `dll_exact`.
        std::uint16_t max_polyphony = 0;
        /// `Linear` = the DLLs.
        Interpolation interpolation = Interpolation::Linear;
        /// Superset: fade-out time in ms of a voice stolen for a new note. 0 = the modelled
        /// DLL's fast steal release (S-YXG50 damp, S-YXG2006LE rate 0x7E: ≈ 1.5 ms, can click
        /// on sustained sounds). Above 0 the stolen sound keeps playing and fades linearly to
        /// silence over this time, and both engines keep one fading slot per voice (the
        /// 2006LE at least its DLL's 64). All sound off and resets still cut at once.
        float steal_fade_ms = 0.0f;
        /// SysEx device number 0..15, or −1 = accept all (the DLLs ignore it).
        int device_id = -1;
        /// MIDI ports of 16 parts each (1..4; the DLLs have 1).
        std::size_t ports = 1;
        /// Turn off every superset behaviour, so the output follows the reference DLL (the
        /// stock DLL for the stock banks, the patched DLL with `enable_mu`): see
        /// docs/ENGINE.md, "DLL-exact mode".
        bool dll_exact = false;
        /// Superset option: play the part EQ and the drum-note EQ (08 nn 72 / 73, 3n rr 20 /
        /// 21) with the MU2000's gain strength instead of the XG specification's ±12 dB: the
        /// MU2000 acts in steps of four values and reaches only −2.1 … +2.3 dB (docs/ENGINE.md,
        /// "Superset: EQ and HPF"). For songs written on MU hardware; off by default, ignored
        /// with `dll_exact` (no part EQ at all).
        bool mu_part_eq = false;
        /// Superset option for the S-YXG2006LE bank (`render_yxg --2006full`): accept and act
        /// on the full XG message set and parameter behaviour the S-YXG50 engine supports
        /// instead of the S-YXG2006LE's XGlite subset (XG parameters after a GS reset, GS
        /// messages that carry several parameters, the S-YXG50's variation parameter ranges
        /// and distortion drive; docs/ENGINE.md, "Full XG on the S-YXG2006LE"). The 2006LE's
        /// voices and sound stay its own. Ignored for other banks and with `dll_exact`.
        bool full_xg_2006le = false;
    };

#if !HVYXG_NO_FILES
    /// Settings, and where the bank and the engine tables come from (hosted builds).
    struct Options : Settings {
        /// Bank files (bin / voice table and data / wave table). Ignored when `bank` is set.
        std::filesystem::path bin_file;
        std::filesystem::path data_file;
        /// A plugin DLL (also see `plugin_files`). When no bank is given otherwise (`bank`,
        /// `bin_data`, `bin_file` / `data_file`, `bank_paths`), the first of the DLLs that carries
        /// its bank tables as RT_RCDATA resources (SXG-Create-NF `--embed`, or the 5 MB S-YXG50)
        /// supplies the bank (hvyxg/plugin_tables.hpp); combine with `enable_mu` for an
        /// SXG-Create-NF MU plugin.
        std::filesystem::path plugin_file;
        /// DLLs that supply the engine tables (searched by content, hvyxg ships none of them):
        /// `plugin_file`, every file in `plugin_files` and every PE file under `plugin_path`
        /// (recursively). The S-YXG50 engine needs syxg50.dll or an SXG-Create-NF plugin, the
        /// S-YXG2006LE engine the S-YXG2006LE DLL; the constructor throws a BankError naming
        /// what is missing. Ignored when `tables` or `bank` is set.
        std::vector<std::filesystem::path> plugin_files;
        std::filesystem::path plugin_path;
        /// Engine tables loaded before (hvyxg/engine_tables.hpp), shared between synths and banks.
        std::shared_ptr<const EngineTables> tables;
        /// Bank files already in memory (used when `bin_data` is not empty).
        std::vector<std::uint8_t> bin_data;
        std::vector<std::uint8_t> wave_data;
        /// A bank loaded before (`load_bank`), shared between synths; it carries its engine tables.
        std::shared_ptr<const Bank> bank;
        /// `Auto` sniffs the bin header.
        Model model = Model::Auto;
        /// Load an SXG-Create-NF MU bank (S-YXG50 bin / wave table converted from MU50 …
        /// MU2000 ROMs) and play it as the SXG-Create-NF patched syxg50.dll does
        /// (docs/ENGINE.md, "MU banks"). The layout is read from the table unless `mu_layout`
        /// forces it. Ignored when `bank` is set (pass these to `load_bank` instead).
        bool enable_mu = false;
        MuLayout mu_layout = MuLayout::Auto;
        MuVoiceMap mu_voice_map = MuVoiceMap::Default;
        /// Files and / or directories (searched recursively) where the bank is found by
        /// content, when `bin_file` / `data_file` are not given: the stock S-YXG50 (4 MB before
        /// 2 MB) and S-YXG2006LE tables, SXG-Create-NF's MU tables, or MU ROM sets, built into
        /// an MU bank with hvyxg's port of SXG-Create-NF (hvyxg/mu_rom.hpp). `model`,
        /// `enable_mu` and `mu_rom_model` choose between kinds; an MU bank implies `enable_mu`.
        std::vector<std::filesystem::path> bank_paths;
        /// Which MU bank to use when `bank_paths` holds several.
        MuRomModel mu_rom_model = MuRomModel::Auto;
        /// Optional directory for a cache of the converted tables (empty = convert every time).
        std::filesystem::path mu_cache;
    };
#endif

    /// The largest `Options::max_polyphony`.
    static constexpr std::uint16_t kMaxPolyphony = 1024;

    /// A synth playing `bank` (non-null), which carries its engine tables. Never throws: a
    /// sample rate outside 8 … 192 kHz or a polyphony above `kMaxPolyphony` is clamped
    /// (`check` says whether the settings are in range).
    Synth(std::shared_ptr<const Bank> bank, const Settings& settings);
    /// Whether the settings are in range; false and `error` otherwise.
    static bool check(const Settings& settings, std::string& error);
#if !HVYXG_NO_FILES
    /// Loads the bank (unless `Options::bank` is set). Throws `BankError`, or
    /// std::invalid_argument for a sample rate outside 8 … 192 kHz or a polyphony above
    /// `kMaxPolyphony`.
    explicit Synth(Options options);
#endif
    ~Synth();
    Synth(const Synth&) = delete;
    Synth& operator=(const Synth&) = delete;

    /// One complete MIDI message: a channel message with its status byte, or a SysEx
    /// `F0 … F7`. System common / real-time messages are ignored. Ports above 3 count as 3;
    /// ports ≥ `Options::ports` reach only parts whose receive channel was moved there. The message takes effect
    /// from the start of the engine block (S-YXG50 128 frames, S-YXG2006LE 64) that contains
    /// frame `frame_offset` of the next `render` call, as in the DLLs; if that block has
    /// already started rendering, from the next block.
    void midi(std::span<const std::uint8_t> msg, std::uint32_t frame_offset = 0, std::size_t port = 0);

    /// A raw byte stream (running status, real-time bytes interleaved, SysEx split across
    /// calls), for live MIDI input. Complete messages go to `midi(…, frame_offset, port)`.
    void midi_stream(std::span<const std::uint8_t> bytes, std::uint32_t frame_offset = 0,
                     std::size_t port = 0);

    /// One Universal MIDI Packet (all of its words), its group the port. MIDI 1.0 channel voice
    /// messages (type 2) play as `midi` does; 7-bit SysEx packets (type 3) are gathered per
    /// group (groups 0..3, up to 4 KB) and the message played when it is complete; a System
    /// Reset (type 1, FF) is an XG System On, the power-on state. Everything else is ignored
    /// (MIDI 2.0 channel voice messages included: send MIDI 1.0). Real-time safe as `midi`.
    void ump(std::span<const std::uint32_t> words, std::uint32_t frame_offset = 0);

    /// Render `frames` frames of interleaved stereo (L, R, …) into `out`.
    void render(float* interleaved_stereo, std::size_t frames);
    /// The same as 32-bit integers at a 16-bit sample's scale (full scale ±32768, rounded to
    /// nearest), not clipped: Hira-V's `midi::Instrument::render` and hvopl3's driver.
    void render(std::int32_t* interleaved_stereo, std::size_t frames);

    /// Power-on state (XG mode, as both DLLs); pending messages are dropped.
    void reset();

    // Introspection.
    Model model() const;
    float sample_rate() const;
    /// The engine block size (128 / 64 frames).
    std::size_t block_size() const;
    ResetMode reset_mode() const;
    std::size_t part_count() const;
    /// The part's latched program (bank MSB, LSB, program), as the tone module plays it.
    struct PartProgram {
        std::uint8_t bank_msb, bank_lsb, program;
        bool drums;
    };
    PartProgram part_program(std::size_t part) const;
    /// Something may still sound (voices or effect tails).
    bool active() const;
    /// Voices sounding now (S-YXG50: chip voices, S-YXG2006LE: DSP voices; stolen voices still
    /// fading out included).
    std::size_t voice_count() const;
    const Bank& bank() const;

private:
    friend struct SynthAccess;
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace hvyxg
