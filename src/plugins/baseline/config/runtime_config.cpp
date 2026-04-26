/*
 * Copyright (C) 2026 LIHUO
 *
 * Licensed under the MIT License. See LICENSE file in the project root
 * for full license information.
 *
 */

#include "plugins/baseline/config/runtime_config.h"

#include <common/error_code.h>

#include <algorithm>
#include <atomic>
#include <exception>
#include <memory>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <utility>

#include <yaml-cpp/yaml.h>

#include "plugins/baseline/model/profile_config.h"
#include "plugins/baseline/solver/solver_backend.h"

namespace flowsql {
namespace baseline {

namespace {

struct RatioGlobalNumericalConfig {
    double eps_logit = kRatioEpsLogit;
    double m_floor = kRatioMFloor;
    double v_floor = kRatioVFloor;
};

struct RuntimeConfigState {
    std::string tz_default = "Asia/Shanghai";

    SharedProfileConfig shared_profile;
    std::unordered_map<std::string, ValueSampledProfileConfig> value_sampled_profiles;
    RatioGlobalNumericalConfig ratio_global;
    std::unordered_map<std::string, RatioProfileConfig> ratio_profiles;

    BlockSolverConfig block_solver;

    int64_t runtime_idle_prune_bucket_gap = 4096;
    std::size_t runtime_idle_prune_scan_limit = 32;

    double score_warn = 3.0;
    double score_crit = 5.0;
    double confidence_formal_base = 0.8;
    double confidence_source_base = 0.6;
    double confidence_shadow_base = 0.5;

    double value_shadow_confidence_cap = 0.8;
    double value_shadow_sigma_scale = 1.5;
    double ratio_shadow_confidence_cap = 0.8;
    double ratio_shadow_score_scale = 1.5;

    double candidate_huber_delta = 1.5;
    double candidate_shadow_alpha = 0.2;
    double candidate_ratio_variance_floor = 0.25;
    double candidate_switch_loss_abs_tol = 1e-12;
    std::size_t candidate_min_train_point_count = 2;
    std::size_t relation_min_replay_for_holdout = 3;
    std::size_t relation_switch_validation_tail = 16;

    double key_fusion_persistence_window = 3.0;
    std::size_t key_fusion_window_limit = 2;

