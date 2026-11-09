// Copyright (C) 2026 Kevin H. Patterson
// SPDX-License-Identifier: BSD-3-Clause (see LICENSE)
// Translated from SXG-Create-NF, Copyright (c) 2026 Soundshock, BSD-3-Clause (THIRD_PARTY.md)
// Port of SXG-Create-NF utility/SampleConvert.py (BSD-3-Clause, Copyright 2026 Soundshock; the
// DPCM decoder and its tables follow MAME, BSD-3-Clause, copyright-holders Olivier Galibert).
// The arithmetic follows the Python expressions step by step (see py.hpp).
#include "syxg50/mu_rom/sample_convert.hpp"

#include <algorithm>
#include <cmath>
#include <numbers>

#include "syxg50/mu_rom/tables.hpp"
#include "fail.hpp"

namespace hvyxg::syxg50::mu_rom {

namespace {

using Wav = std::span<const std::uint8_t>;

void put_u16(Bytes& out, std::int64_t v) {
    out.push_back(static_cast<std::uint8_t>(v & 0xFF));
    out.push_back(static_cast<std::uint8_t>((v >> 8) & 0xFF));
}

int at(Wav w, std::int64_t i) {
    check(i >= 0 && i < static_cast<std::int64_t>(w.size()), "wave index out of range");
    return w[static_cast<std::size_t>(i)];
}

// ---- 12-bit (S12_to_16bit)

int cnv12_pos1(int v1, int v2) {
    int a = ((v2 & 0x0F) << 8) | (v1 & 0xF0) | (v1 & 0x0F);
    if (a & 0x0800) a -= 4096;
    return a * 16;
}
int cnv12_pos2(int v2, int v3) {
    int a = ((v2 & 0xF0) >> 4) | (v3 << 4);
    if (a & 0x0800) a -= 4096;
    return a * 16;
}

Bytes s12_to_u16(const Sample& s, Wav w) {
    check(s.out_sample_type == Fmt::U16, "S12 needs U16");
    // get_start_address_12bit / get_end_address_12bit: int(n * 12 / 8 + 0.5), nibble flag.
    const std::int64_t start = s.loop_address - static_cast<std::int64_t>(s.offset_negative * 12 / 8.0 + 0.5);
    const bool start_nib = (s.offset_negative * 12) % 8 != 0;
    const std::int64_t end = s.loop_address + static_cast<std::int64_t>(s.offset_positive * 12 / 8.0 + 0.5);
    const bool end_nib = (s.offset_positive * 12) % 8 != 0;
    const std::int64_t start_pt = start_nib ? start + 2 : start;
    const std::int64_t end_pt = end_nib ? end - 2 : end;
    std::vector<int> o;
    if (start_nib) o.push_back(cnv12_pos2(at(w, start), at(w, start + 1)));
    for (std::int64_t i = start_pt; i < end_pt; i += 3) {
        check(i >= 0 && i + 3 <= static_cast<std::int64_t>(w.size()), "12-bit read out of range");
        o.push_back(cnv12_pos1(w[i], w[i + 1]));
        o.push_back(cnv12_pos2(w[i + 1], w[i + 2]));
    }
    if (end_nib) {
        (void)at(w, end);  // the Python code passes waves[end] as well
        o.push_back(cnv12_pos1(at(w, end - 2), at(w, end - 1)));
    }
    Bytes out;
    out.reserve(o.size() * 2);
    for (const int v : o) put_u16(out, v + 32768);
    return out;
}

Bytes s8_to_u8(const Sample& s, Wav w) {
    const std::int64_t a0 = s.start_address(), a1 = s.end_address();
    check(a0 >= 0, "negative sample address");
    Bytes out(static_cast<std::size_t>(std::max<std::int64_t>(0, s.len_bytes())), 0);
    std::size_t idx = 0;
    for (std::int64_t a = a0; a < a1; ++a, ++idx) {
        const int v = a < static_cast<std::int64_t>(w.size()) ? static_cast<std::int8_t>(w[a]) : 0;
        check(idx < out.size(), "8-bit sample longer than its size");
        out[idx] = static_cast<std::uint8_t>(v + 128);
    }
    return out;
}

Bytes s16_to_u16(const Sample& s, Wav w) {
    check(s.out_sample_type == Fmt::U16, "S16 needs U16");
    const std::int64_t a0 = s.start_address(), a1 = s.end_address();
    check(a0 >= 0, "negative sample address");
    Bytes out(static_cast<std::size_t>(std::max<std::int64_t>(0, s.len_bytes())), 0);
    std::size_t idx = 0;
    const auto n = static_cast<std::int64_t>(w.size());
    for (std::int64_t a = a0; a < a1; a += 2, idx += 2) {
        int v = 0;
        if (a + 1 < n) v = static_cast<std::int16_t>(w[a] | (w[a + 1] << 8));
        else if (a < n) v = static_cast<std::int8_t>(w[a]);  // int.from_bytes of one byte, signed
        const int u = v + 32768;
        if (idx + 2 > out.size()) out.resize(idx + 2);
        out[idx] = static_cast<std::uint8_t>(u & 0xFF);
        out[idx + 1] = static_cast<std::uint8_t>(u >> 8);
    }
    return out;
}

// ---- DPCM (_ADPCM_Stepper, ADPCM_Stream, ADPCM_Find_LeadIn, ADPCM_to_S16)

struct Dpcm {
    int mode, scale, limit;
    explicit Dpcm(int enc) : mode(enc & 3), scale((enc >> 2) & 7), limit(kDpcmLimits[(enc >> 2) & 7]) {}
    // state: sample, delta, remainder
    void step(int input, int& smp, int& delta, int& rem) const {
        const int accum = smp >> scale;
        const int d = delta + kDpcmTable[input];
        int out = (accum + d) << scale;
        if (out < -0x8000) out = -0x8000;
        else if (out > limit) out = limit;
        int nd = (out >> scale) - accum;
        int nr = 0;
        switch (mode) {
        case 0: {
            const int y = nd * 7 + rem;
            nr = (y & 7) ? -(8 - (y & 7)) : 0;
            nd = y >> 3;
            break;
        }
        case 1: {
            const int y = nd * 3 + rem;
            nr = (y & 3) ? -(4 - (y & 3)) : 0;
            nd = y >> 2;
            break;
        }
        case 2: {
            const int y = nd + rem;
            nr = (y & 1) ? -(2 - (y & 1)) : 0;
            nd = y >> 1;
            break;
        }
        default:
            nd = 0;
            nr = 0;
        }
        smp = out;
        delta = nd;
        rem = nr;
    }
};

std::pair<Wav, Wav> dpcm_body_loop(const Sample& s, Wav w, std::int64_t loop_len) {
    const std::int64_t loop = s.loop_address;
    check(loop - s.offset_negative >= 0 && loop <= static_cast<std::int64_t>(w.size()), "DPCM body out of range");
    const Wav body = w.subspan(static_cast<std::size_t>(loop - s.offset_negative),
                               static_cast<std::size_t>(s.offset_negative));
    const std::int64_t le = std::min<std::int64_t>(loop + loop_len, static_cast<std::int64_t>(w.size()));
    const Wav lp = w.subspan(static_cast<std::size_t>(loop), static_cast<std::size_t>(le - loop));
    return {body, lp};
}

std::pair<std::int64_t, bool> dpcm_find_lead_in(const Sample& s, Wav w, std::int64_t loop_len, int max_passes = 8) {
    const Dpcm d(s.encoding_parameters);
    const auto [body, lp] = dpcm_body_loop(s, w, loop_len);
    int smp = 0, delta = 0, rem = 0;
    for (const auto b : body) d.step(b, smp, delta, rem);
    std::array<int, 3> state{smp, delta, rem};
    for (int j = 0; j < max_passes; ++j) {
        for (const auto b : lp) d.step(b, smp, delta, rem);
        const std::array<int, 3> ns{smp, delta, rem};
        if (ns == state) return {j, true};
        state = ns;
    }
    return {0, false};
}

Bytes dpcm_to_u16(const Sample& s, Wav w) {
    const std::int64_t n_out = s.offset_negative + s.lead_in + s.offset_positive;
    const std::int64_t loop_len = s.adpcm_loop_len;
    std::vector<int> out;
    const Dpcm d(s.encoding_parameters);
    if (loop_len > 0) {
        const auto [body, lp] = dpcm_body_loop(s, w, loop_len);
        check(static_cast<std::int64_t>(lp.size()) == loop_len, "DPCM loop out of range");
        int smp = 0, delta = 0, rem = 0;
        out.reserve(static_cast<std::size_t>(std::max<std::int64_t>(n_out, 0)) + lp.size());
        for (const auto b : body) {
            d.step(b, smp, delta, rem);
            out.push_back(smp);
        }
        while (static_cast<std::int64_t>(out.size()) < n_out)
            for (const auto b : lp) {
                d.step(b, smp, delta, rem);
                out.push_back(smp);
            }
        out.resize(static_cast<std::size_t>(std::max<std::int64_t>(0, std::min<std::int64_t>(n_out, out.size()))));
    } else {
        const std::int64_t a0 = s.start_address(), a1 = s.end_address();
        check(a1 < static_cast<std::int64_t>(w.size()), "DPCM end out of range");
        check(a0 >= 0, "negative sample address");
        out.assign(static_cast<std::size_t>(std::max<std::int64_t>(0, n_out)), 0);
        int smp = 0, delta = 0, rem = 0;
        std::size_t i = 0;
        for (std::int64_t a = a0; a < a1; ++a, ++i) {
            d.step(w[a], smp, delta, rem);
            check(i < out.size(), "DPCM sample longer than its size");
            out[i] = smp;
        }
    }
    Bytes b;
    b.reserve(out.size() * 2);
    check(s.out_sample_type == Fmt::U16, "DPCM needs U16");
    for (const int v : out) put_u16(b, v + 32768);
    return b;
}

// ---- high-pass (Apply_HPF)

std::vector<double> to_signed(const Bytes& data, Fmt fmt) {
    std::vector<double> x;
    if (fmt == Fmt::U16) {
        x.reserve(data.size() / 2);
        for (std::size_t i = 0; i + 1 < data.size(); i += 2) x.push_back((data[i] | (data[i + 1] << 8)) - 32768);
    } else if (fmt == Fmt::U8) {
        x.reserve(data.size());
        for (const auto v : data) x.push_back((v - 128) * 256);
    } else {
        HVYXG_THROW(ConvertError("HPF: unsupported format"));
    }
    return x;
}

Bytes apply_hpf(const Bytes& data, Fmt fmt, int fc, std::int64_t loop_start, std::int64_t loop_len) {
    const double fs = 44100.0, q = 1.0;
    const std::vector<double> x = to_signed(data, fmt);
    const double m = std::min(static_cast<double>(fc), 0.45 * fs);
    const double w0 = 2 * std::numbers::pi * m / fs;
    const double alpha = std::sin(w0) / (2 * q);
    const double c = std::cos(w0);
    const double a0 = 1 + alpha;
    const double b0 = (1 + c) / 2 / a0;
    const double b1 = -(1 + c) / a0;
    const double b2 = b0;
    const double a1 = -2 * c / a0;
    const double a2 = (1 - alpha) / a0;
    double x1 = 0.0, x2 = 0.0, y1 = 0.0, y2 = 0.0;
    const auto run = [&](const double* p, std::size_t n, std::vector<double>& out) {
        out.resize(n);
        for (std::size_t i = 0; i < n; ++i) {
            const double v = p[i];
            const double o = b0 * v + b1 * x1 + b2 * x2 - a1 * y1 - a2 * y2;
            x2 = x1;
            x1 = v;
            y2 = y1;
            y1 = o;
            out[i] = o;
        }
    };
    std::vector<double> y;
    if (loop_len > 0 && loop_start + loop_len <= static_cast<std::int64_t>(x.size())) {
        const auto le = static_cast<std::size_t>(loop_start + loop_len);
        run(x.data(), le, y);
        std::vector<double> ly;
        const std::int64_t passes = std::max<std::int64_t>(1, ceildiv(8192, loop_len)) + 1;
        for (std::int64_t k = 0; k < passes; ++k) run(x.data() + loop_start, static_cast<std::size_t>(loop_len), ly);
        std::copy(ly.begin(), ly.end(), y.begin() + loop_start);
        std::vector<double> rest;
        run(x.data() + le, x.size() - le, rest);
        y.insert(y.end(), rest.begin(), rest.end());
    } else {
        run(x.data(), x.size(), y);
    }
    Bytes out;
    if (fmt == Fmt::U16) {
        out.reserve(y.size() * 2);
        for (const double o : y) put_u16(out, std::min<std::int64_t>(32767, std::max<std::int64_t>(-32768, py_round(o))) + 32768);
    } else {
        out.reserve(y.size());
        for (const double o : y)
            out.push_back(static_cast<std::uint8_t>(std::min<std::int64_t>(127, std::max<std::int64_t>(-128, py_round(o / 256))) + 128));
    }
    return out;
}

// ---- baked amp EG (ConvertSample, env_bake)

Bytes env_sample(const Sample& s, Wav w) {
    const EnvBake& eb = *s.env_bake;
    Sample plain = s;
    plain.env_bake = nullptr;
    plain.offset_negative = s.src_neg;
    plain.offset_positive = s.src_pos + 2;
    plain.lead_in = 0;
    plain.adpcm_loop_len = 0;
    const Bytes data = convert_sample(plain, w);
    const bool u16 = s.out_sample_type == Fmt::U16;
    std::vector<double> x;
    if (u16) {
        for (std::size_t i = 0; i + 1 < data.size(); i += 2) x.push_back((data[i] | (data[i + 1] << 8)) - 32768);
    } else {
        for (const auto v : data) x.push_back((v - 128) * 256);
    }
    const auto xs = static_cast<std::int64_t>(x.size());
    const auto clip = [&](std::int64_t i) { return std::max<std::int64_t>(0, std::min(i, xs)); };
    const std::vector<double> body(x.begin(), x.begin() + clip(s.src_neg + 1));
    const std::vector<double> loop(x.begin() + clip(s.src_neg + 1), x.begin() + clip(s.src_neg + 1 + s.src_pos));
    const std::int64_t n = s.offset_negative + 1;
    std::vector<double> y = body;
    while (static_cast<std::int64_t>(y.size()) < n && !loop.empty()) y.insert(y.end(), loop.begin(), loop.end());
    y.resize(static_cast<std::size_t>(std::max<std::int64_t>(0, n)), 0.0);
    const double step = 0.005 * 44100.0 * eb.ratio;
    const auto last = static_cast<std::int64_t>(eb.gain_db.size()) - 1;
    for (std::int64_t i = 0; i < n; ++i) {
        const double p = static_cast<double>(i) / step;
        const auto j = static_cast<std::int64_t>(p);
        double gdb = j >= last ? eb.gain_db[last]
                               : eb.gain_db[j] + (eb.gain_db[j + 1] - eb.gain_db[j]) * (p - static_cast<double>(j));
        if (!eb.loop_db && j >= last) gdb = -120.0;
        y[i] *= std::pow(10.0, gdb / 20);
    }
    if (!eb.loop_db) {
        y.insert(y.end(), static_cast<std::size_t>(std::max<std::int64_t>(0, s.offset_positive - 1)), 0.0);
    } else {
        const double gl = std::pow(10.0, *eb.loop_db / 20);
        for (const double v : loop) y.push_back(v * gl);
        y.push_back(loop.empty() ? 0.0 : loop[0] * gl);
    }
    double peak = 0;
    bool any = false;
    for (const double v : y) {
        const double a = std::fabs(v);
        if (!any || a > peak) peak = a;
        any = true;
    }
    const double scale = peak > 32767 ? 32767 / peak : 1.0;
    Bytes out;
    if (u16) {
        out.reserve(y.size() * 2);
        for (const double v : y) put_u16(out, std::min<std::int64_t>(65535, std::max<std::int64_t>(0, py_round(v * scale) + 32768)));
    } else {
        out.reserve(y.size());
        for (const double v : y)
            out.push_back(static_cast<std::uint8_t>(std::min<std::int64_t>(255, std::max<std::int64_t>(0, py_round(v * scale / 256) + 128))));
    }
    return out;
}

} // namespace

Bytes convert_sample(const Sample& s, Wav w) {
    if (s.env_bake) return env_sample(s, w);
    if (s.hpf_fc) {
        Sample plain = s;
        plain.hpf_fc = 0;
        const Bytes data = convert_sample(plain, w);
        const std::int64_t loop_len = std::max<std::int64_t>(0, s.offset_positive - 2);
        const std::int64_t loop_start = s.offset_negative + s.lead_in + 1;
        return apply_hpf(data, s.out_sample_type, s.hpf_fc, loop_start, loop_len);
    }
    if (s.reverse) {
        const std::int64_t total = s.offset_negative, guard = s.offset_positive;
        Sample fwd = s;
        fwd.reverse = false;
        fwd.offset_positive = s.src_offset_positive;
        fwd.offset_negative = total - fwd.offset_positive;
        fwd.lead_in = 0;
        fwd.adpcm_loop_len = 0;
        fwd.offset_positive = fwd.offset_positive + guard;
        const Bytes data = convert_sample(fwd, w);
        const auto bps = static_cast<std::size_t>(fmt_bits(s.out_sample_type) >> 3);
        check(static_cast<std::int64_t>(data.size()) == (total + guard) * static_cast<std::int64_t>(bps), "reverse length");
        Bytes out;
        out.reserve(data.size());
        for (std::int64_t f = total - 1; f >= 0; --f)
            out.insert(out.end(), data.begin() + f * bps, data.begin() + (f + 1) * bps);
        if (guard > 0) {
            check(total > 0, "reverse: empty sample");
            for (std::int64_t g = 0; g < guard; ++g) out.insert(out.end(), data.begin(), data.begin() + bps);
        }
        return out;
    }
    Bytes out;
    switch (s.sample_type) {
    case Fmt::S8: out = s8_to_u8(s, w); break;
    case Fmt::S12: out = s12_to_u16(s, w); break;
    case Fmt::S16: out = s16_to_u16(s, w); break;
    case Fmt::Adpcm: out = dpcm_to_u16(s, w); break;
    default: HVYXG_THROW(ConvertError("unsupported source sample format"));
    }
    check(!out.empty(), "empty sample");
    return out;
}

std::pair<std::int64_t, Fmt> SampleMonster::feed(const SampleP& sp) {
    Sample& s = *sp;
    check(!s.written, "sample written twice");
    const Fmt target = fmt_target(s.sample_type);
    const std::int64_t bps = fmt_bits(target) >> 3;
    s.lead_in = 0;
    s.adpcm_loop_len = 0;
    if (s.sample_type == Fmt::Adpcm && s.offset_positive > 0 && !s.env_bake) {
        s.adpcm_loop_len = s.offset_positive;
        const auto [passes, settled] = dpcm_find_lead_in(s, waverom_, s.offset_positive);
        (void)settled;
        std::int64_t lead = passes * s.offset_positive;
        const std::int64_t body_limit = (s.used_by_drum && !big_) ? 0xFFFF : 0xFFFFFF;
        if (s.offset_negative + lead > body_limit) lead = 0;
        if (lead) {
            const std::int64_t projected = rom_end_ + (s.offset_negative + lead + s.offset_positive + 2) * bps;
            if (projected + adpcm_reserve_ > (big_ ? 0x3FFFFFFF : 0xFFFFFF)) lead = 0;
        }
        s.lead_in = lead;
    }
    s.offset_positive += 2;
    const std::int64_t body = (s.offset_negative + s.lead_in) * bps;
    const std::int64_t loop = s.offset_positive * bps;
    if (rom_end_ % 2) ++rom_end_;
    const std::int64_t new_loop = rom_end_ + body;
    queue_.push_back({rom_end_, sp, new_loop});
    rom_end_ += body + loop;
    return {new_loop + bps, target};
}

Bytes SampleMonster::generate() {
    Bytes out;
    out.reserve(static_cast<std::size_t>(rom_end_));
    for (const Item& it : queue_) {
        Sample& s = *it.sample;
        s.out_loop_address = it.new_loop;
        check(it.at - static_cast<std::int64_t>(out.size()) < 2, "wave file gap");
        while (static_cast<std::int64_t>(out.size()) < it.at) out.push_back(0x7F);
        const Bytes b = convert_sample(s, waverom_);
        const std::int64_t bps = s.out_sample_type == Fmt::U8 ? 1 : 2;
        check(static_cast<std::int64_t>(b.size()) == (s.offset_negative + s.lead_in + s.offset_positive) * bps,
              "converted sample length");
        out.insert(out.end(), b.begin(), b.end());
    }
    return out;
}

} // namespace hvyxg::syxg50::mu_rom
