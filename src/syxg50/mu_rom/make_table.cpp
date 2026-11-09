// Copyright (C) 2026 Kevin H. Patterson
// SPDX-License-Identifier: BSD-3-Clause (see LICENSE)
// Translated from SXG-Create-NF, Copyright (c) 2026 Soundshock, BSD-3-Clause (THIRD_PARTY.md)
// Port of SXG-Create-NF utility/makeSYXG50.py, tableconvert.py (fromMU90 / fromMU1000 /
// ToBigDrumVoice) and drumvel.py (BSD-3-Clause, Copyright 2026 Soundshock), for the big table
// layout of the MU100-and-later models. The arithmetic follows the Python expressions (py.hpp).
#include "syxg50/mu_rom/make_table.hpp"

#include <algorithm>
#include <cmath>
#include <map>
#include <set>

#include "syxg50/dll_tables.hpp"
#include "syxg50/mu_rom/sample_convert.hpp"
#include "syxg50/mu_rom/tables.hpp"
#include "fail.hpp"

namespace hvyxg::syxg50::mu_rom {

namespace {

/// A Python `(i for i in range(a, b))` generator.
struct Gen {
    int next_, end_;
    int next() {
        check(next_ < end_, "index generator exhausted");
        return next_++;
    }
    std::optional<int> try_next() { return next_ < end_ ? std::optional<int>(next_++) : std::nullopt; }
};

void put_be(Bytes& b, std::size_t at, std::int64_t v, int n) {
    check(v >= 0 && (n >= 8 || v < (std::int64_t{1} << (8 * n))), "value does not fit its field");
    for (int i = n - 1; i >= 0; --i, v >>= 8) b[at + static_cast<std::size_t>(i)] = static_cast<std::uint8_t>(v & 0xFF);
}
void put_le(Bytes& b, std::size_t at, std::int64_t v, int n) {
    check(v >= 0 && (n >= 8 || v < (std::int64_t{1} << (8 * n))), "value does not fit its field");
    for (int i = 0; i < n; ++i, v >>= 8) b[at + static_cast<std::size_t>(i)] = static_cast<std::uint8_t>(v & 0xFF);
}
void put_byte(Bytes& b, std::size_t at, std::int64_t v) {
    check(v >= 0 && v < 256, "byte out of range");
    b[at] = static_cast<std::uint8_t>(v);
}

int s8(int b) { return b > 127 ? b - 256 : b; }

// ---- tableconvert.py

double drum_decay_slope(int rate) {
    const auto& pts = kDecaySlope;
    constexpr std::size_t n = sizeof kDecaySlope / sizeof kDecaySlope[0];
    if (rate >= 0x7E) return 1e6;
    if (rate <= pts[0].rate) return pts[0].slope * std::pow(2.0, (rate - pts[0].rate) / 8.0);
    if (rate >= pts[n - 1].rate) return pts[n - 1].slope * std::pow(2.0, (rate - pts[n - 1].rate) / 8.0);
    for (std::size_t i = 0; i + 1 < n; ++i) {
        const int r0 = pts[i].rate, r1 = pts[i + 1].rate;
        const double s0 = pts[i].slope, s1 = pts[i + 1].slope;
        if (r0 <= rate && rate <= r1)
            return std::exp(std::log(s0) + static_cast<double>(rate - r0) / (r1 - r0) * (std::log(s1) - std::log(s0)));
    }
    HVYXG_THROW(ConvertError("decay slope"));
}

int fast_decay2_rate(int d1, int d2) {
    const double t1 = 0.75 / drum_decay_slope(d1);
    const double target = 4.343 / (t1 + 4.343 / drum_decay_slope(d2));
    int best = 0;
    double bk = 0;
    for (int r = 0; r < 0x7E; ++r) {
        const double k = std::fabs(std::log(drum_decay_slope(r)) - std::log(target));
        if (r == 0 || k < bk) {
            best = r;
            bk = k;
        }
    }
    return best;
}

/// fromMU90.ConvertDrumVoice: 42-byte MU90-layout drum voice → 30-byte S-YXG50 one.
void convert_drum_voice(DrumVoice& drum, const Sample* sample, const Voice* ext) {
    const Bytes& d = drum.data;
    check(d.size() == 42, "drum voice size");
    Bytes nd(d.begin(), d.begin() + 30);
    for (int i = 16; i < 30; ++i) nd[static_cast<std::size_t>(i)] = 0;
    if (ext == nullptr) {
        nd[13] = static_cast<std::uint8_t>(std::max(0, nd[13] - 0x21));
        if (d[13] == 0x7F && d[14] == d[15]) {
            nd[13] = 0x60;
        } else if (d[13] == 0x7F && d[14] >= 0x30 && (d[15] >= 0x7E || (0x50 <= d[15] && d[14] < 0x40))) {
            nd[13] = 0x60;
            nd[14] = nd[15] = static_cast<std::uint8_t>(fast_decay2_rate(d[14], d[15]));
            drum.fast_decay = true;
        }
    }
    if (ext != nullptr) {
        check(ext->converted && ext->extvoice_index >= 0, "ext voice not converted");
        put_be(nd, 16, ext->extvoice_index, 2);
    } else if (sample != nullptr) {
        nd[18] = d[26];
        nd[28] = d[27];
        nd[29] = d[28];
        nd[16] = 0xFF;
        nd[17] = 0xFF;
        put_be(nd, 19, std::min<std::int64_t>(drum.offset_negative, 0xFFFF), 2);
        put_be(nd, 21, drum.offset_positive, 3);
        put_be(nd, 24, sample->out_loop_address & 0xFFFFFF, 3);
        nd[27] = fmt_byte_syxg50(sample->out_sample_type);
    }
    drum.data = std::move(nd);
}

/// tableconvert.ToBigDrumVoice.
void to_big_drum_voice(DrumVoice& drum, const Sample* sample, const Voice* ext) {
    Bytes out = drum.data;
    out[10] = drum.data[18];
    if (ext != nullptr) {
        check(ext->extvoice_index < 0xFF00, "ext voice index");
        put_le(out, 16, ext->extvoice_index, 2);
        for (int i = 18; i < 30; ++i) out[static_cast<std::size_t>(i)] = 0;
    } else {
        check(sample != nullptr, "drum voice without sample");
        out[16] = 0xFF;
        out[17] = 0xFF;
        put_be(out, 18, drum.offset_negative, 3);
        put_be(out, 21, drum.offset_positive, 3);
        check(sample->out_loop_address < 0x40000000, "drum sample beyond 1 GB");
        const std::int64_t fmt = fmt_byte_syxg50(sample->out_sample_type) & 0xC0;
        put_be(out, 24, sample->out_loop_address | (fmt << 24), 4);
    }
    drum.data = std::move(out);
}

// ---- makeSYXG50.py writers

Bytes write_wavedata(const WaveBank& wb, const std::vector<SampleP>& samples) {
    check(!wb.waves.empty() && wb.waves.size() == samples.size(), "wave data");
    Bytes out(wb.waves.size() * 16, 0);
    for (std::size_t i = 0; i < wb.waves.size(); ++i) {
        const Wave& w = wb.waves[i];
        const Sample& s = *samples[i];
        check(s.written && s.out_loop_address >= 0, "sample not written");
        const std::size_t o = i * 16;
        put_byte(out, o + 0, w.attenuation);
        put_byte(out, o + 1, w.tune_note);
        put_byte(out, o + 2, w.tune_cent);
        const std::int64_t neg = w.offset_negative + s.lead_in;
        check(neg <= 0xFFFFFF && w.offset_positive <= 0xFFFFFF, "wave offsets");
        put_be(out, o + 3, neg, 3);
        put_be(out, o + 6, w.offset_positive, 3);
        check(s.out_loop_address <= 0xFFFFFFFF, "wave address");
        put_be(out, o + 9, s.out_loop_address, 4);
        out[o + 13] = fmt_byte_syxg50(s.out_sample_type);
        put_byte(out, o + 14, w.key_min);
        put_byte(out, o + 15, w.key_max);
        if (i == wb.waves.size() - 1) {
            check(w.key_max >= 0x7F, "last key range");
            out[o + 15] = 0xFF;
        }
    }
    return out;
}

Bytes write_voice(const Voice& v) {
    check(v.converted && !v.elements.empty(), "voice not converted");
    Bytes out{static_cast<std::uint8_t>(v.volume), static_cast<std::uint8_t>(v.elements.size() == 2 ? 3 : 1)};
    for (const Element& e : v.elements) out.insert(out.end(), e.data.begin(), e.data.end());
    return out;
}

std::size_t voice_len(const Voice& v) {
    std::size_t n = 2;
    for (const Element& e : v.elements) n += e.data.size();
    return n;
}

// ---- drumvel.py

constexpr double kVelPitchCents = 0.25;
constexpr int kVelPitchRef = 100;
constexpr double kVelLpfUnits = 0.5;
constexpr int kVelLpfRef = 100;
constexpr int kSense = 7;
constexpr double kPegCentsPerStep = 75.0 / 16;
constexpr int kFegUnitsPerStep = 32;
constexpr double kBoostK = 1.5;

double peg_factor(int v) { return 1 - static_cast<double>(((128 - v) * kSense * 36) >> 7) / 256; }
double feg_factor(const dll::Tables& tb, int v) {
    return 1 - static_cast<double>(((128 - tb.kFegVelCurve0[static_cast<std::size_t>(v)]) * kSense * 36) >> 7) / 256;
}

struct VelPlan {
    int peg_level = 0x40;
    std::int64_t cents = 0;
    int feg_level = 0x40;
    int cutoff = 0;
};

VelPlan vel_plan(const dll::Tables& tb, int vel_pitch, int vel_lpf, int cutoff) {
    VelPlan p;
    p.cutoff = cutoff;
    const int sp = vel_pitch - 0x40;
    if (sp) {
        const double slope = (peg_factor(127) - peg_factor(0)) / 127;
        std::int64_t x = py_round(kVelPitchCents * sp / slope / kPegCentsPerStep);
        x = std::max<std::int64_t>(-64, std::min<std::int64_t>(63, x));
        if (x) {
            p.peg_level = static_cast<int>(0x40 + (x > 0 ? x - 1 : x));
            p.cents = py_round(static_cast<double>(-x) * kPegCentsPerStep * peg_factor(kVelPitchRef));
        }
    }
    const int sl = vel_lpf - 0x40;
    if (sl) {
        const double slope = (feg_factor(tb, 127) - feg_factor(tb, 64)) / 63;
        std::int64_t y = py_round(kVelLpfUnits * sl / slope / kFegUnitsPerStep);
        y = std::max<std::int64_t>(0, std::min<std::int64_t>(63, y));
        if (y) {
            p.feg_level = static_cast<int>(0x40 + y);
            const double base = tb.kCutoff[cutoff] - static_cast<double>(y * kFegUnitsPerStep) * feg_factor(tb, kVelLpfRef);
            int best = 0;
            double bk = 0;
            for (int c = 0; c < 128; ++c) {
                const double k = std::fabs(tb.kCutoff[c] - base);
                if (c == 0 || k < bk) {
                    best = c;
                    bk = k;
                }
            }
            p.cutoff = best;
        }
    }
    return p;
}

std::optional<std::array<int, 3>> amp_eg(const Bytes& d) {
    int x = d[13];
    if (x >= 0x60 && d[14] == d[15]) x = 0x5E;
    if (x >= 0x60) return std::nullopt;
    const int R = 2 * (x >> 1) + 0x21;
    const int attack = R >= 0x7F ? 0x3F : std::max(1, std::min(0x3E, (R + 1) / 2));
    const int decay1 = std::max(1, std::min(0x3F, (d[14] + 1) / 2));
    const int decay2 = d[15] >> 1;
    return std::array<int, 3>{attack, decay1, decay2};
}

int boost_level(double db) {
    if (db <= 0) return 0;
    return static_cast<int>(std::max<std::int64_t>(
        1, std::min<std::int64_t>(127, py_round(127 * std::pow((std::pow(10.0, db / 20) - 1) / kBoostK, 0.5)))));
}

Bytes make_element(int wave_index, int note, const Bytes& d, const VelPlan& vp, const std::array<int, 3>& eg) {
    Bytes e(78, 0);
    put_byte(e, 0, wave_index);
    e[1] = 0;
    e[2] = 0x7F;
    e[3] = 1;
    e[4] = 0x7F;
    e[7] = 0x20;
    put_byte(e, 13, note);
    e[14] = 0x40;
    e[15] = 0;
    e[16] = 0x40;
    e[17] = 0;
    e[18] = static_cast<std::uint8_t>(vp.peg_level != 0x40 ? 0x40 + kSense : 0x40);
    e[19] = e[20] = 0x40;
    const std::uint8_t eg5[5] = {0x3C, 0x3F, 0x3F, 0x3F, 0x3F};
    std::copy(eg5, eg5 + 5, e.begin() + 21);
    for (int i = 26; i < 31; ++i) put_byte(e, static_cast<std::size_t>(i), vp.peg_level);
    e[31] = static_cast<std::uint8_t>(d[12] >> 1);
    e[32] = 0;
    put_byte(e, 33, vp.cutoff);
    const std::uint8_t bp[4] = {0x18, 0x30, 0x48, 0x60};
    std::copy(bp, bp + 4, e.begin() + 34);
    for (int i = 38; i < 42; ++i) e[static_cast<std::size_t>(i)] = 0x40;
    e[42] = static_cast<std::uint8_t>(vp.feg_level != 0x40 ? 0x40 + kSense : 0x40);
    e[43] = e[44] = 0x40;
    std::copy(eg5, eg5 + 5, e.begin() + 45);
    for (int i = 50; i < 55; ++i) put_byte(e, static_cast<std::size_t>(i), vp.feg_level);
    e[55] = 0x7F;
    std::copy(bp, bp + 4, e.begin() + 56);
    for (int i = 60; i < 64; ++i) e[static_cast<std::size_t>(i)] = 0x40;
    e[64] = 1;
    e[65] = 7;
    e[66] = 0x40;
    e[67] = 0x3C;
    e[68] = 0;
    put_byte(e, 69, eg[0]);
    put_byte(e, 70, eg[1]);
    put_byte(e, 71, eg[2]);
    e[72] = 0x28;
    e[73] = 0x7D;
    e[74] = 0x00;
    e[77] = 0x40;
    return e;
}

struct WKey {
    SKey addr;
    std::int64_t neg, pos, q, r;
    bool operator==(const WKey&) const = default;
    bool operator<(const WKey& o) const {
        return std::tie(addr, neg, pos, q, r) < std::tie(o.addr, o.neg, o.pos, o.q, o.r);
    }
};

struct Writer {
    const dll::Tables& tb;
    Table& t;
    Table& alt;
    SampleMonster monster;
    Gen drumkit_idx{0, 257};
    Gen wave_idx[3] = {{0, 257}, {256, 513}, {512, 769}};
    Gen extvoice_idx{0, 0xFF00};

