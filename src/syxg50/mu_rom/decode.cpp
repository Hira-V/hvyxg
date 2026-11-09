// Copyright (C) 2026 Kevin H. Patterson
// SPDX-License-Identifier: BSD-3-Clause (see LICENSE)
// Translated from SXG-Create-NF, Copyright (c) 2026 Soundshock, BSD-3-Clause (THIRD_PARTY.md)
// Port of SXG-Create-NF utility/decMU1000.py, decMU90.py and decBase.py (BSD-3-Clause,
// Copyright 2026 Soundshock). Program-ROM addresses are table locations, not ROM data.
#include "syxg50/mu_rom/decode.hpp"

#include <algorithm>
#include <bit>
#include <cmath>
#include <map>
#include <set>

#include "syxg50/mu_rom/trims.hpp"

namespace hvyxg::syxg50::mu_rom {

namespace {

struct Range32 {
    std::int64_t start, end;
};

struct Layout {
    Range32 wavedata, wdoffs, voices, prgmap;
    std::int64_t bankrows;
    Range32 extoffs;
    std::int64_t gsrow;
    Range32 drumvoices, keymaps;
    std::int64_t drumprg;
    int native_xg_base, silent;
    std::int64_t wavespan;
};

const Layout& layout(Engine e) {
    static constexpr Layout mu1000{{0x164598, 0x16FAE8}, {0x16FAE8, 0x16FAE8 + 503 * 2}, {0x16FED8, 0x1ABEC6},
                               {0x1ABEC8, 0x1C78C8}, 0x1C78C8, {0x1C7AC8, 0x1C7AC8 + 97 * 4}, 0x1C7CC8,
                               {0x1C7D48, 0x1D25C6}, {0x1D25C6, 0x1D61C6}, 0x1D62B8, 0x5B, 0x4C, 0x2000000};
    static constexpr Layout mu2000{{0x1F55A0, 0x200AF0}, {0x200AF0, 0x200AF0 + 503 * 2}, {0x200EE0, 0x23CECE},
                               {0x267F50, 0x283950}, 0x283950, {0x283B50, 0x283B50 + 97 * 4}, 0x283D50,
                               {0x283DD0, 0x28E64E}, {0x28E64E, 0x29224E}, 0x292340, 0x5B, 0x4C, 0x2000000};
    static constexpr Layout mu128{{0x0FDB0C, 0x1084BC}, {0x1084BC, 0x108856}, {0x108858, 0x1418B2},
                              {0x1418B4, 0x15C6B4}, 0x15C6B4, {0x15C8B4, 0x15CA2C}, 0x15CAB4,
                              {0x15CB34, 0x166836}, {0x166836, 0x16A236}, 0x16A320, 0x58, 0x49, 0x1800000};
    switch (e) {
    case Engine::Mu2000: return mu2000;
    case Engine::Mu128: return mu128;
    default: return mu1000;
    }
}

constexpr std::size_t kElementLength = 84;
constexpr std::size_t kHeaderLength = 14;
constexpr std::int64_t kMu2000ScalingTables = 0x23CED0;
constexpr SKey kReversedKey = 0x40000000;
constexpr int kHpfKeyShift = 36;
constexpr int kElementHpfMin = 16;
constexpr int kElementHpfSpan = 6;
constexpr double kWaveFineCent = 1200.0 / 1024;

int s8(int b) { return b > 127 ? b - 256 : b; }

double drum_hpf_cutoff(int value) { return std::pow(2.0, 5.066 + 0.0591 * value); }

int rounded_hpf_cutoff(double fc) {
    const double m = 1.0 > fc ? 1.0 : fc;
    return static_cast<int>(std::max<std::int64_t>(1, py_round(std::pow(2.0, static_cast<double>(py_round(std::log2(m) * 6)) / 6))));
}

SampleP hpf_sample(const Sample& sample, double fc_in) {
    const int fc = rounded_hpf_cutoff(18000.0 < fc_in ? 18000.0 : fc_in);
    auto s = std::make_shared<Sample>(sample);
    s->hpf_fc = fc;
    s->address_src = sample.address_src + (static_cast<SKey>(fc + 1) << kHpfKeyShift);
    return s;
}

double static_peg_semitones(const Bytes& e) {
    const int depth = e[19], vel_sens = e[20];
    const int lv = e[28];
    if (vel_sens != 64 || lv == 64) return 0.0;
    for (int i = 28; i < 33; ++i)
        if (e[static_cast<std::size_t>(i)] != lv) return 0.0;
    // 3 * 2 ** depth * (level - 64) / d: an exact integer over d, correctly rounded
    return std::ldexp(static_cast<double>(3 * (lv - 64)), depth) / (lv > 64 ? 63 : 64);
}

void make_reversed(Sample& s) {
    s.reverse = true;
    s.src_offset_negative = s.offset_negative;
    s.src_offset_positive = s.offset_positive;
    s.address_src = s.loop_address + kReversedKey;
    s.offset_negative = s.offset_negative + s.offset_positive;
    s.offset_positive = 0;
}

struct Decoded {
    VoiceP voice;
    WaveBankDict wavebanks;
    SampleDict samples;
};

class Decoder {
public:
    Decoder(Engine engine, const Bytes& image, bool basic, std::span<const std::uint8_t> waverom, elemreduce::EnvIds& ids)
        : L_(layout(engine)), waverom_(waverom), ids_(ids) {
        data_ = image;
        const auto row = [&](std::int64_t a, int r) {
            check(a + 128 * (r + 1) <= static_cast<std::int64_t>(image.size()), "program image too short");
            return Bytes(image.begin() + a + 128 * r, image.begin() + a + 128 * (r + 1));
        };
        const auto append = [&](const Bytes& b) { data_.insert(data_.end(), b.begin(), b.end()); };
        drumprg_start_ = static_cast<std::int64_t>(data_.size());
        append(row(L_.drumprg, 3));
        append(row(L_.drumprg, basic ? 0 : 1));
        append(row(L_.drumprg, 2));
        banks_start_ = static_cast<std::int64_t>(data_.size());
        append(row(L_.bankrows, basic ? 1 : 2));
        Bytes msb = row(L_.bankrows, 0);
        for (auto& v : msb)
            if (v == 0x00 && !basic) v = static_cast<std::uint8_t>(L_.native_xg_base);
        msb[121] = static_cast<std::uint8_t>(L_.silent);
        append(msb);
        append(row(L_.gsrow, 0));
        append(row(L_.drumprg, basic ? 6 : 7));
        append(row(L_.bankrows, 3));
    }

