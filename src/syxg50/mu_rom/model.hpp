// Copyright (C) 2026 Kevin H. Patterson
// SPDX-License-Identifier: BSD-3-Clause (see LICENSE)
// Translated from SXG-Create-NF, Copyright (c) 2026 Soundshock, BSD-3-Clause (THIRD_PARTY.md)
#pragma once
// The converter's object model, after SXG-Create-NF's utility/table.py (BSD-3-Clause,
// Copyright 2026 Soundshock): samples keyed by their (source) loop address, multisamples
// ("wave banks"), voices with their elements, drum voices, voice banks and drum kits, each
// keyed by its ROM address as the Python dicts are. Objects that Python shares between dicts
// are shared here too (shared_ptr), since the converter mutates them in place.

#include <array>
#include <cstdint>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "syxg50/mu_rom/py.hpp"
#include "fail.hpp"

namespace hvyxg::syxg50::mu_rom {

using Bytes = std::vector<std::uint8_t>;
/// Sample-pool key (Python int: loop address plus reverse / HPF / baked-EG tags).
using SKey = i128;

enum class Fmt : std::uint8_t { S16, U16, Adpcm, S12, S8, U8, Tmp };

inline int fmt_bits(Fmt f) {
    switch (f) {
    case Fmt::S16:
    case Fmt::U16: return 16;
    case Fmt::S12: return 12;
    case Fmt::S8:
    case Fmt::U8:
    case Fmt::Adpcm: return 8;
    default: HVYXG_THROW(ConvertError("sample format without a size"));
    }
}

/// MU90 / MU1000 format bits (top two bits of the format byte).
inline Fmt fmt_from_mu90(int b) {
    switch (b & 0xC0) {
    case 0x80: return Fmt::S8;
    case 0x40: return Fmt::S12;
    case 0xC0: return Fmt::Adpcm;
    default: return Fmt::S16;
    }
}

/// S-YXG50 target format (dataenum.get_SampleFormat_Target).
inline Fmt fmt_target(Fmt f) {
    switch (f) {
    case Fmt::U16:
    case Fmt::U8: return f;
    case Fmt::S8: return Fmt::U8;
    case Fmt::S12:
    case Fmt::S16:
    case Fmt::Adpcm: return Fmt::U16;
    default: HVYXG_THROW(ConvertError("no target format"));
    }
}

inline std::uint8_t fmt_byte_syxg50(Fmt f) {
    if (f == Fmt::U16) return 0x00;
    if (f == Fmt::U8) return 0x80;
    HVYXG_THROW(ConvertError("not an S-YXG50 sample format"));
}

/// An amp EG baked into a sample copy (elemreduce.Env_Sample).
struct EnvBake {
    std::vector<double> gain_db;
    std::optional<double> loop_db;
    double ratio = 1.0;
};

struct Sample {
    SKey address_src = 0;
    std::int64_t offset_negative = 0;
    std::int64_t offset_positive = 0;
    std::int64_t loop_address = 0;
    Fmt sample_type = Fmt::S16;
    int encoding_parameters = 0;
    std::int64_t out_loop_address = -1;
    Fmt out_sample_type = Fmt::Tmp;
    bool written = false;
    // Attributes the Python code adds on the way (getattr defaults).
    bool reverse = false;
    std::int64_t src_offset_negative = 0;
    std::int64_t src_offset_positive = 0;
    int hpf_fc = 0;
    std::shared_ptr<const EnvBake> env_bake;
    std::int64_t src_neg = 0;
    std::int64_t src_pos = 0;
    std::int64_t lead_in = 0;
    std::int64_t adpcm_loop_len = 0;
    bool used_by_drum = false;

