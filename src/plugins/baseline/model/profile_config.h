/*
 * Copyright (C) 2026 LIHUO
 *
 * Licensed under the MIT License. See LICENSE file in the project root
 * for full license information.
 *
 */

#ifndef _FLOWSQL_PLUGINS_BASELINE_MODEL_PROFILE_CONFIG_H_
#define _FLOWSQL_PLUGINS_BASELINE_MODEL_PROFILE_CONFIG_H_

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <string>

#include "plugins/baseline/model/drift_state.h"

namespace flowsql {
namespace baseline {

constexpr double kT2EpsLogit = 1e-4;

// SharedProfileConfig 承载 design.md 第 12 章定义的 T1/T2 公共主参数。
// detector / trainer 不应各自硬编码这些值，后续参数标定也应从这里收口。
struct SharedProfileConfig {
    int32_t k_day = 4;
    int32_t k_week = 3;
    int32_t dme_max = 7;
    uint32_t m_month_enable = 4;
    double month_cov_min = 0.8;
    double lambda_season = 1.0;
    double lambda_dom = 4.0;
    double lambda_dme = 2.0;
    double lambda_lwd = 1.0;
    double lambda_event = 2.0;
    uint32_t n_val_switch = 16;
    double eps_switch = 0.05;
    double z_warn = 3.0;
    double z_crit = 5.0;
    double w_shift = 0.8;
    DriftConfig drift;
};

inline SharedProfileConfig DefaultSharedProfileConfig() {
    SharedProfileConfig config;
    config.drift.alpha = 0.2;
    config.drift.shift_clip = 6.0;
    config.drift.lambda_mem = 0.9;
    config.drift.kappa_shift = 0.25;
    config.drift.u_min = 0.5;
    config.drift.h_shift = 3.0;
    config.drift.p_shift_low = 0.3;
    config.drift.p_shift_high = 0.6;
    config.drift.m_shift = 3;
    config.drift.g_skip = 3;
    config.drift.g_reset = 12;
    return config;
}

struct T1bProfileConfig {
    std::string name;
    uint32_t n_train_min = 0;
    std::string transform_name_override = "log1p";

    uint32_t n_score_min() const { return (n_train_min + 1) / 2; }
    uint32_t n_shift_min() const { return n_train_min * 2; }
    double kappa_sample() const { return static_cast<double>(n_train_min); }
};

inline bool GetT1bProfileConfig(const std::string& profile_name, T1bProfileConfig* out) {
    if (!out) return false;
    T1bProfileConfig profile;
    profile.name = profile_name;
    if (profile_name == "cont_core") {
        profile.n_train_min = 50;
    } else if (profile_name == "cont_tail") {
        profile.n_train_min = 100;
    } else {
        return false;
    }
    *out = profile;
    return true;
}

struct T2ProfileConfig {
    std::string name;
    double s_prior = 0.0;
    uint32_t d_min_train = 0;
    double phi_over = 1.0;
    double m_floor = 1e-4;
    double eps_logit = kT2EpsLogit;
    double v_floor = 0.25;

    uint32_t d_score_min() const { return (d_min_train + 1) / 2; }
    uint32_t d_shift_min() const { return d_min_train * 2; }
    double kappa_den() const { return static_cast<double>(d_min_train); }
};

inline bool GetT2ProfileConfig(const std::string& profile_name, T2ProfileConfig* out) {
    if (!out) return false;
    T2ProfileConfig profile;
    profile.name = profile_name;
    if (profile_name == "rate_core") {
        profile.d_min_train = 50;
        profile.s_prior = 2.0;
        profile.phi_over = 1.5;
    } else if (profile_name == "ratio_bursty") {
        profile.d_min_train = 100;
        profile.s_prior = 4.0;
        profile.phi_over = 2.0;
    } else {
        return false;
    }
    *out = profile;
    return true;
}

struct RatioPriorConfig {
    double m0 = 0.5;
    double alpha0 = 0.0;
    double beta0 = 0.0;
};

inline RatioPriorConfig ComputeRatioPrior(const T2ProfileConfig& profile,
                                          double numerator_sum,
                                          double denominator_sum) {
    RatioPriorConfig prior;
    const double raw_m0 = denominator_sum > 0.0 ? numerator_sum / denominator_sum : 0.5;
    prior.m0 = std::max(profile.m_floor, std::min(1.0 - profile.m_floor, raw_m0));
    prior.alpha0 = profile.s_prior * prior.m0;
    prior.beta0 = profile.s_prior * (1.0 - prior.m0);
    return prior;
}

}  // namespace baseline
}  // namespace flowsql

#endif  // _FLOWSQL_PLUGINS_BASELINE_MODEL_PROFILE_CONFIG_H_
