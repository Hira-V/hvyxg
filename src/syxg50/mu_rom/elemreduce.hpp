// Copyright (C) 2026 Kevin H. Patterson
// SPDX-License-Identifier: BSD-3-Clause (see LICENSE)
// Translated from SXG-Create-NF, Copyright (c) 2026 Soundshock, BSD-3-Clause (THIRD_PARTY.md)
#pragma once
// Port of SXG-Create-NF utility/elemreduce.py (BSD-3-Clause, Copyright 2026 Soundshock): folds
// the 3- and 4-element voices of the MU128 engine into the S-YXG50's two elements (stereo
// pairs, key splits merged into one multisample, dropped layers' power folded into a level),
// with the amp EG baked into sample copies where a key split's members disagree.

#include <map>
#include <optional>
#include <span>
#include <string>
#include <tuple>
#include <utility>
#include <vector>

#include "syxg50/mu_rom/model.hpp"

namespace hvyxg::syxg50::mu_rom::elemreduce {

inline constexpr double kKsLevelSteps = 2.25;

using Range = std::pair<int, int>;

struct Out {
    std::vector<std::pair<int, Range>> members;  // (element index, key range)
    Range vels{0, 0};
    bool centre = false;
    std::optional<int> pair_mate;
    std::vector<int> absorbed;
};

double effective_level(const Bytes& e, int key);
/// plan(): nullopt when there is nothing to fold.
std::optional<std::vector<Out>> plan(const std::vector<Bytes>& els, const std::optional<std::vector<double>>& loudness);
int primary_member(const Out& o, const std::vector<Bytes>& els, const std::optional<std::vector<double>>& loudness);

/// Keys of the baked-EG sample copies (Env_Sample's module-wide id table).
class EnvIds {
public:
    std::int64_t id(SKey addr, std::int64_t neg, std::int64_t pos, const std::vector<double>& gain_db,
                    std::optional<double> loop_db, double ratio);

private:
    struct Key {
        SKey addr;
        std::int64_t neg, pos;
        std::vector<double> gain;
        bool has_loop;
        double loop;
        double ratio;
        bool operator<(const Key& o) const;
    };
    std::map<Key, std::int64_t> ids_;
};

SampleP env_sample(EnvIds& ids, const Sample& sample, const Wave& w, const std::vector<double>& gain_db,
                   std::optional<double> loop_db, double ratio);
WaveBankP start_offset_wavebank(const WaveBankP& wb, int units);
/// merge_wavebanks → (wave bank, round(top)).
std::pair<WaveBankP, int> merge_wavebanks(EnvIds& ids, const std::vector<std::tuple<WaveBankP, Range, int>>& members,
                                          const std::vector<Bytes>& els, int primary, const std::string& address,
                                          const SampleDict* samples, SampleDict* new_samples);
void flatten_level(Bytes& edata, std::int64_t level, bool keep_ks = false);
/// absorb_level; `level_is_int` keeps Python's int arithmetic for an int level.
double absorb_level(double level, bool level_is_int, const std::vector<int>& extra_levels);
std::optional<double> wavebank_power(const WaveBank& wb, const SampleDict& samples, std::span<const std::uint8_t> waverom);
double pan_power(int pan);
double element_power(const Bytes& e, double wave_power);
double matched_level(double kept, double target, double level);
std::optional<double> element_loudness(const Bytes& e, std::optional<double> wave_power);
/// gain_wavebank → (wave bank, new samples).
std::pair<WaveBankP, SampleDict> gain_wavebank(EnvIds& ids, const WaveBank& wb, const SampleDict& samples, double gain_db);

} // namespace hvyxg::syxg50::mu_rom::elemreduce