    Writer(const dll::Tables& tables, Table& table, Table& alt_table, std::span<const std::uint8_t> waverom)
        : tb(tables), t(table), alt(alt_table), monster(waverom, true) {}

    void feed(const SampleP& s) {
        if (s->written) return;
        const auto [addr, fmt] = monster.feed(s);
        s->out_loop_address = addr;
        s->out_sample_type = fmt;
        s->written = true;
    }

    std::vector<SampleP> samples_of(const WaveBank& wb) {
        std::vector<SampleP> out;
        for (const Wave& w : wb.waves) out.push_back(t.samples.at(w.loop_address_src));
        return out;
    }

    void convert_voice(Voice& v) {
        for (const Element& e : v.elements) {
            WaveBank& wb = *t.wavebanks.at(e.wavebank_address);
            for (const Wave& w : wb.waves) feed(t.samples.at(w.loop_address_src));
            wb.out_data = write_wavedata(wb, samples_of(wb));
            const int page = v.page;
            if (wb.index[static_cast<std::size_t>(page)] < 0) wb.index[static_cast<std::size_t>(page)] = wave_idx[page].next();
        }
        // fromMU1000.ConvertElements
        std::vector<int> idx;
        for (const Element& e : v.elements)
            idx.push_back(t.wavebanks.at(e.wavebank_address)->index[static_cast<std::size_t>(v.page)] - 256 * v.page);
        for (std::size_t i = 0; i < v.elements.size(); ++i) {
            Element& e = v.elements[i];
            check(e.data.size() == 84, "element size");
            check(0 <= idx[i] && idx[i] < 256, "wave number");
            const Bytes& s = e.data;
            Bytes nd(78, 0);
            nd[0] = static_cast<std::uint8_t>(idx[i]);
            std::copy(s.begin() + 2, s.begin() + 6, nd.begin() + 1);
            put_byte(nd, 5, s[7] | (s[6] ? 0x80 : 0));
            std::copy(s.begin() + 8, s.begin() + 80, nd.begin() + 6);
            e.data = std::move(nd);
        }
        v.converted = true;
        for (Element& e : v.elements)  // Rising_Decay2_Fix
            if (e.data[73] < 64 && e.data[74] > e.data[73]) e.data[73] = 64;
        v.data = write_voice(v);
    }