    double relation_pattern_lambda_sup = 0.5;
    double relation_pattern_lambda_opp = 0.5;
    double relation_pattern_persistence_window = 3.0;
};

SharedProfileConfig BuildDefaultSharedProfileConfigRaw() {
    SharedProfileConfig config;
    config.k_day = 4;
    config.k_week = 3;
    config.dme_max = 7;
    config.m_month_enable = 4;
    config.month_cov_min = 0.8;
    config.lambda_season = 1.0;
    config.lambda_dom = 4.0;
    config.lambda_dme = 2.0;
    config.lambda_lwd = 1.0;
    config.lambda_event = 2.0;
    config.n_val_switch = 16;
    config.eps_switch = 0.05;
    config.z_warn = 3.0;
    config.z_crit = 5.0;
    config.w_shift = 0.8;

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

std::unordered_map<std::string, ValueSampledProfileConfig> BuildDefaultValueSampledProfiles() {
    std::unordered_map<std::string, ValueSampledProfileConfig> profiles;
    ValueSampledProfileConfig cont_core;
    cont_core.name = "cont_core";
    cont_core.n_train_min = 50;
    cont_core.transform_name_override = "log1p";
    profiles.emplace(cont_core.name, cont_core);

    ValueSampledProfileConfig cont_tail;
    cont_tail.name = "cont_tail";
    cont_tail.n_train_min = 100;
    cont_tail.transform_name_override = "log1p";
    profiles.emplace(cont_tail.name, cont_tail);
    return profiles;
}

std::unordered_map<std::string, RatioProfileConfig> BuildDefaultRatioProfiles(
    const RatioGlobalNumericalConfig& global) {
    std::unordered_map<std::string, RatioProfileConfig> profiles;

    RatioProfileConfig rate_core;
    rate_core.name = "rate_core";
    rate_core.d_min_train = 50;
    rate_core.s_prior = 2.0;
    rate_core.phi_over = 1.5;
    rate_core.eps_logit = global.eps_logit;
    rate_core.m_floor = global.m_floor;
    rate_core.v_floor = global.v_floor;
    profiles.emplace(rate_core.name, rate_core);

    RatioProfileConfig ratio_bursty;
    ratio_bursty.name = "ratio_bursty";
    ratio_bursty.d_min_train = 100;
    ratio_bursty.s_prior = 4.0;
    ratio_bursty.phi_over = 2.0;
    ratio_bursty.eps_logit = global.eps_logit;
    ratio_bursty.m_floor = global.m_floor;
    ratio_bursty.v_floor = global.v_floor;
    profiles.emplace(ratio_bursty.name, ratio_bursty);

    return profiles;
}

RuntimeConfigState BuildDefaultRuntimeConfigState() {
    RuntimeConfigState state;
    state.shared_profile = BuildDefaultSharedProfileConfigRaw();
    state.value_sampled_profiles = BuildDefaultValueSampledProfiles();
    state.ratio_profiles = BuildDefaultRatioProfiles(state.ratio_global);
    return state;
}

std::shared_ptr<const RuntimeConfigState>& SnapshotRef() {
    static std::shared_ptr<const RuntimeConfigState> snapshot =
        std::make_shared<const RuntimeConfigState>(BuildDefaultRuntimeConfigState());
    return snapshot;
}

std::shared_ptr<const RuntimeConfigState> Snapshot() {
    return std::atomic_load_explicit(&SnapshotRef(), std::memory_order_acquire);
}

void StoreSnapshot(const RuntimeConfigState& state) {
    auto next = std::make_shared<const RuntimeConfigState>(state);
    std::atomic_store_explicit(&SnapshotRef(), std::move(next), std::memory_order_release);
}

template <typename T>
void ParseOptionalScalar(const YAML::Node& node, const char* key, T* out) {
    if (!out || !node || !node[key]) return;
    *out = node[key].as<T>();
}

void ParseSharedProfileConfig(const YAML::Node& node, SharedProfileConfig* out) {
    if (!node || !out) return;
    ParseOptionalScalar(node, "k_day", &out->k_day);
    ParseOptionalScalar(node, "k_week", &out->k_week);
    ParseOptionalScalar(node, "dme_max", &out->dme_max);
    ParseOptionalScalar(node, "m_month_enable", &out->m_month_enable);
    ParseOptionalScalar(node, "month_cov_min", &out->month_cov_min);
    ParseOptionalScalar(node, "lambda_season", &out->lambda_season);
    ParseOptionalScalar(node, "lambda_dom", &out->lambda_dom);
    ParseOptionalScalar(node, "lambda_dme", &out->lambda_dme);
    ParseOptionalScalar(node, "lambda_lwd", &out->lambda_lwd);
    ParseOptionalScalar(node, "lambda_event", &out->lambda_event);
    ParseOptionalScalar(node, "n_val_switch", &out->n_val_switch);
    ParseOptionalScalar(node, "eps_switch", &out->eps_switch);
    ParseOptionalScalar(node, "z_warn", &out->z_warn);
    ParseOptionalScalar(node, "z_crit", &out->z_crit);
    ParseOptionalScalar(node, "w_shift", &out->w_shift);
    if (node["drift"]) {
        const YAML::Node drift = node["drift"];
        ParseOptionalScalar(drift, "alpha", &out->drift.alpha);
        ParseOptionalScalar(drift, "shift_clip", &out->drift.shift_clip);
        ParseOptionalScalar(drift, "lambda_mem", &out->drift.lambda_mem);
        ParseOptionalScalar(drift, "kappa_shift", &out->drift.kappa_shift);
        ParseOptionalScalar(drift, "u_min", &out->drift.u_min);
        ParseOptionalScalar(drift, "h_shift", &out->drift.h_shift);
        ParseOptionalScalar(drift, "p_shift_low", &out->drift.p_shift_low);
        ParseOptionalScalar(drift, "p_shift_high", &out->drift.p_shift_high);
        ParseOptionalScalar(drift, "m_shift", &out->drift.m_shift);
        ParseOptionalScalar(drift, "g_skip", &out->drift.g_skip);
        ParseOptionalScalar(drift, "g_reset", &out->drift.g_reset);
    }
}

void ParseValueSampledProfiles(const YAML::Node& node, RuntimeConfigState* out) {
    if (!node || !out) return;
    out->value_sampled_profiles.clear();
    for (auto it = node.begin(); it != node.end(); ++it) {
        const std::string profile_name = it->first.as<std::string>();
        const YAML::Node profile_node = it->second;
        if (!profile_node || !profile_node.IsMap()) continue;

        ValueSampledProfileConfig profile;
        profile.name = profile_name;
        ParseOptionalScalar(profile_node, "n_train_min", &profile.n_train_min);
        ParseOptionalScalar(
            profile_node, "transform_name_override", &profile.transform_name_override);
        out->value_sampled_profiles[profile_name] = std::move(profile);
    }
}

void ParseRatioProfiles(const YAML::Node& node, RuntimeConfigState* out) {
    if (!node || !out) return;
    if (node["global"]) {
        const YAML::Node global = node["global"];
        ParseOptionalScalar(global, "eps_logit", &out->ratio_global.eps_logit);
        ParseOptionalScalar(global, "m_floor", &out->ratio_global.m_floor);
        ParseOptionalScalar(global, "v_floor", &out->ratio_global.v_floor);
    }

    out->ratio_profiles.clear();
    for (auto it = node.begin(); it != node.end(); ++it) {
        const std::string profile_name = it->first.as<std::string>();
        if (profile_name == "global") continue;

        const YAML::Node profile_node = it->second;
        if (!profile_node || !profile_node.IsMap()) continue;

        RatioProfileConfig profile;
        profile.name = profile_name;
        profile.eps_logit = out->ratio_global.eps_logit;
        profile.m_floor = out->ratio_global.m_floor;
        profile.v_floor = out->ratio_global.v_floor;
        ParseOptionalScalar(profile_node, "d_min_train", &profile.d_min_train);
        ParseOptionalScalar(profile_node, "s_prior", &profile.s_prior);
        ParseOptionalScalar(profile_node, "phi_over", &profile.phi_over);
        out->ratio_profiles[profile_name] = std::move(profile);
    }
}

void ParseBlockSolverConfig(const YAML::Node& node, BlockSolverConfig* out) {
    if (!node || !out) return;
    ParseOptionalScalar(node, "solver_name", &out->solver_name);
    ParseOptionalScalar(node, "c_huber", &out->c_huber);
    ParseOptionalScalar(node, "s_min_fit", &out->s_min_fit);
    ParseOptionalScalar(node, "max_iter_fit", &out->max_iter_fit);
    ParseOptionalScalar(node, "tol_obj_rel", &out->tol_obj_rel);
    ParseOptionalScalar(node, "tol_beta_inf", &out->tol_beta_inf);
    ParseOptionalScalar(node, "cond_max", &out->cond_max);
}

void ParseRuntimeAndRebuild(const YAML::Node& node, RuntimeConfigState* out) {
    if (!node || !out) return;
    if (node["runtime_state_prune"]) {
        const YAML::Node prune = node["runtime_state_prune"];
        ParseOptionalScalar(prune, "idle_bucket_gap", &out->runtime_idle_prune_bucket_gap);
        ParseOptionalScalar(prune, "prune_scan_limit", &out->runtime_idle_prune_scan_limit);
    }
    if (node["candidate_builder"]) {
        ParseOptionalScalar(node["candidate_builder"],
                            "min_train_point_count",
                            &out->candidate_min_train_point_count);
    }
    if (node["candidate_validator"]) {
        const YAML::Node validator = node["candidate_validator"];
        ParseOptionalScalar(validator, "huber_delta", &out->candidate_huber_delta);
        ParseOptionalScalar(validator, "shadow_alpha", &out->candidate_shadow_alpha);
        ParseOptionalScalar(
            validator, "ratio_variance_floor", &out->candidate_ratio_variance_floor);
        ParseOptionalScalar(
            validator, "switch_loss_abs_tol", &out->candidate_switch_loss_abs_tol);
    }
    if (node["relation_rebuild"]) {
        const YAML::Node relation = node["relation_rebuild"];
        ParseOptionalScalar(
            relation, "min_replay_for_holdout", &out->relation_min_replay_for_holdout);
        ParseOptionalScalar(
            relation, "switch_validation_tail", &out->relation_switch_validation_tail);
    }
}

void ParseScoringAndConfidence(const YAML::Node& node, RuntimeConfigState* out) {
    if (!node || !out) return;
    ParseOptionalScalar(node, "score_warn", &out->score_warn);
    ParseOptionalScalar(node, "score_crit", &out->score_crit);
    ParseOptionalScalar(node, "confidence_formal_base", &out->confidence_formal_base);
    ParseOptionalScalar(node, "confidence_source_base", &out->confidence_source_base);
    ParseOptionalScalar(node, "confidence_shadow_base", &out->confidence_shadow_base);
    ParseOptionalScalar(
        node, "value_shadow_confidence_cap", &out->value_shadow_confidence_cap);
    ParseOptionalScalar(node, "value_shadow_sigma_scale", &out->value_shadow_sigma_scale);
    ParseOptionalScalar(
        node, "ratio_shadow_confidence_cap", &out->ratio_shadow_confidence_cap);
    ParseOptionalScalar(node, "ratio_shadow_score_scale", &out->ratio_shadow_score_scale);
}

void ParseFusionConfig(const YAML::Node& node, RuntimeConfigState* out) {
    if (!node || !out) return;
    if (node["key_risk_fusion"]) {
        const YAML::Node key = node["key_risk_fusion"];
        ParseOptionalScalar(
            key, "fuse_persistence_window", &out->key_fusion_persistence_window);
        ParseOptionalScalar(key, "window_limit", &out->key_fusion_window_limit);
    }
    if (node["relation_pattern_fusion"]) {
        const YAML::Node relation = node["relation_pattern_fusion"];
        ParseOptionalScalar(relation, "lambda_sup", &out->relation_pattern_lambda_sup);
        ParseOptionalScalar(relation, "lambda_opp", &out->relation_pattern_lambda_opp);
        ParseOptionalScalar(
            relation, "fuse_persistence_window", &out->relation_pattern_persistence_window);
    }
}

bool IsUnitInterval(double value) {
    return value >= 0.0 && value <= 1.0;
}

bool ValidateAllowedKeys(const YAML::Node& node,
                         std::initializer_list<const char*> allowed,
                         const char* path,
                         std::string* err) {
    if (!node) return true;
    if (!node.IsMap()) {
        if (err) *err = std::string(path) + " must be an object";
        return false;
    }
    const std::unordered_set<std::string> allowed_keys(allowed.begin(), allowed.end());
    for (auto it = node.begin(); it != node.end(); ++it) {
        if (!it->first.IsScalar()) {
            if (err) *err = std::string(path) + " contains non-scalar key";
            return false;
        }
        const std::string key = it->first.as<std::string>();
        if (allowed_keys.find(key) != allowed_keys.end()) continue;
        if (err) *err = std::string(path) + "." + key + " is not allowed in strict mode";
        return false;
    }
    return true;
}

bool ValidateNamedProfileMapKeys(const YAML::Node& node,
                                 std::initializer_list<const char*> profile_allowed_keys,
                                 const char* path,
                                 std::string* err) {
    if (!node) return true;
    if (!node.IsMap()) {
        if (err) *err = std::string(path) + " must be an object";
        return false;
    }
    const std::unordered_set<std::string> allowed_keys(
        profile_allowed_keys.begin(), profile_allowed_keys.end());
    for (auto it = node.begin(); it != node.end(); ++it) {
        if (!it->first.IsScalar()) {
            if (err) *err = std::string(path) + " contains non-scalar profile name";
            return false;
        }
        const std::string profile_name = it->first.as<std::string>();
        const YAML::Node profile = it->second;
        if (!profile || !profile.IsMap()) {
            if (err) *err = std::string(path) + "." + profile_name + " must be an object";
            return false;
        }
        for (auto pit = profile.begin(); pit != profile.end(); ++pit) {
            if (!pit->first.IsScalar()) {
                if (err) {
                    *err = std::string(path) + "." + profile_name +
                           " contains non-scalar key";
                }
                return false;
            }
            const std::string key = pit->first.as<std::string>();
            if (allowed_keys.find(key) != allowed_keys.end()) continue;
            if (err) {
                *err = std::string(path) + "." + profile_name + "." + key +
                       " is not allowed in strict mode";
            }
            return false;
        }
    }
    return true;
}

bool ValidateStrictDefaultsSchema(const YAML::Node& defaults, std::string* err) {
    if (!ValidateAllowedKeys(defaults,
                             {"shared_profile_config",
                              "value_sampled_profiles",
                              "ratio_profiles",
                              "solver_constants",
                              "runtime_and_rebuild_constants",
                              "scoring_and_confidence_constants",
                              "fusion_constants",
                              "parser_level_defaults"},
                             "builtin_defaults",
                             err)) {
        return false;
    }

    if (!ValidateAllowedKeys(defaults["parser_level_defaults"], {"tz_default"}, "parser_level_defaults", err)) {
        return false;
    }
    if (!ValidateAllowedKeys(defaults["shared_profile_config"],
                             {"k_day",
                              "k_week",
                              "dme_max",
                              "m_month_enable",
                              "month_cov_min",
                              "lambda_season",
                              "lambda_dom",
                              "lambda_dme",
                              "lambda_lwd",
                              "lambda_event",
                              "n_val_switch",
                              "eps_switch",
                              "z_warn",
                              "z_crit",
                              "w_shift",
                              "drift"},
                             "shared_profile_config",
                             err)) {
        return false;
    }
    if (!ValidateAllowedKeys(defaults["shared_profile_config"]["drift"],
                             {"alpha",
                              "shift_clip",
                              "lambda_mem",
                              "kappa_shift",
                              "u_min",
                              "h_shift",
                              "p_shift_low",
                              "p_shift_high",
                              "m_shift",
                              "g_skip",
                              "g_reset"},
                             "shared_profile_config.drift",
                             err)) {
        return false;
    }
    if (!ValidateNamedProfileMapKeys(
            defaults["value_sampled_profiles"], {"n_train_min", "transform_name_override"}, "value_sampled_profiles", err)) {
        return false;
    }
    if (defaults["ratio_profiles"]) {
        if (!defaults["ratio_profiles"].IsMap()) {
            if (err) *err = "ratio_profiles must be an object";
            return false;
        }
        for (auto it = defaults["ratio_profiles"].begin();
             it != defaults["ratio_profiles"].end();
             ++it) {
            if (!it->first.IsScalar()) {
                if (err) *err = "ratio_profiles contains non-scalar profile name";
                return false;
            }
            const std::string profile_name = it->first.as<std::string>();
            const std::string profile_path = std::string("ratio_profiles.") + profile_name;
            if (profile_name == "global") {
                if (!ValidateAllowedKeys(it->second,
                                         {"eps_logit", "m_floor", "v_floor"},
                                         profile_path.c_str(),
                                         err)) {
                    return false;
                }
                continue;
            }
            if (!ValidateAllowedKeys(
                    it->second, {"d_min_train", "s_prior", "phi_over"}, profile_path.c_str(), err)) {
                return false;
            }
        }
    }
    if (!ValidateAllowedKeys(defaults["solver_constants"],
                             {"solver_name",
                              "c_huber",
                              "s_min_fit",
                              "max_iter_fit",
                              "tol_obj_rel",
                              "tol_beta_inf",
                              "cond_max"},
                             "solver_constants",
                             err)) {
        return false;
    }
    if (!ValidateAllowedKeys(defaults["runtime_and_rebuild_constants"],
                             {"runtime_state_prune",
                              "candidate_builder",
                              "candidate_validator",
                              "relation_rebuild"},
                             "runtime_and_rebuild_constants",
                             err)) {
        return false;
    }
    if (!ValidateAllowedKeys(defaults["runtime_and_rebuild_constants"]["runtime_state_prune"],
                             {"idle_bucket_gap", "prune_scan_limit"},
                             "runtime_and_rebuild_constants.runtime_state_prune",
                             err)) {
        return false;
    }
    if (!ValidateAllowedKeys(defaults["runtime_and_rebuild_constants"]["candidate_builder"],
                             {"min_train_point_count"},
                             "runtime_and_rebuild_constants.candidate_builder",
                             err)) {
        return false;
    }
    if (!ValidateAllowedKeys(defaults["runtime_and_rebuild_constants"]["candidate_validator"],
                             {"huber_delta",
                              "shadow_alpha",
                              "ratio_variance_floor",
                              "switch_loss_abs_tol"},
                             "runtime_and_rebuild_constants.candidate_validator",
                             err)) {
        return false;
    }
    if (!ValidateAllowedKeys(defaults["runtime_and_rebuild_constants"]["relation_rebuild"],
                             {"min_replay_for_holdout", "switch_validation_tail"},
                             "runtime_and_rebuild_constants.relation_rebuild",
                             err)) {
        return false;
    }
    if (!ValidateAllowedKeys(defaults["scoring_and_confidence_constants"],
                             {"score_warn",
                              "score_crit",
                              "confidence_formal_base",
                              "confidence_source_base",
                              "confidence_shadow_base",
                              "value_shadow_confidence_cap",
                              "value_shadow_sigma_scale",
                              "ratio_shadow_confidence_cap",
                              "ratio_shadow_score_scale"},
                             "scoring_and_confidence_constants",
                             err)) {
        return false;
    }
    if (!ValidateAllowedKeys(defaults["fusion_constants"],
                             {"key_risk_fusion", "relation_pattern_fusion"},
                             "fusion_constants",
                             err)) {
        return false;
    }
    if (!ValidateAllowedKeys(defaults["fusion_constants"]["key_risk_fusion"],
                             {"fuse_persistence_window", "window_limit"},
                             "fusion_constants.key_risk_fusion",
                             err)) {
        return false;
    }
    if (!ValidateAllowedKeys(defaults["fusion_constants"]["relation_pattern_fusion"],
                             {"lambda_sup", "lambda_opp", "fuse_persistence_window"},
                             "fusion_constants.relation_pattern_fusion",
                             err)) {
        return false;
    }
    return true;
}

bool ValidateRuntimeConfig(const RuntimeConfigState& cfg, std::string* err) {
    if (cfg.tz_default.empty()) {
        if (err) *err = "parser_level_defaults.tz_default must not be empty";
        return false;
    }
    if (cfg.shared_profile.k_day < 0 || cfg.shared_profile.k_week < 0 ||
        cfg.shared_profile.dme_max < 0 || cfg.shared_profile.n_val_switch == 0) {
        if (err) *err = "shared_profile_config integer fields are invalid";
        return false;
    }
    if (!(cfg.shared_profile.month_cov_min > 0.0 && cfg.shared_profile.month_cov_min <= 1.0)) {
        if (err) *err = "shared_profile_config.month_cov_min must be in (0,1]";
        return false;
    }
    if (!(cfg.shared_profile.z_warn > 0.0 && cfg.shared_profile.z_crit > cfg.shared_profile.z_warn)) {
        if (err) *err = "shared_profile_config z_warn/z_crit are invalid";
        return false;
    }
    if (!IsUnitInterval(cfg.shared_profile.drift.alpha) ||
        !IsUnitInterval(cfg.shared_profile.drift.lambda_mem) ||
        !IsUnitInterval(cfg.shared_profile.drift.p_shift_low) ||
        !IsUnitInterval(cfg.shared_profile.drift.p_shift_high) ||
        cfg.shared_profile.drift.p_shift_low > cfg.shared_profile.drift.p_shift_high) {
        if (err) *err = "shared_profile_config.drift probability fields are invalid";
        return false;
    }
    if (cfg.value_sampled_profiles.empty()) {
        if (err) *err = "value_sampled_profiles must not be empty";
        return false;
    }
    for (const auto& entry : cfg.value_sampled_profiles) {
        if (entry.first.empty() || entry.second.n_train_min == 0) {
            if (err) *err = "value_sampled_profiles contains invalid profile";
            return false;
        }
    }
    if (cfg.ratio_profiles.empty()) {
        if (err) *err = "ratio_profiles must not be empty";
        return false;
    }
    if (!(cfg.ratio_global.eps_logit > 0.0 && cfg.ratio_global.eps_logit < 0.5) ||
        !(cfg.ratio_global.m_floor > 0.0 && cfg.ratio_global.m_floor < 0.5) ||
        cfg.ratio_global.v_floor <= 0.0) {
        if (err) *err = "ratio_profiles.global numerical fields are invalid";
        return false;
    }
    for (const auto& entry : cfg.ratio_profiles) {
        if (entry.first.empty() || entry.second.d_min_train == 0 || entry.second.s_prior < 0.0 ||
            entry.second.phi_over < 1.0) {
            if (err) *err = "ratio_profiles contains invalid profile";
            return false;
        }
    }
    if (cfg.runtime_idle_prune_bucket_gap <= 0 || cfg.runtime_idle_prune_scan_limit == 0) {
        if (err) *err = "runtime_state_prune values must be > 0";
        return false;
    }
    if (!(cfg.score_warn >= 0.0 && cfg.score_crit > cfg.score_warn)) {
        if (err) *err = "score_warn/score_crit are invalid";
        return false;
    }
    if (!IsUnitInterval(cfg.confidence_formal_base) || !IsUnitInterval(cfg.confidence_source_base) ||
        !IsUnitInterval(cfg.confidence_shadow_base) ||
        !IsUnitInterval(cfg.value_shadow_confidence_cap) ||
        !IsUnitInterval(cfg.ratio_shadow_confidence_cap)) {
        if (err) *err = "confidence fields must be in [0,1]";
        return false;
    }
    if (!(cfg.value_shadow_sigma_scale > 0.0 && cfg.ratio_shadow_score_scale > 0.0)) {
        if (err) *err = "shadow score scale must be > 0";
        return false;
    }
    if (!(cfg.candidate_huber_delta > 0.0 && cfg.candidate_shadow_alpha > 0.0 &&
          cfg.candidate_shadow_alpha <= 1.0 && cfg.candidate_ratio_variance_floor > 0.0 &&
          cfg.candidate_switch_loss_abs_tol >= 0.0 && cfg.candidate_min_train_point_count > 0)) {
        if (err) *err = "candidate validator constants are invalid";
        return false;
    }
    if (cfg.relation_min_replay_for_holdout == 0 || cfg.relation_switch_validation_tail == 0) {
        if (err) *err = "relation_rebuild constants are invalid";
        return false;
    }
    if (!(cfg.key_fusion_persistence_window > 0.0 && cfg.key_fusion_window_limit > 0 &&
          cfg.relation_pattern_persistence_window > 0.0)) {
        if (err) *err = "fusion persistence/window values are invalid";
        return false;
    }
    if (cfg.block_solver.solver_name.empty() || cfg.block_solver.c_huber <= 0.0 ||
        cfg.block_solver.s_min_fit <= 0.0 || cfg.block_solver.max_iter_fit == 0 ||
        cfg.block_solver.tol_obj_rel <= 0.0 || cfg.block_solver.tol_beta_inf <= 0.0 ||
        cfg.block_solver.cond_max <= 0.0) {
        if (err) *err = "solver_constants are invalid";
        return false;
    }
    return true;
}

bool ApplyYamlConfig(const YAML::Node& root,
                     bool strict,
                     RuntimeConfigState* out,
                     std::string* err) {
    if (!out) {
        if (err) *err = "runtime config output must not be null";
        return false;
    }

    if (strict) {
        if (!ValidateAllowedKeys(root,
                                 {"builtin_defaults",
                                  "task_templates",
                                  "common_optional_blocks",
                                  "validation_notes"},
                                 "root",
                                 err)) {
            return false;
        }
    }

    YAML::Node defaults = root["builtin_defaults"];
    if (!defaults) {
        if (strict) {
            if (err) *err = "missing object field: builtin_defaults";
            return false;
        }
        defaults = root;
    }
    if (!defaults || !defaults.IsMap()) {
        if (err) *err = "builtin_defaults must be an object";
        return false;
    }
    if (strict && !ValidateStrictDefaultsSchema(defaults, err)) return false;

    if (defaults["parser_level_defaults"]) {
        ParseOptionalScalar(defaults["parser_level_defaults"], "tz_default", &out->tz_default);
    }
    ParseSharedProfileConfig(defaults["shared_profile_config"], &out->shared_profile);
    ParseValueSampledProfiles(defaults["value_sampled_profiles"], out);
    ParseRatioProfiles(defaults["ratio_profiles"], out);
    ParseBlockSolverConfig(defaults["solver_constants"], &out->block_solver);
    ParseRuntimeAndRebuild(defaults["runtime_and_rebuild_constants"], out);
    ParseScoringAndConfidence(defaults["scoring_and_confidence_constants"], out);
    ParseFusionConfig(defaults["fusion_constants"], out);
    return true;
}

}  // namespace

int LoadBaselineRuntimeConfigFromYaml(const std::string& file_path,
                                      bool strict,
                                      std::string* err) {
    if (file_path.empty()) {
        ResetBaselineRuntimeConfig();
        return error::OK;
    }

    RuntimeConfigState next = BuildDefaultRuntimeConfigState();
    try {
        const YAML::Node root = YAML::LoadFile(file_path);
        if (!ApplyYamlConfig(root, strict, &next, err)) return error::BAD_REQUEST;
    } catch (const YAML::Exception& ex) {
        if (err) *err = ex.what();
        return error::BAD_REQUEST;
    } catch (const std::exception& ex) {
        if (err) *err = ex.what();
        return error::BAD_REQUEST;
    } catch (...) {
        if (err) *err = "unknown exception while loading baseline runtime config";
        return error::BAD_REQUEST;
    }

    if (!ValidateRuntimeConfig(next, err)) return error::BAD_REQUEST;
    StoreSnapshot(next);
    return error::OK;
}

void ResetBaselineRuntimeConfig() {
    StoreSnapshot(BuildDefaultRuntimeConfigState());
}

bool TryGetSharedProfileConfigOverride(SharedProfileConfig* out) {
    if (!out) return false;
    const auto snapshot = Snapshot();
    *out = snapshot->shared_profile;
    return true;
}

bool TryGetValueSampledProfileConfigOverride(const std::string& profile_name,
                                             ValueSampledProfileConfig* out) {
    if (!out || profile_name.empty()) return false;
    const auto snapshot = Snapshot();
    const auto& profiles = snapshot->value_sampled_profiles;
    const auto it = profiles.find(profile_name);
    if (it == profiles.end()) return false;
    *out = it->second;
    return true;
}

bool TryGetRatioProfileConfigOverride(const std::string& profile_name,
                                      RatioProfileConfig* out) {
    if (!out || profile_name.empty()) return false;
    const auto snapshot = Snapshot();
    const auto& profiles = snapshot->ratio_profiles;
    const auto it = profiles.find(profile_name);
    if (it == profiles.end()) return false;
    *out = it->second;
    return true;
}

bool TryGetRatioGlobalNumericalOverride(double* eps_logit,
                                        double* m_floor,
                                        double* v_floor) {
    const auto snapshot = Snapshot();
    const RatioGlobalNumericalConfig& global = snapshot->ratio_global;
    if (eps_logit) *eps_logit = global.eps_logit;
    if (m_floor) *m_floor = global.m_floor;
    if (v_floor) *v_floor = global.v_floor;
    return true;
}

bool TryGetBlockSolverConfigOverride(BlockSolverConfig* out) {
    if (!out) return false;
    const auto snapshot = Snapshot();
    *out = snapshot->block_solver;
    return true;
}

std::string BaselineDefaultTimezone() {
    const auto snapshot = Snapshot();
    return snapshot->tz_default;
}

int64_t RuntimeIdlePruneBucketGap() {
    const auto snapshot = Snapshot();
    return snapshot->runtime_idle_prune_bucket_gap;
}

std::size_t RuntimeIdlePruneScanLimit() {
    const auto snapshot = Snapshot();
    return snapshot->runtime_idle_prune_scan_limit;
}

double ScoreWarn() {
    const auto snapshot = Snapshot();
    return snapshot->score_warn;
}

double ScoreCrit() {
    const auto snapshot = Snapshot();
    return snapshot->score_crit;
}

double ConfidenceFormalBase() {
    const auto snapshot = Snapshot();
    return snapshot->confidence_formal_base;
}

double ConfidenceSourceBase() {
    const auto snapshot = Snapshot();
    return snapshot->confidence_source_base;
}

double ConfidenceShadowBase() {
    const auto snapshot = Snapshot();
    return snapshot->confidence_shadow_base;
}

double ValueShadowConfidenceCap() {
    const auto snapshot = Snapshot();
    return snapshot->value_shadow_confidence_cap;
}

double ValueShadowSigmaScale() {
    const auto snapshot = Snapshot();
    return snapshot->value_shadow_sigma_scale;
}

double RatioShadowConfidenceCap() {
    const auto snapshot = Snapshot();
    return snapshot->ratio_shadow_confidence_cap;
}

double RatioShadowScoreScale() {
    const auto snapshot = Snapshot();
    return snapshot->ratio_shadow_score_scale;
}

double CandidateHuberDelta() {
    const auto snapshot = Snapshot();
    return snapshot->candidate_huber_delta;
}

double CandidateShadowAlpha() {
    const auto snapshot = Snapshot();
    return snapshot->candidate_shadow_alpha;
}

double CandidateRatioVarianceFloor() {
    const auto snapshot = Snapshot();
    return snapshot->candidate_ratio_variance_floor;
}

double CandidateSwitchLossAbsTol() {
    const auto snapshot = Snapshot();
    return snapshot->candidate_switch_loss_abs_tol;
}

std::size_t CandidateMinTrainPointCount() {
    const auto snapshot = Snapshot();
    return snapshot->candidate_min_train_point_count;
}

double KeyFusionPersistenceWindow() {
    const auto snapshot = Snapshot();
    return snapshot->key_fusion_persistence_window;
}

std::size_t KeyFusionWindowLimit() {
    const auto snapshot = Snapshot();
    return snapshot->key_fusion_window_limit;
}

double RelationPatternLambdaSup() {
    const auto snapshot = Snapshot();
    return snapshot->relation_pattern_lambda_sup;
}

double RelationPatternLambdaOpp() {
    const auto snapshot = Snapshot();
    return snapshot->relation_pattern_lambda_opp;
}

double RelationPatternPersistenceWindow() {
    const auto snapshot = Snapshot();
    return snapshot->relation_pattern_persistence_window;
}

std::size_t RelationMinReplayForHoldout() {
    const auto snapshot = Snapshot();
    return snapshot->relation_min_replay_for_holdout;
}

std::size_t RelationSwitchValidationTail() {
    const auto snapshot = Snapshot();
    return snapshot->relation_switch_validation_tail;
}

}  // namespace baseline
}  // namespace flowsql
