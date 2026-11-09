// Copyright (C) 2026 Kevin H. Patterson
// SPDX-License-Identifier: BSD-3-Clause (see LICENSE)
// Translated from SXG-Create-NF, Copyright (c) 2026 Soundshock, BSD-3-Clause (THIRD_PARTY.md)
// Port of SXG-Create-NF utility/elemreduce.py (BSD-3-Clause, Copyright 2026 Soundshock). The
// arithmetic follows the Python expressions step by step (see py.hpp).
#include "syxg50/mu_rom/elemreduce.hpp"

#include <algorithm>
#include <cmath>
#include <set>

#include "syxg50/mu_rom/sample_convert.hpp"

namespace hvyxg::syxg50::mu_rom::elemreduce {

namespace {

constexpr double kAudibleDrop = 0.6;
constexpr double kAttenDb = 0.75;
constexpr double kAttenPieceDb = 1.5;
constexpr int kCentral0 = 36, kCentral1 = 96;
constexpr double kSilent = 0.01;
constexpr double kEgBakeMaxDb = 12.0;
constexpr double kEgSilentDb = -49.0;
constexpr double kEgStep = 0.005;
constexpr double kEgMaxT = 6.0;
constexpr int kEgBakeSpan = 6;
constexpr int kStartOffsetUnit = 128;
constexpr int kEnvKeyShift = 56;
constexpr std::int64_t kEnvLoop = 16;
constexpr int kEnvFs = 44100;

Range keys(const Bytes& e) { return {e[2], e[3]}; }
Range vels(const Bytes& e) { return {e[4], e[5]}; }
int pan(const Bytes& e) { return e[67]; }
int level(const Bytes& e) { return e[57]; }
bool overlap(Range a, Range b) { return !(a.second < b.first || b.second < a.first); }

double level_ks(const Bytes& e, int key) {
    const int bp[4] = {e[58], e[59], e[60], e[61]};
    const int off[4] = {e[62] - 64, e[63] - 64, e[64] - 64, e[65] - 64};
    if (key <= bp[0]) return off[0];
    for (int i = 0; i < 3; ++i) {
        if (key <= bp[i + 1]) {
            const int span = bp[i + 1] - bp[i];
            if (span <= 0) return off[i];
            return off[i] + static_cast<double>((off[i + 1] - off[i]) * (key - bp[i])) / span;
        }
    }
    return off[3];
}

/// Python min(a, b) / max(a, b) for floats: the first unless the second is smaller / larger.
double pmin(double a, double b) { return b < a ? b : a; }
double pmax(double a, double b) { return b > a ? b : a; }

Range audible_keys(const Bytes& e) {
    if (e[2] > e[3]) return keys(e);
    std::vector<double> lv;
    for (int x = e[2]; x <= e[3]; ++x) lv.push_back(effective_level(e, x));
    double top = lv[0];
    for (const double v : lv) top = pmax(top, v);
    int first = -1, last = -1;
    for (int x = e[2]; x <= e[3]; ++x) {
        const double v = lv[static_cast<std::size_t>(x - e[2])];
        if (v > 0 && v >= kAudibleDrop * top) {
            if (first < 0) first = x;
            last = x;
        }
    }
    return first >= 0 ? Range{first, last} : keys(e);
}

struct Unit {
    std::vector<int> idx;
    Range keys, vels;
    double level;
    bool pair;
};

Unit make_unit(std::vector<int> idx, const std::vector<Bytes>& els, const std::optional<std::vector<double>>& loudness) {
    Unit u;
    const Bytes& e = els[static_cast<std::size_t>(idx[0])];
    u.keys = audible_keys(e);
    u.vels = vels(e);
    if (loudness) {
        std::vector<double> v;
        for (const int i : idx) v.push_back((*loudness)[static_cast<std::size_t>(i)]);
        u.level = py_sum(v);
    } else {
        u.level = level(e);
    }
    u.pair = idx.size() > 1;
    u.idx = std::move(idx);
    return u;
}

std::vector<Range> key_ranges(const Out& o) {
    std::vector<Range> r;
    for (const auto& m : o.members) r.push_back(m.second);
    return r;
}

bool fits(const Out& o, Range r, Range v) {
    if (o.vels != v) return false;
    for (const auto& q : key_ranges(o))
        if (overlap(r, q)) return false;
    return true;
}

double gain_db(double lv) { return lv <= 0 ? -200.0 : 40 * std::log10(lv / 128); }

double eg_slope(int rate) { return rate == 0 ? 0.0 : 16.2 * std::pow(2.0, (rate - 20) / 4.0); }
double eg_level_db(int lv) { return -0.735 * (127 - lv); }

std::vector<std::optional<double>> eg_curve(const Bytes& e) {
    const int ar = e[71], d1r = e[72], d2r = e[73], d1l = e[75], d2l = e[76];
    std::vector<std::optional<double>> out;
    double lvl = ar >= 60 ? 0.0 : kEgSilentDb;
    int stage = ar >= 60 ? 1 : 0;
    const double l1 = eg_level_db(d1l), l2 = eg_level_db(d2l);
    const auto steps = static_cast<int>(kEgMaxT / kEgStep) + 1;
    for (int i = 0; i < steps; ++i) {
        out.push_back(lvl > kEgSilentDb ? std::optional<double>(lvl) : std::nullopt);
        if (stage == 0) {
            lvl += eg_slope(ar) * kEgStep;
            if (lvl >= 0) {
                lvl = 0.0;
                stage = 1;
            }
        } else if (stage == 1) {
            const double sl = eg_slope(d1r) * kEgStep;
            lvl = l1 < lvl ? pmax(l1, lvl - sl) : pmin(l1, lvl + sl);
            if (lvl == l1 || sl == 0) stage = 2;
        } else {
            const double sl = eg_slope(d2r) * kEgStep;
            lvl = l2 < lvl ? pmax(l2, lvl - sl) : pmin(l2, lvl + sl);
        }
    }
    return out;
}

/// eg_bake_curve: the gain curve of a one-shot copy, or nullopt (EG_BAKE_SUSTAINED is off).
std::optional<std::vector<double>> eg_bake_curve(const Bytes& own, const Bytes& kept) {
    const auto a = eg_curve(own), b = eg_curve(kept);
    const auto ends = [](const std::vector<std::optional<double>>& c) {
        bool on = false;
        for (std::size_t i = 0; i < c.size(); ++i) {
            if (c[i]) on = true;
            else if (on) return i;
        }
        return c.size();
    };
    const double lo = kEgSilentDb - 20;
    const std::size_t end = std::min(ends(a), ends(b));
    if (end < a.size()) {
        const std::size_t n = std::max<std::size_t>(1, end);
        std::vector<double> g;
        for (std::size_t i = 0; i < n && i < b.size(); ++i)
            g.push_back(pmin(kEgBakeMaxDb, (a[i] ? *a[i] : lo) - (b[i] ? *b[i] : kEgSilentDb)));
        return g;
    }
    return std::nullopt;
}

double pitch_scale(const Bytes& e) {
    switch (e[17] & 7) {
    case 0: return 1.0;
    case 1: return 0.5;
    case 2: return 0.25;
    case 3: return 0.125;
    default: return 0.0;
    }
}

int pitch_key(const Bytes& e, int key) {
    const int c = e[18];
    return c + static_cast<int>(std::floor((key - c) * pitch_scale(e))) + (e[15] - 0x40);
}

double pitch_exact(const Bytes& e, double key) {
    const int c = e[18];
    return c + (key - c) * pitch_scale(e) + (e[15] - 0x40);
}

double key_on_delay_ms(const Bytes& e) {
    const int d = e[70] & 0x0F;
    return d < 2 ? 0.0 : 69 * std::pow(1.43, static_cast<double>(d - 4));
}

} // namespace

double effective_level(const Bytes& e, int key) {
    return pmax(0.0, pmin(145.0, e[57] + kKsLevelSteps * level_ks(e, key)));
}

std::optional<std::vector<Out>> plan(const std::vector<Bytes>& els, const std::optional<std::vector<double>>& loudness) {
    std::vector<int> live;
    for (int i = 0; i < static_cast<int>(els.size()); ++i) {
        const Bytes& e = els[static_cast<std::size_t>(i)];
        if (level(e) > 0 && e[2] <= e[3] && e[4] <= e[5]) live.push_back(i);
    }
    std::vector<Unit> units;
    std::set<int> used;
    for (const int i : live) {
        if (used.count(i)) continue;
        std::optional<int> mate;
        const Bytes& ei = els[static_cast<std::size_t>(i)];
        for (const int j : live) {
            const Bytes& ej = els[static_cast<std::size_t>(j)];
            if (j > i && !used.count(j) && keys(ej) == keys(ei) && vels(ej) == vels(ei) && ei[15] == ej[15] &&
                pan(ei) != 7 && pan(ei) + pan(ej) == 14) {
                mate = j;
                break;
            }
        }
        used.insert(i);
        if (mate) {
            used.insert(*mate);
            units.push_back(make_unit({i, *mate}, els, loudness));
        } else {
            units.push_back(make_unit({i}, els, loudness));
        }
    }
    if (els.size() <= 2 || units.empty()) return std::nullopt;
    std::vector<std::size_t> order(units.size());
    for (std::size_t i = 0; i < order.size(); ++i) order[i] = i;
    std::stable_sort(order.begin(), order.end(), [&](std::size_t a, std::size_t b) {
        const Unit &ua = units[a], &ub = units[b];
        if (-ua.level != -ub.level) return -ua.level < -ub.level;
        return ua.idx[0] < ub.idx[0];
    });
    std::vector<Out> outs;
    outs.reserve(4);
    std::vector<std::size_t> dropped;
    for (const std::size_t ui : order) {
        const Unit& u = units[ui];
        Out* o = nullptr;
        for (Out& x : outs)
            if (fits(x, u.keys, u.vels)) {
                o = &x;
                break;
            }
        if (o == nullptr && outs.size() < 2) {
            outs.emplace_back();
            o = &outs.back();
            o->vels = u.vels;
        }
        if (o == nullptr) {
            dropped.push_back(ui);
            continue;
        }
        o->members.push_back({u.idx[0], u.keys});
        if (u.pair) {
            o->centre = true;
            o->pair_mate = u.idx[1];
        }
    }
    const auto covered_in = [](const Out& o, int k) {
        for (const auto& m : o.members)
            if (m.second.first <= k && k <= m.second.second) return true;
        return false;
    };
    const auto covered = [&](int k) {
        for (const Out& o : outs)
            if (covered_in(o, k)) return true;
        return false;
    };
    double top = 0;
    bool first = true;
    for (const Unit& u : units) {
        if (first || u.level > top) top = u.level;
        first = false;
    }
    for (const std::size_t ui : dropped) {
        const Unit& u = units[ui];
        if (loudness && u.level < kSilent * top) continue;
        std::vector<std::pair<std::size_t, std::vector<int>>> assign;  // out index → keys, first-assigned order
        for (int k = u.keys.first; k <= u.keys.second; ++k) {
            std::optional<std::size_t> o;
            for (std::size_t x = 0; x < outs.size(); ++x)
                if (outs[x].vels == u.vels && !covered_in(outs[x], k)) {
                    o = x;
                    break;
                }
            if (!o && !covered(k)) {
                std::size_t best = 0;
                for (std::size_t x = 1; x < outs.size(); ++x) {
                    const auto kx = std::make_pair(outs[x].members.size(), outs[x].vels != u.vels);
                    const auto kb = std::make_pair(outs[best].members.size(), outs[best].vels != u.vels);
                    if (kx < kb) best = x;
                }
                o = best;
            }
            if (o) {
                auto it = std::find_if(assign.begin(), assign.end(), [&](const auto& a) { return a.first == *o; });
                if (it == assign.end()) {
                    assign.push_back({*o, {}});
                    it = assign.end() - 1;
                }
                it->second.push_back(k);
            }
        }
        if (assign.empty()) {
            std::size_t best = 0;
            const auto score = [&](const Out& o) {
                int n = 0;
                for (const auto& r : key_ranges(o)) n += overlap(u.keys, r) ? 1 : 0;
                return std::make_pair(n, o.vels == u.vels);
            };
            for (std::size_t x = 1; x < outs.size(); ++x)
                if (score(outs[x]) > score(outs[best])) best = x;
            outs[best].absorbed.insert(outs[best].absorbed.end(), u.idx.begin(), u.idx.end());
            continue;
        }
        for (auto& [oi, ks] : assign) {
            int seg0 = ks[0];
            for (std::size_t n = 0; n < ks.size(); ++n) {
                const int a = ks[n];
                const bool has_b = n + 1 < ks.size();
                const int b = has_b ? ks[n + 1] : 0;
                if (!has_b || b != a + 1) {
                    outs[oi].members.push_back({u.idx[0], {seg0, a}});
                    if (has_b) seg0 = b;
                }
            }
        }
    }
    if (outs.size() == 2 && outs[0].vels != outs[1].vels) {
        Out* a = &outs[0];
        Out* b = &outs[1];
        if (b->vels.first < a->vels.first) std::swap(a, b);  // sorted by vels[0], stable
        if (a->vels.second + 1 < b->vels.first) {
            const int mid = static_cast<int>(floordiv(a->vels.second + b->vels.first, 2));
            a->vels = {a->vels.first, mid};
            b->vels = {mid + 1, b->vels.second};
        }
        a->vels = {std::min(a->vels.first, 1), a->vels.second};
        b->vels = {b->vels.first, std::max(b->vels.second, 127)};
    }
    for (Out& o : outs) {
        std::stable_sort(o.members.begin(), o.members.end(),
                         [](const auto& x, const auto& y) { return x.second.first < y.second.first; });
        auto& ms = o.members;
        for (std::size_t n = 0; n < ms.size(); ++n) {
            const int i = ms[n].first;
            const auto [a, b] = ms[n].second;
            const Bytes& e = els[static_cast<std::size_t>(i)];
            const int lo = n ? std::max<int>(e[2], ms[n - 1].second.second + 1) : e[2];
            const int hi = n + 1 < ms.size() ? std::min<int>(e[3], ms[n + 1].second.first - 1) : e[3];
            ms[n] = {i, {std::min(a, lo), std::max(b, hi)}};
        }
    }
    return outs;
}

int primary_member(const Out& o, const std::vector<Bytes>& els, const std::optional<std::vector<double>>& loudness) {
    const auto central = [](Range r) { return std::max(0, std::min(r.second, kCentral1) - std::max(r.first, kCentral0) + 1); };
    const auto weight = [&](int i) { return loudness ? std::pow((*loudness)[static_cast<std::size_t>(i)], 0.25) : 1.0; };
    const auto key = [&](const std::pair<int, Range>& m) {
        return std::make_tuple(central(m.second) * weight(m.first), static_cast<int>(els[static_cast<std::size_t>(m.first)][57]), -m.first);
    };
    std::size_t best = 0;
    for (std::size_t i = 1; i < o.members.size(); ++i)
        if (key(o.members[i]) > key(o.members[best])) best = i;
    return o.members[best].first;
}

bool EnvIds::Key::operator<(const Key& o) const {
    if (addr != o.addr) return addr < o.addr;
    if (neg != o.neg) return neg < o.neg;
    if (pos != o.pos) return pos < o.pos;
    if (gain.size() != o.gain.size()) return gain.size() < o.gain.size();
    for (std::size_t i = 0; i < gain.size(); ++i)
        if (gain[i] != o.gain[i]) return gain[i] < o.gain[i];
    if (has_loop != o.has_loop) return has_loop < o.has_loop;
    if (has_loop && loop != o.loop) return loop < o.loop;
    if (ratio != o.ratio) return ratio < o.ratio;
    return false;
}

std::int64_t EnvIds::id(SKey addr, std::int64_t neg, std::int64_t pos, const std::vector<double>& gain_db,
                        std::optional<double> loop_db, double ratio) {
    Key k{addr, neg, pos, {}, loop_db.has_value(), loop_db ? py_round_nd(*loop_db, 1) : 0.0, py_round_nd(ratio, 3)};
    k.gain.reserve(gain_db.size());
    for (const double g : gain_db) k.gain.push_back(py_round_nd(g, 1));
    const auto it = ids_.find(k);
    if (it != ids_.end()) return it->second;
    const auto n = static_cast<std::int64_t>(ids_.size()) + 1;
    ids_.emplace(std::move(k), n);
    return n;
}

SampleP env_sample(EnvIds& ids, const Sample& sample, const Wave& w, const std::vector<double>& gain_db,
                   std::optional<double> loop_db, double ratio) {
    const std::int64_t id = ids.id(sample.address_src, w.offset_negative, w.offset_positive, gain_db, loop_db, ratio);
    auto s = std::make_shared<Sample>(sample);
    s->env_bake = std::make_shared<const EnvBake>(EnvBake{gain_db, loop_db, ratio});
    s->src_neg = w.offset_negative;
    s->src_pos = w.offset_positive;
    std::int64_t n = static_cast<std::int64_t>(
        std::ceil(static_cast<double>(gain_db.size()) * kEgStep * ratio * kEnvFs));
    if (!loop_db) {
        if (w.offset_positive == 0) n = std::min(n, w.offset_negative);
        s->offset_negative = std::max<std::int64_t>(1, std::min<std::int64_t>(n, std::int64_t{1} << 21));
        s->offset_positive = kEnvLoop;
    } else {
        const std::int64_t k = std::max<std::int64_t>(
            0, static_cast<std::int64_t>(std::ceil(static_cast<double>(n - w.offset_negative) / static_cast<double>(w.offset_positive))));
        s->offset_negative = w.offset_negative + k * w.offset_positive;
        s->offset_positive = w.offset_positive;
    }
    s->address_src = sample.address_src + (static_cast<SKey>(id) << kEnvKeyShift);
    return s;
}

WaveBankP start_offset_wavebank(const WaveBankP& wb, int units) {
    if (!units) return wb;
    auto out = std::make_shared<WaveBank>();
    out->address_src = wb->address_src + "_so" + std::to_string(units);
    for (const Wave& w : wb->waves) {
        Wave nw = w;
        nw.offset_negative = std::max<std::int64_t>(0, w.offset_negative - static_cast<std::int64_t>(units) * kStartOffsetUnit);
        out->waves.push_back(nw);
    }
    return out;
}

std::pair<WaveBankP, int> merge_wavebanks(EnvIds& ids, const std::vector<std::tuple<WaveBankP, Range, int>>& members,
                                          const std::vector<Bytes>& els, int primary_i, const std::string& address,
                                          const SampleDict* samples, SampleDict* new_samples) {
    const Bytes& primary = els[static_cast<std::size_t>(primary_i)];
    bool first = true;
    double top = 0;
    for (const auto& [wb, r, i] : members)
        for (int k = r.first; k <= r.second; ++k) {
            const double v = effective_level(els[static_cast<std::size_t>(i)], k);
            if (first || v > top) top = v;
            first = false;
        }
    check(!first, "merge: no keys");
    const int co_p = primary[15] - 0x40;
    std::vector<Wave> waves;
    auto sorted = members;
    std::stable_sort(sorted.begin(), sorted.end(),
                     [](const auto& a, const auto& b) { return std::get<1>(a).first < std::get<1>(b).first; });
    for (const auto& [wb, r, ei] : sorted) {
        const Bytes& e = els[static_cast<std::size_t>(ei)];
        const auto [k0, k1] = r;
        std::optional<std::vector<double>> bake;
        if (samples != nullptr && ei != primary_i) bake = eg_bake_curve(e, primary);
        if (bake) {
            for (const Wave& w : wb->waves) {
                const SampleP* s = samples->find(w.loop_address_src);
                if (s != nullptr && (*s)->reverse) {
                    bake.reset();
                    break;
                }
            }
        }
        const auto wave_of = [&](int k) {
            const int x = pitch_key(e, k);
            for (std::size_t n = 0; n < wb->waves.size(); ++n)
                if (x <= wb->waves[n].key_max) return n;
            return wb->waves.size() - 1;
        };
        std::vector<double> att;  // index k - k0
        for (int k = k0; k <= k1; ++k) att.push_back(gain_db(top) - gain_db(effective_level(e, k)));
        const auto A = [&](int k) { return att[static_cast<std::size_t>(k - k0)]; };
        std::vector<Range> pieces;
        int p0 = k0;
        int span = bake ? kEgBakeSpan : 999;
        if (pitch_scale(e) != 1.0) span = 1;
        for (int k = k0 + 1; k <= k1 + 1; ++k) {
            bool cut = k > k1 || wave_of(k) != wave_of(p0) || k - p0 >= span;
            if (!cut) {
                double mx = A(p0), mn = A(p0);
                for (int j = p0; j <= k; ++j) {
                    mx = pmax(mx, A(j));
                    mn = pmin(mn, A(j));
                }
                cut = mx - mn > kAttenPieceDb;
            }
            if (cut) {
                pieces.push_back({p0, k - 1});
                p0 = k;
            }
        }
        for (const auto& [q0, q1] : pieces) {
            const Wave& w = wb->waves[wave_of(q0)];
            const int r1 = std::max(0, std::min(0x7F, q1 + co_p));
            if (!waves.empty() && r1 <= waves.back().key_max) continue;
            const double kr = (q0 + q1) / 2.0;
            Wave nw = w;
            nw.key_min = std::max(0, std::min(0x7F, q0 + co_p));
            nw.key_max = r1;
            nw.tune_note = static_cast<int>(py_round(w.tune_note + (kr + co_p) - pitch_exact(e, kr)));
            check(0 <= nw.tune_note && nw.tune_note <= 0xFF, "merge: tune note out of range");
            std::vector<double> part;
            for (int k = q0; k <= q1; ++k) part.push_back(A(k));
            const double mean = py_sum(part) / (q1 - q0 + 1);
            nw.attenuation = static_cast<int>(std::min<std::int64_t>(0x7F, w.attenuation + std::max<std::int64_t>(0, py_round(mean / kAttenDb))));
            if (bake) {
                const double ratio = std::pow(2.0, (pitch_exact(e, kr) - w.tune_note) / 12);
                const SampleP es = env_sample(ids, *samples->at(w.loop_address_src), w, *bake, std::nullopt, ratio);
                new_samples->set(es->address_src, es);
                nw.loop_address_src = es->address_src;
                nw.offset_negative = es->offset_negative;
                nw.offset_positive = es->offset_positive;
            }
            waves.push_back(nw);
        }
    }
    check(!waves.empty(), "merge: no waves");
    waves[0].key_min = 0;
    for (std::size_t i = 1; i < waves.size(); ++i) waves[i].key_min = waves[i - 1].key_max + 1;
    waves.back().key_max = 0x7F;
    auto out = std::make_shared<WaveBank>();
    out->address_src = address;
    out->waves = std::move(waves);
    return {out, static_cast<int>(py_round(top))};
}

void flatten_level(Bytes& edata, std::int64_t lv, bool keep_ks) {
    edata[57] = static_cast<std::uint8_t>(std::max<std::int64_t>(1, std::min<std::int64_t>(127, lv)));
    const std::int64_t extra = py_round(static_cast<double>(std::max<std::int64_t>(0, lv - 127)) / kKsLevelSteps);
    for (int i = 62; i < 66; ++i)
        edata[static_cast<std::size_t>(i)] =
            static_cast<std::uint8_t>(keep_ks ? std::min<std::int64_t>(127, edata[static_cast<std::size_t>(i)] + extra) : 64 + extra);
}

double absorb_level(double lv, bool level_is_int, const std::vector<int>& extra_levels) {
    std::int64_t s = 0;
    for (const int x : extra_levels) s += static_cast<std::int64_t>(x) * x * x * x;
    if (level_is_int) {
        const auto l = static_cast<std::int64_t>(lv);
        return std::pow(static_cast<double>(l * l * l * l + s), 0.25);
    }
    return std::pow(std::pow(lv, 4.0) + static_cast<double>(s), 0.25);
}

std::optional<double> wavebank_power(const WaveBank& wb, const SampleDict& samples, std::span<const std::uint8_t> waverom) {
    double p = 0.0;
    std::int64_t n = 0;
    for (const Wave& w : wb.waves) {
        if (w.key_max < 48 || w.key_min > 84) continue;
        Sample s = *samples.at(w.loop_address_src);
        s.lead_in = 0;
        s.adpcm_loop_len = 0;
        s.out_sample_type = fmt_target(s.sample_type);
        // The formats convert_sample takes; any other has no level to compare.
        if (s.sample_type != Fmt::S8 && s.sample_type != Fmt::S12 && s.sample_type != Fmt::S16 &&
            s.sample_type != Fmt::Adpcm)
            return std::nullopt;
        Bytes data;
#if HVYXG_EXCEPTIONS
        try {
            data = convert_sample(s, waverom);
        } catch (const ConvertError&) {
            return std::nullopt;
        }
#else
        data = convert_sample(s, waverom);
#endif
        u128 sum = 0;
        std::uint64_t len = 0;
        if (s.out_sample_type == Fmt::U16) {
            for (std::size_t i = 0; i + 1 < data.size(); i += 2) {
                const std::int64_t v = (data[i] | (data[i + 1] << 8)) - 32768;
                sum += static_cast<u128>(v * v);
                ++len;
            }
        } else {
            for (const auto b : data) {
                const std::int64_t v = (b - 128) * 256;
                sum += static_cast<u128>(v * v);
                ++len;
            }
        }
        if (len == 0) continue;
        const int n_keys = std::min(w.key_max, 84) - std::max(w.key_min, 48) + 1;
        p += n_keys * py_truediv(sum, len) * std::pow(10.0, -w.attenuation * kAttenDb / 10);
        n += n_keys;
    }
    if (!n) return std::nullopt;
    return p / static_cast<double>(n);
}

double pan_power(int pan) {
    static constexpr double kFar[8] = {0, 0, -16.61, -10.59, -9.35, -6.02, -3.33, 0.0};
    std::optional<double> far = 0.0;
    if (0 <= pan && pan <= 14) {
        const int i = std::min(pan, 14 - pan);
        far = i < 2 ? std::nullopt : std::optional<double>(kFar[i]);
    }
    return (1 + (far ? std::pow(10.0, *far / 10) : 0.0)) / 2;
}

double element_power(const Bytes& e, double wave_power) {
    return std::pow(e[57] / 128.0, 4.0) * wave_power * pan_power(e[67]);
}

double matched_level(double kept, double target, double lv) { return lv * std::pow(target / kept, 0.25); }

std::optional<double> element_loudness(const Bytes& e, std::optional<double> wave_power) {
    if (!wave_power) return std::nullopt;
    const double audible = pmax(0.02, 1 - key_on_delay_ms(e) / 1000);
    return std::pow(e[57] / 128.0, 4.0) * *wave_power * audible;
}

std::pair<WaveBankP, SampleDict> gain_wavebank(EnvIds& ids, const WaveBank& wb, const SampleDict& samples, double gain) {
    auto out = std::make_shared<WaveBank>();
    SampleDict news;
    for (const Wave& w : wb.waves) {
        Wave nw = w;
        const std::int64_t steps =
            w.offset_positive ? 1 : static_cast<std::int64_t>(static_cast<double>(w.offset_negative) / (kEgStep * kEnvFs)) + 2;
        const SampleP es = env_sample(ids, *samples.at(w.loop_address_src), w, std::vector<double>(static_cast<std::size_t>(steps), gain),
                                      w.offset_positive ? std::optional<double>(gain) : std::nullopt, 1.0);
        if (w.offset_positive) es->offset_negative = w.offset_negative;
        news.set(es->address_src, es);
        nw.loop_address_src = es->address_src;
        nw.offset_negative = es->offset_negative;
        nw.offset_positive = es->offset_positive;
        out->waves.push_back(nw);
    }
    out->address_src = wb.address_src + "_g" + std::to_string(py_round(gain * 10));
    return {out, std::move(news)};
}

} // namespace hvyxg::syxg50::mu_rom::elemreduce