    void convert_drum_voice_entry(DrumVoice& dv) {
        Voice* ext = nullptr;
        Sample* sample = nullptr;
        if (!dv.ext_voice.empty()) {
            Voice& v = *t.voices.at(dv.ext_voice);
            v.extvoice_index = extvoice_idx.next();
            dv.extvoice_index = v.extvoice_index;
            if (!v.converted) convert_voice(v);
            ext = &v;
        } else {
            const SampleP& s = t.samples.at(dv.sample_hash);
            feed(s);
            sample = s.get();
        }
        if (ext != nullptr) {
            convert_drum_voice(dv, nullptr, ext);
        } else {
            dv.offset_negative += sample->lead_in;
            convert_drum_voice(dv, sample, nullptr);
        }
        to_big_drum_voice(dv, sample, ext);
        dv.converted = true;
    }

    void drumvel(int page, int free_ms) {
        struct Job {
            DrumVoice* dv;
            Bytes d;
            VelPlan p;
            std::array<int, 3> eg;
            WKey wkey;
            SampleP sample;
            int boost;
        };
        std::vector<Job> jobs;
        t.drumvoices.each([&](const std::int64_t&, DrumVoiceP& dvp) {
            DrumVoice& dv = *dvp;
            if (!dv.ext_voice.empty()) return;
            const int vp = dv.vel_pitch, vl = dv.vel_lpf;
            const int boost = dv.fast_decay ? 0 : boost_level(dv.level_boost);
            if (vp == 0x40 && vl == 0x40 && !boost) return;
            const Bytes& d = dv.data;
            const auto eg = amp_eg(d);
            if (!eg) return;
            const VelPlan p = vel_plan(tb, vp, vl, d[11]);
            const int root = d[10];
            const int semis = 0x40 + s8(d[29]) - root;
            const std::int64_t total = semis * 100 + (d[1] - 0x40) + p.cents;
            const std::int64_t q = py_round(static_cast<double>(total) / 100);
            const SampleP& sample = t.samples.at(dv.sample_hash);
            const WKey wkey{sample->address_src, dv.offset_negative - sample->lead_in, dv.offset_positive, q, total - 100 * q};
            jobs.push_back({&dv, d, p, *eg, wkey, sample, boost});
        });
        if (jobs.empty()) return;
        std::set<WKey> uniq;
        for (const Job& j : jobs) uniq.insert(j.wkey);
        const auto nwaves = static_cast<std::int64_t>(uniq.size());
        int R = 12;
        while (R > 0 && ceildiv(nwaves, 128 / (2 * R + 1)) > free_ms) --R;
        const int B = 2 * R + 1, nblocks = 128 / B;
        std::vector<std::map<int, std::pair<WKey, SampleP>>> packs;
        std::map<WKey, std::pair<std::size_t, int>> where;
        for (const Job& j : jobs) {
            if (where.count(j.wkey)) continue;
            const std::int64_t q = j.wkey.q;
            std::vector<int> ok;
            for (int b = 0; b < nblocks; ++b)
                if (0 <= b * B + R - q && b * B + R - q <= 127) ok.push_back(b);
            check(!ok.empty(), "drum pitch out of range");
            for (std::size_t i = 0; i <= packs.size(); ++i) {
                std::map<int, std::pair<WKey, SampleP>> empty;
                auto& pk = i < packs.size() ? packs[i] : empty;
                std::vector<int> fr;
                for (const int b : ok)
                    if (!pk.count(b)) fr.push_back(b);
                if (fr.empty()) continue;
                int best = fr[0];
                for (const int b : fr)
                    if (std::abs(b * B + R - q - 64) < std::abs(best * B + R - q - 64)) best = b;
                if (i == packs.size()) packs.push_back(empty);
                packs[i][best] = {j.wkey, j.sample};
                where[j.wkey] = {i, best * B + R};
                break;
            }
        }
        std::vector<WaveBankP> wbs;
        for (std::size_t i = 0; i < packs.size(); ++i) {
            const auto& pk = packs[i];
            auto wb = std::make_shared<WaveBank>();
            wb->address_src = "drumvel" + std::to_string(i);
            std::vector<SampleP> samples;
            std::vector<int> used;
            for (const auto& [b, v] : pk) used.push_back(b);
            for (int n = 0; n < 128; ++n) {
                const int j = std::min(n / B, nblocks - 1);
                int src = j;
                if (!pk.count(j)) {
                    src = used[0];
                    for (const int u : used)
                        if (std::abs(u - j) < std::abs(src - j)) src = u;
                }
                const auto& [wk, sample] = pk.at(src);
                const std::int64_t tune = src * B + R - wk.q;
                check(0 <= tune && tune <= 127, "drum tune out of range");
                Wave w;
                w.offset_negative = wk.neg;
                w.offset_positive = wk.pos;
                w.loop_address_src = wk.addr;
                w.attenuation = 0;
                w.tune_note = static_cast<int>(tune);
                w.tune_cent = static_cast<int>(wk.r & 0xFF);
                w.key_min = n;
                w.key_max = n < 127 ? n : 0x7F;
                wb->waves.push_back(w);
                samples.push_back(sample);
            }
            const int idx = wave_idx[page].try_next().value_or(999);
            if (idx >= 256 * (page + 1)) {
                for (const auto& w : wbs) t.wavebanks.erase(w->address_src);
                return;
            }
            wb->index[static_cast<std::size_t>(page)] = idx;
            wb->out_data = write_wavedata(*wb, samples);
            t.wavebanks.set(wb->address_src, wb);
            wbs.push_back(wb);
        }
        std::map<Bytes, VoiceP> voices;
        for (Job& j : jobs) {
            const auto [pi, n] = where.at(j.wkey);
            const WaveBankP& wb = wbs[pi];
            const Bytes edata = make_element(wb->index[static_cast<std::size_t>(page)] - 256 * page, n, j.d, j.p, j.eg);
            std::vector<Bytes> edatas{edata};
            if (j.boost) {
                Bytes e2 = edata;
                e2[55] = static_cast<std::uint8_t>(j.boost);
                edatas.push_back(e2);
            }
            Bytes key;
            for (const Bytes& e : edatas) key.insert(key.end(), e.begin(), e.end());
            VoiceP voice;
            if (const auto it = voices.find(key); it != voices.end()) voice = it->second;
            if (!voice) {
                voice = std::make_shared<Voice>();
                voice->address_src = "drumvel_v" + std::to_string(voices.size());
                voice->volume = 0x7F;
                voice->name = "DrumVel";
                for (const Bytes& e : edatas) voice->elements.push_back({wb->address_src, e});
                voice->converted = true;
                voice->page = page;
                voice->data = write_voice(*voice);
                voice->extvoice_index = extvoice_idx.next();
                t.voices.set(voice->address_src, voice);
                voices.emplace(key, voice);
            }
            DrumVoice& dv = *j.dv;
            dv.ext_voice = voice->address_src;
            dv.extvoice_index = voice->extvoice_index;
            Bytes out = j.d;
            put_le(out, 16, voice->extvoice_index, 2);
            for (int i = 18; i < 30; ++i) out[static_cast<std::size_t>(i)] = 0;
            dv.data = std::move(out);
        }
    }