    std::int64_t len_samples() const { return offset_positive + offset_negative; }
    std::int64_t start_address() const {
        return loop_address - static_cast<std::int64_t>(offset_negative * fmt_bits(sample_type) / 8.0);
    }
    std::int64_t end_address() const {
        return loop_address + static_cast<std::int64_t>(offset_positive * fmt_bits(sample_type) / 8.0);
    }
    std::int64_t len_bytes() const {
        return static_cast<std::int64_t>((offset_negative + offset_positive) * fmt_bits(sample_type) / 8.0);
    }
};
using SampleP = std::shared_ptr<Sample>;
using SampleDict = ODict<SKey, SampleP, I128Hash>;

/// table.Sample.ReturnGreater: the longer sample, with the larger offsets of both.
inline SampleP return_greater(const SampleP& a, const SampleP& b) {
    SampleP out = a->len_samples() >= b->len_samples() ? a : b;
    const std::int64_t n = std::max(a->offset_negative, b->offset_negative);
    const std::int64_t p = std::max(a->offset_positive, b->offset_positive);
    out->offset_negative = n;
    out->offset_positive = p;
    return out;
}

/// table.AddToSampleList.
inline void add_sample(SampleDict& d, const SampleP& s) {
    if (SampleP* old = d.find(s->address_src)) {
        *old = return_greater(*old, s);
        return;
    }
    d.set(s->address_src, s);
}

/// table.MergeSampleDicts(d1, d2) assigned back to d1.
inline void merge_samples(SampleDict& d1, const SampleDict& d2) {
    d2.each([&](const SKey& k, const SampleP& v) {
        if (SampleP* old = d1.find(k)) *old = return_greater(*old, v);
        else d1.set(k, v);
    });
}

struct Wave {
    std::int64_t address_src = 0;
    std::int64_t offset_negative = 0;
    std::int64_t offset_positive = 0;
    SKey loop_address_src = 0;
    int attenuation = 0;
    int tune_note = 0;
    int tune_cent = 0;
    int key_min = 0;
    int key_max = 0;
};

struct WaveBank {
    std::string address_src;
    std::vector<Wave> waves;
    std::array<int, 3> index{-1, -1, -1};  // multisample number per page (index, index1, index2)
    Bytes out_data;
    std::int64_t offset = -1;
};
using WaveBankP = std::shared_ptr<WaveBank>;
using WaveBankDict = ODict<std::string, WaveBankP>;

struct Element {
    std::string wavebank_address;
    Bytes data;
};

struct Voice {
    std::string address_src;
    int volume = 0;
    std::string name;  // raw 8 bytes
    std::vector<Element> elements;
    bool converted = false;
    int extvoice_index = -1;
    Bytes data;
    std::int64_t offset = -1;
    int voicebank = 0;  // 0 none, 1 bank A, 2 bank B
    int page = 0;
};
using VoiceP = std::shared_ptr<Voice>;

struct DrumVoice {
    Bytes data;
    SKey sample_hash = 0;
    std::string ext_voice;  // empty: internal sample
    std::int64_t offset_negative = 0;
    std::int64_t offset_positive = 0;
    bool converted = false;
    int extvoice_index = -1;
    std::int64_t offset = -1;
    int vel_pitch = 0x40;
    int vel_lpf = 0x40;
    double level_boost = 0.0;
    bool fast_decay = false;
};
using DrumVoiceP = std::shared_ptr<DrumVoice>;

enum class BankKind : std::uint8_t { Gs, Xg, Sfx, Gm2, Msb48, GsDrums, XgDrums, SfxDrums, Gm2Drums };

/// dataenum.BankToLSBMSB → (lsb, msb).
inline std::pair<int, int> bank_lsb_msb(BankKind b, int byte) {
    switch (b) {
    case BankKind::Gs:
    case BankKind::Sfx: return {0, byte};
    case BankKind::Xg: return {byte, 0};
    case BankKind::Gm2: return {byte, 121};
    case BankKind::Msb48: return {byte, 48};
    default: HVYXG_THROW(ConvertError("not a voice bank"));
    }
}

struct VoiceBank {
    std::int64_t prg_address = 0;
    BankKind bank = BankKind::Xg;
    int lsb = 0, msb = 0;
    std::shared_ptr<std::map<int, std::string>> voices;  // program → voice key
    std::vector<std::pair<BankKind, int>> aliases;
    int index = -1;
    bool alt = false;

    int relevant_byte() const { return (bank == BankKind::Gs || bank == BankKind::Sfx) ? msb : lsb; }
};
using VoiceBankP = std::shared_ptr<VoiceBank>;

struct DrumKit {
    std::int64_t keymap_address = 0;
    BankKind bank = BankKind::GsDrums;
    int prg = 0;
    ODict<int, std::int64_t> drumvoices;  // key → drum voice address
    std::vector<std::pair<BankKind, int>> aliases;
    int index = -1;
};
using DrumKitP = std::shared_ptr<DrumKit>;

struct Table {
    SampleDict samples;
    WaveBankDict wavebanks;
    ODict<std::string, VoiceP> voices;
    ODict<std::int64_t, DrumVoiceP> drumvoices;
    ODict<std::int64_t, DrumKitP> drumkits;
    ODict<std::int64_t, VoiceBankP> voice_banks;
};

} // namespace hvyxg::syxg50::mu_rom
