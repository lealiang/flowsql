/*
 * Copyright (C) 2026 LIHUO
 *
 * Licensed under the MIT License. See LICENSE file in the project root
 * for full license information.
 *
 */

#include "plugins/baseline/fusion/key_risk_fusion.h"

#include <common/error_code.h>

#include <algorithm>
#include <functional>
#include <rapidjson/stringbuffer.h>
#include <rapidjson/writer.h>

namespace flowsql {
namespace baseline {

namespace {

constexpr double kFusePersistenceWindow = 3.0;
constexpr size_t kWindowLimit = 2;

double ClipUnit(double value) {
    return std::max(0.0, std::min(1.0, value));
}

double ComputeEvidenceStrength(const DetectorResult& result) {
    const double persistence_ratio =
        std::min(1.0, static_cast<double>(result.persistence) / kFusePersistenceWindow);
    return ClipUnit(result.normalized_score) * ClipUnit(result.confidence) * persistence_ratio;
}

std::string CopyStringRef(const BaselineStringRef& ref) {
    if (!ref.data || ref.size == 0) return "";
    return std::string(ref.data, ref.size);
}

void WriteStoredFusionResultJson(const StoredFusionResult& result,
                                 rapidjson::Writer<rapidjson::StringBuffer>* writer) {
    if (!writer) return;

    writer->StartObject();
    writer->Key("available");
    writer->Bool(result.available);
    writer->Key("ts");
    writer->Int64(result.ts);
    writer->Key("risk");
    writer->Double(result.risk);
    writer->Key("dominant_single_count");
    writer->Uint(static_cast<uint32_t>(result.dominant_singles.size()));
    writer->Key("dominant_single");
    writer->StartArray();
    for (const auto& single : result.dominant_singles) {
        writer->StartObject();
        writer->Key("feature");
        writer->String(single.feature.c_str());
        writer->Key("dir");
        writer->Int(static_cast<int32_t>(single.dir));
        writer->Key("reason_code");
        writer->Int(static_cast<int32_t>(single.reason_code));
        writer->Key("a_f");
        writer->Double(single.a_f);
        writer->Key("normalized_score");
        writer->Double(single.normalized_score);
        writer->Key("confidence");
        writer->Double(single.confidence);
        writer->Key("persistence");
        writer->Uint(single.persistence);
        writer->EndObject();
    }
    writer->EndArray();
    writer->Key("dominant_pattern_count");
    writer->Uint(static_cast<uint32_t>(result.dominant_patterns.size()));
    writer->Key("dominant_pattern");
    writer->StartArray();
    for (const auto& pattern : result.dominant_patterns) {
        writer->StartObject();
        writer->Key("pattern");
        writer->String(pattern.pattern.c_str());
        writer->Key("feature_base");
        writer->String(pattern.feature_base.c_str());
        writer->Key("score_pattern");
        writer->Double(pattern.score_pattern);
        writer->Key("metrics_hit");
        writer->StartArray();
        for (const auto& metric : pattern.metrics_hit) writer->String(metric.c_str());
        writer->EndArray();
        writer->Key("supporting_features");
        writer->StartArray();
        for (const auto& feature : pattern.supporting_features) {
            writer->String(feature.c_str());
        }
        writer->EndArray();
        writer->EndObject();
    }
    writer->EndArray();
    writer->EndObject();
}

double SaturatedNoisyOr(const std::vector<double>& values) {
    if (values.empty()) return 0.0;
    double product = 1.0;
    for (double value : values) {
        const double clipped = ClipUnit(value);
        product *= (1.0 - clipped);
    }
    return 1.0 - product;
}

KeyRiskFusion::KeyRiskWindowState* FindWindow(
    KeyRiskFusion::KeyRiskFusionState* state,
    int64_t ts) {
    if (!state) return nullptr;
    for (auto& window : state->windows) {
        if (window.bucket_id == ts) return &window;
    }
    return nullptr;
}

const KeyRiskFusion::KeyRiskWindowState* FindLatestFinalizedWindow(
    const KeyRiskFusion::KeyRiskFusionState& state) {
    for (auto it = state.windows.rbegin(); it != state.windows.rend(); ++it) {
        if (it->finalized) return &(*it);
    }
    return nullptr;
}

const KeyRiskFusion::KeyRiskWindowState* FindActiveWindow(
    const KeyRiskFusion::KeyRiskFusionState& state) {
    for (auto it = state.windows.rbegin(); it != state.windows.rend(); ++it) {
        if (!it->finalized) return &(*it);
    }
    return nullptr;
}

KeyRiskFusion::KeyRiskWindowState* FindOrCreateWindow(
    KeyRiskFusion::KeyRiskFusionState* state,
    int64_t ts) {
    if (!state) return nullptr;
    if (KeyRiskFusion::KeyRiskWindowState* existing = FindWindow(state, ts)) {
        return existing;
    }

    if (state->windows.empty()) {
        state->windows.push_back(KeyRiskFusion::KeyRiskWindowState{});
        state->windows.back().bucket_id = ts;
        return &state->windows.back();
    }

    KeyRiskFusion::KeyRiskWindowState& newest = state->windows.back();
    if (ts > newest.bucket_id) {
        newest.finalized = true;
        state->windows.push_back(KeyRiskFusion::KeyRiskWindowState{});
        state->windows.back().bucket_id = ts;
        if (state->windows.size() > kWindowLimit) {
            state->windows.erase(state->windows.begin());
        }
        return &state->windows.back();
    }

    if (ts < newest.bucket_id && state->windows.size() == 1) {
        state->windows.insert(state->windows.begin(), KeyRiskFusion::KeyRiskWindowState{});
        state->windows.front().bucket_id = ts;
        state->windows.front().finalized = true;
        return &state->windows.front();
    }

    return nullptr;
}

void RecomputeWindowRisk(const std::string& key,
                         KeyRiskFusion::KeyRiskWindowState* window) {
    if (!window) return;

    std::vector<double> direct_scores;
    std::vector<double> routed_scores;
    std::vector<double> pattern_scores;
    std::vector<StoredDominantSingleProjection> dominant_singles;
    std::vector<StoredDominantPatternProjection> dominant_patterns;

    dominant_singles.reserve(window->single_results_by_source_id.size());
    for (const auto& entry : window->single_results_by_source_id) {
        const auto& source_id = entry.first;
        const auto& single = entry.second;
        if (source_id.source_kind == FusionSourceKind::kDirectSingle) {
            direct_scores.push_back(single.effective_score);
        } else if (source_id.source_kind == FusionSourceKind::kRoutedSingle) {
            routed_scores.push_back(single.effective_score);
        }
        if (single.effective_score > 0.0) {
            dominant_singles.push_back(single.projection);
        }
    }

    std::sort(dominant_singles.begin(), dominant_singles.end(),
              [](const auto& lhs, const auto& rhs) {
                  return lhs.a_f > rhs.a_f;
              });
    if (dominant_singles.size() > kBaselineDominantSingleLimit) {
        dominant_singles.resize(kBaselineDominantSingleLimit);
    }

    dominant_patterns.reserve(window->relation_fusions_by_source_id.size());
    for (const auto& entry : window->relation_fusions_by_source_id) {
        const auto& pattern = entry.second.projection;
        pattern_scores.push_back(pattern.weighted_score);
        if (pattern.weighted_score > 0.0) dominant_patterns.push_back(pattern);
    }
    std::sort(dominant_patterns.begin(), dominant_patterns.end(),
              [](const auto& lhs, const auto& rhs) {
                  return lhs.weighted_score > rhs.weighted_score;
              });
    if (dominant_patterns.size() > kBaselineDominantPatternLimit) {
        dominant_patterns.resize(kBaselineDominantPatternLimit);
    }

    const double risk_t1t2 = SaturatedNoisyOr(direct_scores);
    const double risk_single_t3 = SaturatedNoisyOr(routed_scores);
    const double risk_pattern = SaturatedNoisyOr(pattern_scores);
    const double risk_t3 = 1.0 - (1.0 - risk_single_t3) * (1.0 - risk_pattern);

    window->key_risk.available = true;
    window->key_risk.key = key;
    window->key_risk.ts = window->bucket_id;
    window->key_risk.risk = 1.0 - (1.0 - risk_t1t2) * (1.0 - risk_t3);
    window->key_risk.dominant_singles = std::move(dominant_singles);
    window->key_risk.dominant_patterns = std::move(dominant_patterns);
}

bool WindowEmpty(const KeyRiskFusion::KeyRiskWindowState& window) {
    return window.single_results_by_source_id.empty() &&
           window.relation_fusions_by_source_id.empty();
}

void PruneEmptyWindows(KeyRiskFusion::KeyRiskFusionState* state) {
    if (!state) return;
    state->windows.erase(
        std::remove_if(state->windows.begin(),
                       state->windows.end(),
                       [](const auto& window) { return WindowEmpty(window); }),
        state->windows.end());
    if (state->windows.size() > kWindowLimit) {
        state->windows.erase(state->windows.begin(),
                             state->windows.begin() +
                                 static_cast<std::ptrdiff_t>(state->windows.size() - kWindowLimit));
    }
}

}  // namespace

std::size_t KeyRiskFusion::FusionSourceIdHash::operator()(
    const FusionSourceId& value) const {
    std::size_t seed = std::hash<std::string>{}(value.task_id);
    seed ^= static_cast<std::size_t>(value.source_kind) + 0x9e3779b9 + (seed << 6U) +
            (seed >> 2U);
    seed ^= static_cast<std::size_t>(value.local_slot) + 0x9e3779b9 + (seed << 6U) +
            (seed >> 2U);
    return seed;
}

size_t KeyRiskFusion::ShardIndex(const std::string& key) const {
    return std::hash<std::string>{}(key) % shards_.size();
}

void KeyRiskFusion::UpdateSingleDetectorResult(int64_t ts,
                                               const FusionSourceId& source_id,
                                               const DetectorResult& result) {
    const std::string key = CopyStringRef(result.key);
    if (key.empty()) return;

    ShardState& shard = shards_[ShardIndex(key)];
    std::lock_guard<std::mutex> lock(shard.mutex);

    KeyRiskFusionState& state = shard.states[key];
    KeyRiskWindowState* window = FindOrCreateWindow(&state, ts);
    if (!window) return;

    StoredSingleContribution stored;
    stored.effective_score = ComputeEvidenceStrength(result);
    stored.projection.feature = CopyStringRef(result.feature);
    stored.projection.dir = result.direction;
    stored.projection.reason_code = result.reason_code;
    stored.projection.a_f = stored.effective_score;
    stored.projection.normalized_score = result.normalized_score;
    stored.projection.confidence = result.confidence;
    stored.projection.persistence = result.persistence;
    window->single_results_by_source_id[source_id] = std::move(stored);

    RecomputeWindowRisk(key, window);
}

void KeyRiskFusion::UpdateRelationFusionResult(int64_t ts,
                                               const FusionSourceId& source_id,
                                               const FusionResult& result) {
    const std::string key = CopyStringRef(result.key);
    if (key.empty() || result.dominant_pattern_count == 0) return;

    ShardState& shard = shards_[ShardIndex(key)];
    std::lock_guard<std::mutex> lock(shard.mutex);

    KeyRiskFusionState& state = shard.states[key];
    KeyRiskWindowState* window = FindOrCreateWindow(&state, ts);
    if (!window) return;

    const DominantPatternProjection& pattern = result.dominant_pattern[0];

    StoredPatternContribution stored;
    stored.projection.pattern = CopyStringRef(pattern.pattern);
    stored.projection.feature_base = CopyStringRef(pattern.feature_base);
    stored.projection.score_pattern = pattern.score_pattern;
    stored.projection.weighted_score = result.risk;
    for (uint32_t i = 0; i < pattern.metrics_hit_count; ++i) {
        stored.projection.metrics_hit.push_back(CopyStringRef(pattern.metrics_hit[i]));
    }
    for (uint32_t i = 0; i < pattern.supporting_feature_count; ++i) {
        stored.projection.supporting_features.push_back(
            CopyStringRef(pattern.supporting_features[i]));
    }

    window->relation_fusions_by_source_id[source_id] = std::move(stored);
    RecomputeWindowRisk(key, window);
}

void KeyRiskFusion::RemoveTaskContributions(const std::string& task_id) {
    if (task_id.empty()) return;

    for (auto& shard : shards_) {
        std::lock_guard<std::mutex> lock(shard.mutex);
        for (auto it = shard.states.begin(); it != shard.states.end();) {
            KeyRiskFusionState& state = it->second;
            for (auto& window : state.windows) {
                for (auto single_it = window.single_results_by_source_id.begin();
                     single_it != window.single_results_by_source_id.end();) {
                    if (single_it->first.task_id == task_id) {
                        single_it = window.single_results_by_source_id.erase(single_it);
                    } else {
                        ++single_it;
                    }
                }
                for (auto pattern_it = window.relation_fusions_by_source_id.begin();
                     pattern_it != window.relation_fusions_by_source_id.end();) {
                    if (pattern_it->first.task_id == task_id) {
                        pattern_it = window.relation_fusions_by_source_id.erase(pattern_it);
                    } else {
                        ++pattern_it;
                    }
                }
                RecomputeWindowRisk(it->first, &window);
            }

            PruneEmptyWindows(&state);
            if (state.windows.empty()) {
                it = shard.states.erase(it);
            } else {
                ++it;
            }
        }
    }
}

int KeyRiskFusion::QueryKeyFusionSnapshot(const std::string& key,
                                          KeyRiskFusionSnapshot* out_snapshot) const {
    if (!out_snapshot) return error::BAD_REQUEST;
    *out_snapshot = KeyRiskFusionSnapshot{};
    out_snapshot->key = key;
    if (key.empty()) return error::OK;

    const ShardState& shard = shards_[ShardIndex(key)];
    std::lock_guard<std::mutex> lock(shard.mutex);
    auto it = shard.states.find(key);
    if (it == shard.states.end()) return error::OK;

    out_snapshot->available = true;
    if (const auto* finalized = FindLatestFinalizedWindow(it->second)) {
        out_snapshot->latest_finalized_result = finalized->key_risk;
    }
    if (const auto* active = FindActiveWindow(it->second)) {
        out_snapshot->active_window = active->key_risk;
    }
    return error::OK;
}

void KeyRiskFusion::WriteSnapshotJson(const KeyRiskFusionSnapshot& snapshot,
                                      std::string* out_json) {
    if (!out_json) return;

    rapidjson::StringBuffer buf;
    rapidjson::Writer<rapidjson::StringBuffer> writer(buf);
    writer.StartObject();
    writer.Key("key");
    writer.String(snapshot.key.c_str());
    writer.Key("available");
    writer.Bool(snapshot.available);
    if (snapshot.latest_finalized_result.available) {
        writer.Key("latest_finalized_result");
        WriteStoredFusionResultJson(snapshot.latest_finalized_result, &writer);
    }
    if (snapshot.active_window.available) {
        writer.Key("active_window");
        WriteStoredFusionResultJson(snapshot.active_window, &writer);
    }
    writer.EndObject();
    *out_json = buf.GetString();
}

int KeyRiskFusion::QueryKeyFusionSnapshotJson(const BaselineStringRef& key,
                                              std::string* out_json) const {
    if (!out_json) return error::BAD_REQUEST;

    KeyRiskFusionSnapshot snapshot;
    const int rc = QueryKeyFusionSnapshot(CopyStringRef(key), &snapshot);
    if (rc != error::OK) return rc;
    WriteSnapshotJson(snapshot, out_json);
    return error::OK;
}

size_t KeyRiskFusion::KeyCount() const {
    size_t total = 0;
    for (const auto& shard : shards_) {
        std::lock_guard<std::mutex> lock(shard.mutex);
        total += shard.states.size();
    }
    return total;
}

}  // namespace baseline
}  // namespace flowsql