    Tables run(const std::string& version, const std::string& wave_name);
};

/// makeSYXG50.Merge_Alt_Table.
void merge_alt_table(Table& t, Table& alt) {
    alt.voices.each([&](const std::string& k, const VoiceP& v) {
        if (!t.voices.contains(k)) t.voices.set(k, v);
    });
    alt.wavebanks.each([&](const std::string& k, const WaveBankP& v) {
        if (!t.wavebanks.contains(k)) t.wavebanks.set(k, v);
    });
    alt.samples.each([&](const SKey& k, const SampleP& v) {
        if (!t.samples.contains(k)) t.samples.set(k, v);
    });
    alt.drumvoices.each([&](const std::int64_t& k, const DrumVoiceP& v) {
        if (!t.drumvoices.contains(k)) t.drumvoices.set(k, v);
    });
    alt.drumkits.each([&](const std::int64_t& k, const DrumKitP&) {
        check(t.drumkits.contains(k), "second voice map uses drum kits the first one does not have");
    });
    alt.voice_banks.each([&](const std::int64_t& k, const VoiceBankP& vb) {
        if (t.voice_banks.contains(k)) return;
        auto nb = std::make_shared<VoiceBank>(*vb);
        nb->alt = true;
        t.voice_banks.set(k, nb);
    });
}

int drum_loc(BankKind b) {
    switch (b) {
    case BankKind::GsDrums: return 0;
    case BankKind::XgDrums: return 1;
    case BankKind::SfxDrums: return 2;
    case BankKind::Gm2Drums: return 3;
    default: HVYXG_THROW(ConvertError("not a drum bank"));
    }
}

/// decSYXG50.SYXG50().bankorder_voice: GS, SFX, XG, GM2.
std::optional<int> map_row(BankKind b) {
    switch (b) {
    case BankKind::Gs: return 0;
    case BankKind::Sfx: return 1;
    case BankKind::Xg: return 2;
    case BankKind::Gm2: return 3;
    default: return std::nullopt;
    }
}
int map_row_at(BankKind b) {
    const auto r = map_row(b);
    check(r.has_value(), "bank not in the bank map order");
    return *r;
}

Tables Writer::run(const std::string& version, const std::string& wave_name) {
    merge_alt_table(t, alt);
    bool gm2_present = false;
    t.voice_banks.each([&](const std::int64_t&, const VoiceBankP& v) { gm2_present = gm2_present || v->bank == BankKind::Gm2; });
    check(gm2_present, "models without GM2 voices are not supported");

    t.drumvoices.each([&](const std::int64_t&, const DrumVoiceP& dv) {
        if (dv->ext_voice.empty()) t.samples.at(dv->sample_hash)->used_by_drum = true;
    });

    // Multisample pages (big layout).
    bool paging = false;
    std::array<std::set<std::string>, 3> pages;
    std::set<std::string> used_wb;
    t.voices.each([&](const std::string&, const VoiceP& v) {
        for (const Element& e : v->elements) used_wb.insert(e.wavebank_address);
    });
    if (used_wb.size() > 256) {
        paging = true;
        t.voices.each([&](const std::string&, const VoiceP& v) {
            std::set<std::string> wbs;
            for (const Element& e : v->elements) wbs.insert(e.wavebank_address);
            int pg = 3;
            for (int p = 0; p < 3; ++p) {
                std::set<std::string> u = pages[static_cast<std::size_t>(p)];
                u.insert(wbs.begin(), wbs.end());
                if (u.size() <= 256) {
                    pg = p;
                    break;
                }
            }
            check(pg <= 2, "more than 768 multisamples needed");
            v->page = pg;
            pages[static_cast<std::size_t>(pg)].insert(wbs.begin(), wbs.end());
        });
    }

    t.voices.each([&](const std::string&, const VoiceP& v) {
        if (!v->converted) convert_voice(*v);
    });
    t.drumvoices.each([&](const std::int64_t&, const DrumVoiceP& dv) {
        if (!dv->converted) convert_drum_voice_entry(*dv);
    });
    {
        const int dv_page = paging ? ((!pages[2].empty() || pages[1].size() > 256 - 16) ? 2 : 1) : 0;
        int free_ms;
        if (paging) {
            free_ms = 256 - static_cast<int>(pages[static_cast<std::size_t>(dv_page)].size());
        } else {
            int n = 0;
            t.wavebanks.each([&](const std::string&, const WaveBankP& wb) { n += wb->index[0] >= 0; });
            free_ms = 256 - n;
        }
        drumvel(dv_page, free_ms);
    }

    // Drum voices, voices (ext drum voices first; paging: page 2 at the end of bank B).
    Bytes drum_voices, bank_a, bank_b, wavedata;
    t.drumvoices.each([&](const std::int64_t&, const DrumVoiceP& dv) {
        check(dv->converted, "drum voice not converted");
        dv->offset = static_cast<std::int64_t>(drum_voices.size());
        drum_voices.insert(drum_voices.end(), dv->data.begin(), dv->data.end());
    });
    std::vector<VoiceP> ordered;
    t.voices.each([&](const std::string&, const VoiceP& v) {
        if (v->extvoice_index >= 0) ordered.push_back(v);
    });
    t.voices.each([&](const std::string&, const VoiceP& v) {
        if (v->extvoice_index < 0) ordered.push_back(v);
    });
    if (paging)
        std::stable_sort(ordered.begin(), ordered.end(), [](const VoiceP& a, const VoiceP& b) {
            return (a->page == 2 ? 1 : 0) < (b->page == 2 ? 1 : 0);
        });
    std::int64_t page2_start = -1;
    for (const VoiceP& vp : ordered) {
        Voice& v = *vp;
        check(v.converted, "voice not converted");
        std::string label = v.name.substr(0, 8);
        label.resize(8, ' ');
        const bool to_a = paging ? v.page == 0
                                 : static_cast<std::int64_t>(bank_a.size()) < 0xFFFF - static_cast<std::int64_t>(8 + voice_len(v));
        Bytes& bank = to_a ? bank_a : bank_b;
        if (!to_a && paging && v.page == 2 && page2_start < 0) page2_start = static_cast<std::int64_t>(bank_b.size());
        v.offset = static_cast<std::int64_t>(8 + bank.size());
        check(to_a || v.offset <= 0xFFFF || paging, "voice bank B out of space");
        bank.insert(bank.end(), label.begin(), label.end());
        bank.insert(bank.end(), v.data.begin(), v.data.end());
        v.voicebank = to_a ? 1 : 2;
    }

    for (const std::string& k : t.wavebanks.keys()) {
        const WaveBankP& wb = t.wavebanks.at(k);
        if (wb->index[0] < 0 && wb->index[1] < 0 && wb->index[2] < 0) t.wavebanks.erase(k);
    }
    {
        std::vector<WaveBankP> wbs = t.wavebanks.values();
        const auto first_index = [](const WaveBankP& wb) {
            int m = 1 << 30;
            for (const int i : wb->index)
                if (i >= 0) m = std::min(m, i);
            return m;
        };
        std::stable_sort(wbs.begin(), wbs.end(), [&](const WaveBankP& a, const WaveBankP& b) { return first_index(a) < first_index(b); });
        for (const WaveBankP& wb : wbs) {
            wb->offset = static_cast<std::int64_t>(wavedata.size());
            wavedata.insert(wavedata.end(), wb->out_data.begin(), wb->out_data.end());
        }
    }

    // Drum program maps and kit key maps.
    std::vector<DrumKitP> kits = t.drumkits.values();
    Bytes drum_prg(128 * 4, 0);
    for (const DrumKitP& kit : kits) {
        kit->index = drumkit_idx.next();
        drum_prg[static_cast<std::size_t>(kit->prg + 128 * drum_loc(kit->bank))] = static_cast<std::uint8_t>(kit->index);
    }
    for (const DrumKitP& kit : kits)
        for (const auto& [bank, prg] : kit->aliases) put_byte(drum_prg, static_cast<std::size_t>(prg + drum_loc(bank) * 128), kit->index);
    const int highest_kit = std::max(0, drumkit_idx.next() - 1);
    std::stable_sort(kits.begin(), kits.end(), [](const DrumKitP& a, const DrumKitP& b) { return a->index < b->index; });
    Bytes keymaps(static_cast<std::size_t>(256 * (highest_kit + 1)), 0xFF);
    for (std::size_t i = 0; i < kits.size(); ++i)
        kits[i]->drumvoices.each([&](const int& key, const std::int64_t& addr) {
            put_le(keymaps, 256 * i + static_cast<std::size_t>(key) * 2, t.drumvoices.at(addr)->offset, 2);
        });

    const auto voice_byte_offset = [&](const Voice& v) {
        check(v.offset >= 0, "voice not placed");
        return v.voicebank == 2 ? static_cast<std::int64_t>(bank_a.size()) + v.offset : v.offset;
    };
    const int highest_ext = std::max(0, extvoice_idx.next() - 1);
    Bytes ext_offsets(static_cast<std::size_t>((highest_ext + 1) * 4), 0);
    {
        std::map<int, const DrumVoice*> by_idx;  // the last drum voice with that index wins
        t.drumvoices.each([&](const std::int64_t&, const DrumVoiceP& dv) {
            if (dv->extvoice_index >= 0) by_idx[dv->extvoice_index] = dv.get();
        });
        for (int idx = 0; idx <= highest_ext; ++idx) {
            const auto it = by_idx.find(idx);
            if (it == by_idx.end()) continue;
            check(!it->second->ext_voice.empty(), "ext drum voice without voice");
            put_le(ext_offsets, static_cast<std::size_t>(idx) * 4, voice_byte_offset(*t.voices.at(it->second->ext_voice)), 4);
        }
    }

    // Voice bank maps (GS / XG sides apart, as the S-YXG50 requires).
    Gen banks_gs{0, 257}, banks_xg{0, 257};
    Bytes bankmaps(128 * 4, 0xFF);
    for (std::size_t i = 128; i < bankmaps.size(); ++i) bankmaps[i] = 0;
    ODict<std::int64_t, VoiceBankP> gs_banks, xg_banks;
    t.voice_banks.each([&](const std::int64_t&, const VoiceBankP& vb) {
        if (vb->bank == BankKind::Gs) {
            gs_banks.set(vb->prg_address, vb);
            for (const auto& [db, byte] : std::vector<std::pair<BankKind, int>>(vb->aliases)) {
                if (db == BankKind::Gs) continue;
                const auto [l, m] = bank_lsb_msb(db, byte);
                if (VoiceBankP* x = xg_banks.find(vb->prg_address)) {
                    (*x)->aliases.push_back({db, byte});
                } else {
                    auto nb = std::make_shared<VoiceBank>();
                    nb->prg_address = vb->prg_address;
                    nb->bank = db;
                    nb->lsb = l;
                    nb->msb = m;
                    nb->voices = vb->voices;
                    nb->alt = vb->alt;
                    xg_banks.set(vb->prg_address, nb);
                }
            }
        } else {
            xg_banks.set(vb->prg_address, vb);
            for (const auto& [db, byte] : std::vector<std::pair<BankKind, int>>(vb->aliases)) {
                if (db != BankKind::Gs) continue;
                if (VoiceBankP* g = gs_banks.find(vb->prg_address)) {
                    (*g)->aliases.push_back({db, byte});
                } else {
                    auto nb = std::make_shared<VoiceBank>();
                    nb->prg_address = vb->prg_address;
                    nb->bank = db;
                    nb->lsb = 0;
                    nb->msb = byte;
                    nb->voices = vb->voices;
                    nb->alt = vb->alt;
                    gs_banks.set(vb->prg_address, nb);
                }
            }
        }
    });
    {
        bool done = false;
        xg_banks.each([&](const std::int64_t&, const VoiceBankP& x) {
            if (!done && x->bank == BankKind::Sfx && x->msb == 1 && !x->alt) {
                x->index = banks_xg.next();
                done = true;
            }
        });
        done = false;
        xg_banks.each([&](const std::int64_t&, const VoiceBankP& x) {
            if (!done && x->bank == BankKind::Xg && x->lsb == 0 && !x->alt) {
                x->index = banks_xg.next();
                done = true;
            }
        });
    }
    gs_banks.each([&](const std::int64_t&, const VoiceBankP& g) {
        check(g->bank == BankKind::Gs, "GS side");
        g->index = banks_gs.next();
    });
    xg_banks.each([&](const std::int64_t&, const VoiceBankP& x) {
        check(x->bank != BankKind::Gs, "XG side");
        if (x->index < 0) x->index = banks_xg.next();
    });
    std::vector<VoiceBankP> expanded = gs_banks.values();
    for (const VoiceBankP& x : xg_banks.values()) expanded.push_back(x);
    Bytes msb48_row(128, 0xFF);
    for (const VoiceBankP& vb : expanded) {
        std::vector<std::pair<BankKind, int>> all{{vb->bank, vb->relevant_byte()}};
        all.insert(all.end(), vb->aliases.begin(), vb->aliases.end());
        for (const auto& [b, byte] : all)
            if (b == BankKind::Msb48 && vb->bank != BankKind::Gs) put_byte(msb48_row, static_cast<std::size_t>(byte), vb->index);
        check(vb->bank == BankKind::Msb48 || map_row(vb->bank).has_value(), "bank not in the bank map order");
        if (vb->alt) continue;
        if (vb->bank != BankKind::Msb48)
            put_byte(bankmaps, static_cast<std::size_t>(vb->relevant_byte() + 128 * map_row_at(vb->bank)), vb->index);
        for (const auto& [b, byte] : vb->aliases) {
            if ((vb->bank == BankKind::Gs) != (b == BankKind::Gs)) continue;
            if (b == BankKind::Msb48) continue;
            put_byte(bankmaps, static_cast<std::size_t>(byte + 128 * map_row_at(b)), vb->index);
        }
    }
    const int highest_gs = std::max(0, banks_gs.next() - 1);
    const int highest_xg = std::max(0, banks_xg.next() - 1);
    Bytes prg_gs(static_cast<std::size_t>((highest_gs + 1) * 128 * 4), 0);
    Bytes prg_xg(static_cast<std::size_t>((highest_xg + 1) * 128 * 4), 0);
    for (const VoiceBankP& vb : expanded) {
        Bytes& m = vb->bank == BankKind::Gs ? prg_gs : prg_xg;
        const std::size_t base = static_cast<std::size_t>(vb->index) * 128 * 4;
        for (int prg = 0; prg < 128; ++prg) {
            const Voice& v = *t.voices.at(vb->voices->at(prg));
            check(base + static_cast<std::size_t>(prg) * 4 + 4 <= m.size(), "program map index");
            put_le(m, base + static_cast<std::size_t>(prg) * 4, voice_byte_offset(v), 4);
        }
    }
    // GM2 drum programs (MSB 120) select the kits like GS.
    std::copy(drum_prg.begin(), drum_prg.begin() + 128, drum_prg.begin() + 384);

    // Wave data offsets.
    int highest_wave = -1;
    t.wavebanks.each([&](const std::string&, const WaveBankP& wb) {
        for (const int i : wb->index) highest_wave = std::max(highest_wave, i);
    });
    if (!paging) ++highest_wave;
    if (page2_start >= 0) {
        check(highest_wave >= 512, "page 2 without 512 multisamples");
        highest_wave = 768;
    }
    check(!paging || highest_wave >= 256, "paging without 256 multisamples");
    Bytes wd_offsets(static_cast<std::size_t>((highest_wave + 1) * 4), 0);
    t.wavebanks.each([&](const std::string&, const WaveBankP& wb) {
        check(wb->offset >= 0, "wave bank not placed");
        for (const int i : wb->index) {
            if (i < 0) continue;
            check(i < 768, "multisample index");
            put_le(wd_offsets, static_cast<std::size_t>(i) * 4, wb->offset, 4);
        }
    });
    if (page2_start >= 0) put_le(wd_offsets, 768 * 4, page2_start, 4);

    // Header.
    Bytes header(100, 0);
    for (std::size_t i = 0; i < version.size() && i < 16; ++i) header[i] = static_cast<std::uint8_t>(version[i]);
    for (std::size_t i = 0; i < wave_name.size() && 16 + i < header.size(); ++i) header[16 + i] = static_cast<std::uint8_t>(wave_name[i]);
    header[0x1F] = 0x01;
    const auto len = [&](std::size_t at, std::size_t v) { put_le(header, at, static_cast<std::int64_t>(v), 4); };
    for (const std::size_t at : {0x20u, 0x24u, 0x28u, 0x2Cu}) len(at, 128);
    len(0x30, keymaps.size());
    len(0x34, drum_voices.size());
    len(0x38, ext_offsets.size());
    for (const std::size_t at : {0x3Cu, 0x40u, 0x44u, 0x48u}) len(at, 128);
    len(0x4C, prg_gs.size());
    len(0x50, prg_xg.size());
    len(0x54, bank_a.size());
    len(0x58, bank_b.size());
    len(0x5C, wd_offsets.size());
    len(0x60, wavedata.size());

    Tables out;
    Bytes& f = out.tbl;
    for (const Bytes* b : {&header, &drum_prg, &keymaps, &drum_voices, &ext_offsets, &bankmaps, &prg_gs, &prg_xg, &bank_a,
                           &bank_b, &wd_offsets, &wavedata})
        f.insert(f.end(), b->begin(), b->end());

    // MAP2 appendix: the second (MU Basic) voice map's rows with this table's indices.
    {
        Bytes rows(128 * 4, 0);
        std::fill(rows.begin(), rows.begin() + 128, 0xFF);
        alt.voice_banks.each([&](const std::int64_t&, const VoiceBankP& vb) {
            std::vector<std::pair<BankKind, int>> all{{vb->bank, vb->relevant_byte()}};
            all.insert(all.end(), vb->aliases.begin(), vb->aliases.end());
            for (const auto& [b, byte] : all) {
                const auto row = map_row(b);
                if (!row) continue;
                const VoiceBankP* dst = (b == BankKind::Gs ? gs_banks : xg_banks).find(vb->prg_address);
                check(dst != nullptr && (*dst)->index >= 0, "second map bank without an index");
                put_byte(rows, static_cast<std::size_t>(byte + 128 * *row), (*dst)->index);
            }
        });
        std::optional<std::int64_t> lsb0;
        alt.voice_banks.each([&](const std::int64_t&, const VoiceBankP& vb) {
            if (!lsb0 && vb->bank == BankKind::Xg && vb->lsb == 0) lsb0 = vb->prg_address;
        });
        if (lsb0) {
            const int i0 = xg_banks.at(*lsb0)->index;
            const std::size_t msb_row = 128 * static_cast<std::size_t>(map_row_at(BankKind::Sfx));
            for (std::size_t i = msb_row; i < msb_row + 128; ++i)
                if (rows[i] == i0) rows[i] = 1;
        }
        Bytes drows(128 * 4, 0);
        alt.drumkits.each([&](const std::int64_t& key, const DrumKitP& kit) {
            const int idx = t.drumkits.at(key)->index;
            std::vector<std::pair<BankKind, int>> all{{kit->bank, kit->prg}};
            all.insert(all.end(), kit->aliases.begin(), kit->aliases.end());
            for (const auto& [b, prg] : all) put_byte(drows, static_cast<std::size_t>(prg + 128 * drum_loc(b)), idx);
        });
        bool has48 = false;
        for (const auto b : msb48_row) has48 = has48 || b != 0xFF;
        Bytes app{'M', 'A', 'P', '2', 0, static_cast<std::uint8_t>(has48 ? 1 : 0), 0, 0};
        app.insert(app.end(), drows.begin(), drows.end());
        app.insert(app.end(), rows.begin(), rows.end());
        app.insert(app.end(), msb48_row.begin(), msb48_row.end());
        check(app.size() == 8 + 9 * 128, "appendix size");
        // The GM2 drum program row is the same in both maps.
        std::copy(drum_prg.begin() + 384, drum_prg.begin() + 512, app.begin() + 8 + 384);
        f.insert(f.end(), app.begin(), app.end());
    }

    out.upcm = monster.generate();
    return out;
}

} // namespace

Tables make_syxg50(const dll::Tables& tb, Table& table, Table& alt, std::span<const std::uint8_t> waverom,
                   const std::string& version, const std::string& wave_name) {
    Writer w(tb, table, alt, waverom);
    return w.run(version, wave_name);
}

} // namespace hvyxg::syxg50::mu_rom