    Table create_table();

private:
    std::int64_t be(std::int64_t at, int bytes) const {
        std::int64_t v = 0;
        for (int i = 0; i < bytes; ++i) {
            const std::int64_t a = at + i;
            if (a >= static_cast<std::int64_t>(data_.size())) break;  // Python slices stop at the end
            v = (v << 8) | data_[static_cast<std::size_t>(a)];
        }
        return v;
    }
    int byte(std::int64_t at) const {
        check(at >= 0 && at < static_cast<std::int64_t>(data_.size()), "ROM read out of range");
        return data_[static_cast<std::size_t>(at)];
    }

    std::pair<WaveBankP, SampleDict> process_wavedata(std::int64_t address) const;
    std::pair<WaveBankP, SampleDict> apply_element_hpf(const WaveBank& wb, const SampleDict& samples, int value,
                                                       const Bytes& element) const;
    std::tuple<int, WaveBankP, SampleDict> load(std::int64_t address, const std::vector<Bytes>& raw, std::size_t k) const;
    Decoded process_voice(std::int64_t address);
    std::tuple<std::int64_t, DrumVoiceP, SampleP> process_drum_voice(std::int64_t address) const;

    const Layout& L_;
    std::span<const std::uint8_t> waverom_;
    elemreduce::EnvIds& ids_;
    Bytes data_;
    std::int64_t drumprg_start_ = 0, banks_start_ = 0;
};

std::pair<WaveBankP, SampleDict> Decoder::process_wavedata(std::int64_t address) const {
    SampleDict samples;
    auto wb = std::make_shared<WaveBank>();
    wb->address_src = to_str(address);
    int last_key = 0;
    for (std::int64_t addr = address; addr < L_.wavedata.end; addr += 16) {
        std::int64_t neg = be(addr + 5, 3), pos = be(addr + 9, 3);
        const std::int64_t loop = be(addr + 13, 3) * 4;
        const int fb = byte(addr + 12);
        const bool backwards = byte(addr + 8) & 0x80;
        auto s = std::make_shared<Sample>();
        s->address_src = loop;
        s->offset_negative = neg;
        s->offset_positive = pos;
        s->loop_address = loop;
        s->sample_type = fmt_from_mu90(fb & 0xC0);
        s->encoding_parameters = (fb & 0x3E) >> 1;
        if (backwards) {
            make_reversed(*s);
            neg = s->offset_negative;
            pos = s->offset_positive;
        }
        add_sample(samples, s);
        Wave w;
        w.address_src = addr;
        w.offset_negative = neg;
        w.offset_positive = pos;
        w.loop_address_src = s->address_src;
        w.attenuation = byte(addr);
        w.tune_note = byte(addr + 1);
        w.tune_cent = byte(addr + 2);
        w.key_min = last_key;
        w.key_max = byte(addr + 3);
        last_key = w.key_max + 1;
        wb->waves.push_back(w);
        if (w.key_max >= 0x7F) break;
    }
    for (Wave& w : wb->waves) {
        int fine = byte(w.address_src + 8) & 0x7F;
        if (fine >= 0x40) fine -= 0x80;
        if (fine == 0) continue;
        std::int64_t cent = (w.tune_cent >= 128 ? w.tune_cent - 256 : w.tune_cent) + py_round(fine * kWaveFineCent);
        int note = w.tune_note;
        while (cent > 127) {
            cent -= 100;
            note -= 1;
        }
        while (cent < -128) {
            cent += 100;
            note += 1;
        }
        check(0 <= note && note <= 0xFF, "wave fine tune: note out of range");
        w.tune_note = note;
        w.tune_cent = static_cast<int>(cent & 0xFF);
    }
    return {wb, std::move(samples)};
}

std::pair<WaveBankP, SampleDict> Decoder::apply_element_hpf(const WaveBank& wb, const SampleDict& samples, int value,
                                                            const Bytes& element) const {
    const double fc_out = drum_hpf_cutoff(value);
    const double peg = static_peg_semitones(element);
    const double critical = 69 + 12 * std::log2(3 * fc_out / 440) - peg;
    const auto lo_k = static_cast<int>(py_round(24 - peg)), hi_k = static_cast<int>(py_round(108 - peg));
    auto out_wb = std::make_shared<WaveBank>();
    SampleDict out;
    for (const Wave& w : wb.waves) {
        int a = std::max(w.key_min, lo_k), b = std::min(w.key_max, hi_k);
        if (b < a) a = b = w.key_min > hi_k ? hi_k : w.key_max;
        struct Piece {
            int k0, k1;
            double ref;
        };
        std::vector<Piece> pieces;
        const int c = std::min(b + 1, std::max(a, static_cast<int>(std::ceil(critical))));
        if (c > a) {
            const auto n = ceildiv(c - a, kElementHpfSpan);
            std::vector<std::int64_t> bounds;
            for (std::int64_t i = 0; i <= n; ++i)
                bounds.push_back(a + py_round(static_cast<double>(i * (c - a)) / static_cast<double>(n)));
            for (std::int64_t i = 0; i < n; ++i)
                pieces.push_back({static_cast<int>(bounds[i]), static_cast<int>(bounds[i + 1] - 1),
                                  static_cast<double>(bounds[i] + bounds[i + 1] - 1) / 2});
        }
        if (c <= b) pieces.push_back({c, b, static_cast<double>(c + 1)});
        for (std::size_t i = 0; i < pieces.size(); ++i) {
            const Piece& p = pieces[i];
            const double ratio = std::pow(2.0, (p.ref + peg - w.tune_note) / 12);
            const SampleP s = hpf_sample(*samples.at(w.loop_address_src), fc_out / ratio);
            add_sample(out, s);
            Wave nw = w;
            nw.key_min = i == 0 ? w.key_min : p.k0;
            nw.key_max = i == pieces.size() - 1 ? w.key_max : p.k1;
            nw.loop_address_src = s->address_src;
            out_wb->waves.push_back(nw);
        }
    }
    out_wb->address_src = wb.address_src + "_hpf" + std::to_string(value);
    return {out_wb, std::move(out)};
}

std::tuple<int, WaveBankP, SampleDict> Decoder::load(std::int64_t, const std::vector<Bytes>& raw, std::size_t k) const {
    const Bytes& ed = raw[k];
    const int id = (ed[0] << 7) + ed[1];
    const std::int64_t off_addr = L_.wdoffs.start + id * 2;
    check(off_addr < L_.wdoffs.end, "wave number out of range");
    const std::int64_t wd = be(off_addr, 2) + L_.wavedata.start;
    check(wd < L_.wavedata.end, "wave data out of range");
    auto [wb, sw] = process_wavedata(wd);
    if (ed[80] >= kElementHpfMin) {
        auto [wb2, sw2] = apply_element_hpf(*wb, sw, ed[80], ed);
        return {id, wb2, std::move(sw2)};
    }
    return {id, wb, std::move(sw)};
}

Decoded Decoder::process_voice(std::int64_t address) {
    namespace er = elemreduce;
    const int mask = byte(address);
    check(1 <= mask && mask <= 15, "voice element mask");
    const auto cnt = static_cast<std::size_t>(std::popcount(static_cast<unsigned>(mask)));
    Decoded d;
    auto voice = std::make_shared<Voice>();
    voice->address_src = to_str(address);
    voice->volume = byte(address + 1);
    voice->name.assign(data_.begin() + address + 2, data_.begin() + address + 10);
    std::vector<Bytes> raw;
    for (std::size_t k = 0; k < cnt; ++k) {
        const auto a = static_cast<std::size_t>(address) + kHeaderLength + kElementLength * k;
        check(a + kElementLength <= data_.size(), "voice out of range");
        raw.emplace_back(data_.begin() + static_cast<std::ptrdiff_t>(a), data_.begin() + static_cast<std::ptrdiff_t>(a + kElementLength));
    }
    WaveBankDict& wavebanks = d.wavebanks;
    SampleDict& samples = d.samples;
    std::vector<Element>& elements = voice->elements;

    std::optional<std::vector<er::Out>> outs;
    std::optional<std::vector<double>> loudness;
    if (cnt > 2) {
        std::vector<double> l(cnt);
        bool none = false;
        for (std::size_t k = 0; k < cnt; ++k) {
            if (raw[k][57] == 0) {
                l[k] = 0.0;
                continue;
            }
            const auto [id, wb_k, s_k] = load(address, raw, k);
            const auto v = er::element_loudness(raw[k], er::wavebank_power(*wb_k, s_k, waverom_));
            if (!v) none = true;
            else l[k] = *v;
        }
        if (!none) loudness = l;
        outs = er::plan(raw, loudness);
    }
    if (!outs) {
        for (std::size_t k = 0; k < std::min<std::size_t>(cnt, 2); ++k) {
            auto [id, wb, sw] = load(address, raw, k);
            wavebanks.set(wb->address_src, wb);
            merge_samples(samples, sw);
            elements.push_back({wb->address_src, raw[k]});
        }
    } else {
        for (er::Out& o : *outs) {
            int primary = er::primary_member(o, raw, loudness);
            if (o.pair_mate && o.members.size() == 1) {
                const int mate = *o.pair_mate;
                const auto power = [&](int i) {
                    const auto [id, wb, sw] = load(address, raw, static_cast<std::size_t>(i));
                    return er::wavebank_power(*wb, sw, waverom_);
                };
                const auto pp = power(primary);
                const auto pm = power(mate);
                const bool all = pp && *pp != 0.0 && pm && *pm != 0.0;
                if (all && er::element_power(raw[static_cast<std::size_t>(mate)], *pm) >
                               er::element_power(raw[static_cast<std::size_t>(primary)], *pp)) {
                    o.members = {{mate, o.members[0].second}};
                    o.pair_mate = primary;
                    primary = mate;
                }
            }
            Bytes edata = raw[static_cast<std::size_t>(primary)];
            int mn = 1 << 30, mx = -(1 << 30);
            for (const auto& m : o.members) {
                mn = std::min(mn, m.second.first);
                mx = std::max(mx, m.second.second);
            }
            edata[2] = static_cast<std::uint8_t>(mn);
            edata[3] = static_cast<std::uint8_t>(mx);
            edata[4] = static_cast<std::uint8_t>(o.vels.first);
            edata[5] = static_cast<std::uint8_t>(o.vels.second);
            if (o.centre) edata[67] = 7;
            std::set<int> member_set;
            for (const auto& m : o.members) member_set.insert(m.first);
            std::map<int, std::tuple<int, WaveBankP, SampleDict>> loaded;
            for (const int i : member_set) loaded.emplace(i, load(address, raw, static_cast<std::size_t>(i)));
            for (auto& [i, t] : loaded) merge_samples(samples, std::get<2>(t));
            WaveBankP wavebank;
            if (o.members.size() == 1) {
                wavebank = std::get<1>(loaded.at(primary));
            } else {
                SampleDict member_samples;
                for (const int i : member_set)
                    std::get<2>(loaded.at(i)).each([&](const SKey& k, const SampleP& v) { member_samples.set(k, v); });
                SampleDict baked;
                std::map<int, WaveBankP> member_wbs;
                for (const auto& m : o.members) member_wbs.emplace(m.first, std::get<1>(loaded.at(m.first)));
                std::set<int> offsets;
                for (const auto& [i, wb] : member_wbs) offsets.insert(raw[static_cast<std::size_t>(i)][77]);
                if (offsets.size() > 1) {
                    for (auto& [i, wb] : member_wbs) wb = er::start_offset_wavebank(wb, raw[static_cast<std::size_t>(i)][77]);
                    edata[77] = 0;
                }
                std::string name = to_str(address) + "_el";
                std::vector<std::tuple<WaveBankP, er::Range, int>> members;
                for (std::size_t n = 0; n < o.members.size(); ++n) {
                    const auto& [i, r] = o.members[n];
                    if (n) name += "_";
                    name += std::to_string(i) + "x" + std::to_string(r.first) + "-" + std::to_string(r.second);
                    members.emplace_back(member_wbs.at(i), r, i);
                }
                auto [wb, top] = er::merge_wavebanks(ids_, members, raw, primary, name, &member_samples, &baked);
                wavebank = wb;
                if (!baked.empty()) merge_samples(samples, baked);
                er::flatten_level(edata, top);
                edata[17] = 0;
            }
            std::vector<int> folded;
            if (o.pair_mate) folded.push_back(*o.pair_mate);
            folded.insert(folded.end(), o.absorbed.begin(), o.absorbed.end());
            if (!folded.empty()) {
                const bool merged = o.members.size() > 1;
                double lv = edata[57] + (merged ? er::kKsLevelSteps * (edata[62] - 64) : 0);
                const bool lv_int = !merged;
                std::map<int, std::optional<double>> powers;
                std::vector<int> all_i{primary};
                all_i.insert(all_i.end(), folded.begin(), folded.end());
                if (!merged) {
                    for (const int i : all_i) {
                        std::optional<double> p;
                        if (const auto it = loaded.find(i); it != loaded.end())
                            p = er::wavebank_power(*std::get<1>(it->second), std::get<2>(it->second), waverom_);
                        else {
                            const auto [id, wb, sw] = load(address, raw, static_cast<std::size_t>(i));
                            p = er::wavebank_power(*wb, sw, waverom_);
                        }
                        powers[i] = p;
                    }
                }
                bool all = !powers.empty();
                for (const auto& [i, p] : powers)
                    if (!p || *p == 0.0) all = false;
                if (all && edata[57]) {
                    std::vector<double> parts;
                    for (const int i : all_i) parts.push_back(er::element_power(raw[static_cast<std::size_t>(i)], *powers.at(i)));
                    const double target = py_sum(parts);
                    const double kept = std::pow(edata[57] / 128.0, 4.0) * *powers.at(primary) * er::pan_power(edata[67]);
                    lv = er::matched_level(kept, target, edata[57]);
                } else {
                    std::vector<int> extra;
                    for (const int i : o.absorbed) extra.push_back(raw[static_cast<std::size_t>(i)][57]);
                    lv = er::absorb_level(lv, lv_int, extra);
                }
                er::flatten_level(edata, py_round(lv), !merged);
            }
            wavebanks.set(wavebank->address_src, wavebank);
            elements.push_back({wavebank->address_src, std::move(edata)});
        }
    }
    // Measured level trim of this voice (voicetrim.py).
    const std::span<const std::uint8_t> rawv(data_.data() + address, kHeaderLength + kElementLength * cnt);
    const std::uint32_t key = voice_key(rawv);
    std::vector<int> levels;
    for (const Element& el : elements) levels.push_back(el.data[57]);
    const auto [nl, boost] = voice_trim_levels(key, levels);
    for (std::size_t i = 0; i < elements.size() && i < nl.size(); ++i) {
        elements[i].data[57] = static_cast<std::uint8_t>(nl[i]);
        voice_key_trim(key, elements[i].data);
    }
    if (boost != 0.0) {
        for (Element& el : elements) {
            auto [wb, news] = er::gain_wavebank(ids_, *wavebanks.at(el.wavebank_address), samples, boost);
            wavebanks.set(wb->address_src, wb);
            merge_samples(samples, news);
            el.wavebank_address = wb->address_src;
        }
        WaveBankDict only;
        for (const Element& el : elements) only.set(el.wavebank_address, wavebanks.at(el.wavebank_address));
        wavebanks = std::move(only);
    }
    d.voice = voice;
    return d;
}

std::tuple<std::int64_t, DrumVoiceP, SampleP> Decoder::process_drum_voice(std::int64_t address) const {
    check(address + 42 <= static_cast<std::int64_t>(data_.size()), "drum voice out of range");
    const Bytes raw(data_.begin() + address, data_.begin() + address + 42);
    Bytes dv = raw;
    dv[0] = raw[23];
    const int offs = s8(raw[29]);
    if (offs) dv[2] = static_cast<std::uint8_t>(std::max(0, std::min(127, raw[2] + offs)));
    dv[29] = 0;
    double boost = 0.0;
    if (dv[2]) {
        const double kDrumLevelFactor = std::pow(10.0, 0.82 / 40);
        const auto lv = static_cast<int>(std::min<std::int64_t>(127, py_round(dv[2] * kDrumLevelFactor)));
        const auto [l2, b] = drum_trimmed_level(lv, raw);
        dv[2] = static_cast<std::uint8_t>(l2);
        boost = b;
    }
    const auto be_raw = [&](std::size_t at, int n) {
        std::int64_t v = 0;
        for (int i = 0; i < n; ++i) v = (v << 8) | raw[at + static_cast<std::size_t>(i)];
        return v;
    };
    const std::int64_t ext = be_raw(24, 2);
    std::int64_t voice_address = 0;
    if (ext < 0xFFFF) {
        const std::int64_t voff = L_.extoffs.start + ext * 4;
        check(voff < L_.extoffs.end, "ext voice out of range");
        voice_address = L_.voices.start + 2 * be(voff, 4);
    }
    auto s = std::make_shared<Sample>();
    s->sample_type = fmt_from_mu90(raw[38] & 0xC0);
    s->encoding_parameters = (raw[38] & 0x3E) >> 1;
    s->offset_negative = be_raw(31, 3);
    s->offset_positive = be_raw(35, 3);
    s->loop_address = be_raw(39, 3) * 4;
    s->address_src = s->loop_address;
    if (raw[34] & 0x80) make_reversed(*s);
    if (ext == 0xFFFF && raw[20]) {
        const int value = raw[20];
        const int semis = dv[0] - dv[26] + s8(dv[27]);
        const double ratio = std::pow(2.0, semis / 12.0 + s8(dv[28]) / 1200.0);
        const double q = drum_hpf_cutoff(value) / ratio;
        const double fc = 18000.0 < q ? 18000.0 : q;
        s->hpf_fc = static_cast<int>(std::max<std::int64_t>(1, py_round(fc)));
        s->address_src = s->address_src + (static_cast<SKey>(s->hpf_fc + 1) << kHpfKeyShift);
    }
    auto d = std::make_shared<DrumVoice>();
    d->data = dv;
    d->sample_hash = s->address_src;
    d->ext_voice = voice_address ? to_str(voice_address) : std::string();
    d->offset_negative = s->offset_negative;
    d->offset_positive = s->offset_positive;
    d->vel_pitch = raw[21];
    d->vel_lpf = raw[22];
    d->level_boost = ext == 0xFFFF ? boost : 0.0;
    return {voice_address, d, s};
}

Table Decoder::create_table() {
    Table t;
    const std::int64_t wave_len = L_.wavespan;
    const auto check_sample = [&](const Sample& s) {
        check(s.start_address() >= 0, "sample starts before the wave ROM");
        check(s.end_address() < wave_len, "sample ends after the wave ROM");
    };
    static constexpr BankKind kDrumOrder[3] = {BankKind::GsDrums, BankKind::XgDrums, BankKind::SfxDrums};
    for (int idx = 0; idx < 3; ++idx) {
        const BankKind bank = kDrumOrder[idx];
        const std::int64_t prg_addr = drumprg_start_ + 128 * idx;
        for (int prg = 0; prg < 128; ++prg) {
            const int seq = byte(prg_addr + prg);
            if (seq == 0xFF) continue;
            const std::int64_t km = L_.keymaps.start + 256 * seq;
            check(km < L_.keymaps.end, "drum kit out of range");
            if (DrumKitP* k = t.drumkits.find(km)) {
                (*k)->aliases.push_back({bank, prg});
                continue;
            }
            auto kit = std::make_shared<DrumKit>();
            kit->keymap_address = km;
            kit->bank = bank;
            kit->prg = prg;
            for (int key = 0; key < 128; ++key) {
                const std::int64_t off = be(km + 2 * key, 2);
                if (off == 0xFFFF) continue;
                const std::int64_t dva = L_.drumvoices.start + off;
                check(dva < L_.drumvoices.end, "drum voice out of range");
                auto [ext, dv, sample] = process_drum_voice(dva);
                check_sample(*sample);
                if (!ext) {
                    t.drumvoices.set(dva, dv);
                    add_sample(t.samples, sample);
                    kit->drumvoices.set(key, dva);
                } else {
                    Decoded v = process_voice(ext);
                    t.voices.set(to_str(ext), v.voice);
                    t.drumvoices.set(dva, dv);
                    v.wavebanks.each([&](const std::string& k, const WaveBankP& wb) { t.wavebanks.set(k, wb); });
                    merge_samples(t.samples, v.samples);
                    kit->drumvoices.set(key, dva);
                }
            }
            t.drumkits.set(km, kit);
        }
    }
    static constexpr BankKind kVoiceOrder[5] = {BankKind::Xg, BankKind::Sfx, BankKind::Gs, BankKind::Gm2, BankKind::Msb48};
    for (int tn = 0; tn < 5; ++tn) {
        const BankKind bank = kVoiceOrder[tn];
        const std::int64_t bt = banks_start_ + 128 * tn;
        for (int idx = 0; idx < 128; ++idx) {
            const auto [lsb, msb] = bank_lsb_msb(bank, idx);
            const int seq = byte(bt + idx);
            if (seq == 0xFF) continue;
            const std::int64_t pm = L_.prgmap.start + static_cast<std::int64_t>(seq) * 512;
            check(pm < L_.prgmap.end, "program map out of range");
            if (VoiceBankP* vb = t.voice_banks.find(pm)) {
                (*vb)->aliases.push_back({bank, idx});
                continue;
            }
            auto vb = std::make_shared<VoiceBank>();
            vb->prg_address = pm;
            vb->bank = bank;
            vb->lsb = lsb;
            vb->msb = msb;
            vb->voices = std::make_shared<std::map<int, std::string>>();
            for (int prg = 0; prg < 128; ++prg) {
                const std::int64_t off = be(pm + 4 * prg, 4) * 2;
                check(off != 0xFFFF, "empty program");
                const std::int64_t va = L_.voices.start + off;
                check(va < L_.voices.end, "voice out of range");
                Decoded v = process_voice(va);
                check(!v.wavebanks.empty() && !v.samples.empty(), "voice without waves");
                v.samples.each([&](const SKey&, const SampleP& s) { check_sample(*s); });
                t.voices.set(to_str(va), v.voice);
                (*vb->voices)[prg] = to_str(va);
                v.wavebanks.each([&](const std::string& k, const WaveBankP& wb) { t.wavebanks.set(k, wb); });
                merge_samples(t.samples, v.samples);
            }
            t.voice_banks.set(pm, vb);
        }
    }
    return t;
}

} // namespace

Bytes interleave16(std::span<const std::uint8_t> a, std::span<const std::uint8_t> b) {
    check(a.size() == b.size(), "ROM halves differ in size");
    Bytes out(a.size() * 2);
    for (std::size_t i = 0; i + 1 < a.size(); i += 2) {
        out[2 * i] = a[i];
        out[2 * i + 1] = a[i + 1];
        out[2 * i + 2] = b[i];
        out[2 * i + 3] = b[i + 1];
    }
    return out;
}

Bytes dejumble(std::span<const std::uint8_t> in) {
    Bytes b(in.size(), 0);
    for (std::size_t i = 0; i + 1 < in.size(); i += 2) {
        b[i] = in[i + 1];
        b[i + 1] = in[i];
    }
    return b;
}

std::pair<Bytes, Bytes> mu2000_flash_split(std::span<const std::uint8_t> flash) {
    Bytes hi, lo;
    for (std::size_t i = 0; i + 3 < flash.size(); i += 4) {
        hi.push_back(flash[i + 1]);
        hi.push_back(flash[i]);
        lo.push_back(flash[i + 3]);
        lo.push_back(flash[i + 2]);
    }
    return {hi, lo};
}

void mu2000_scaling(Bytes& data) {
    const Layout& L = layout(Engine::Mu2000);
    std::map<int, std::tuple<std::array<int, 4>, std::array<int, 4>, int>> cache;
    std::int64_t p = L.voices.start;
    while (p < L.voices.end) {
        check(static_cast<std::size_t>(p) < data.size(), "MU2000 scaling out of range");
        const int cnt = std::popcount(static_cast<unsigned>(data[static_cast<std::size_t>(p)] & 0x0F));
        for (int k = 0; k < cnt; ++k) {
            const std::int64_t e = p + 14 + 84 * k;
            for (const int f : {36, 58}) {
                const auto at = static_cast<std::size_t>(e + f);
                check(at + 8 <= data.size(), "MU2000 scaling out of range");
                if (data[at] != 0xFF) continue;
                const int idx = (data[at + 6] << 8) | data[at + 7];
                auto it = cache.find(idx);
                if (it == cache.end()) {
                    std::array<int, 128> t{};
                    const auto base = static_cast<std::size_t>(kMu2000ScalingTables + 128 * idx);
                    check(base + 128 <= data.size(), "MU2000 scaling table out of range");
                    for (std::size_t n = 0; n < 128; ++n) t[n] = s8(data[base + n]);
                    it = cache.emplace(idx, scaling_inverse(t)).first;
                }
                const auto& [bp, of, err] = it->second;
                for (int i = 0; i < 4; ++i) {
                    check(bp[i] >= 0 && bp[i] < 256 && of[i] + 64 >= 0 && of[i] + 64 < 256, "MU2000 scaling value");
                    data[at + static_cast<std::size_t>(i)] = static_cast<std::uint8_t>(bp[i]);
                    data[at + 4 + static_cast<std::size_t>(i)] = static_cast<std::uint8_t>(of[i] + 64);
                }
            }
        }
        p += 14 + 84 * cnt;
    }
}

Table create_table(Engine engine, const Bytes& image, bool basic, std::span<const std::uint8_t> waverom,
                   elemreduce::EnvIds& env_ids) {
    Decoder d(engine, image, basic, waverom, env_ids);
    return d.create_table();
}

} // namespace hvyxg::syxg50::mu_rom
