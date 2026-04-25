/*
 * Copyright (C) 2026 LIHUO
 *
 * Licensed under the MIT License. See LICENSE file in the project root
 * for full license information.
 *
 */

#ifndef _FLOWSQL_PLUGINS_BASELINE_FUSION_KEY_RISK_FUSION_H_
#define _FLOWSQL_PLUGINS_BASELINE_FUSION_KEY_RISK_FUSION_H_

#include <array>
#include <atomic>
#include <cstdint>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

#include <framework/interfaces/ibaseline_types.h>

#include "plugins/baseline/fusion/fusion_types.h"

namespace flowsql {
namespace baseline {

enum class FusionSourceKind : int32_t {
    kDirectSingle = 0,
    kRoutedSingle = 1,
    kRelationPattern = 2,
};

struct FusionSourceId {
    std::string task_id;
    FusionSourceKind source_kind = FusionSourceKind::kDirectSingle;
    int32_t local_slot = 0;

    bool operator==(const FusionSourceId& other) const {
        return source_kind == other.source_kind &&
               local_slot == other.local_slot &&
               task_id == other.task_id;
    }
};

struct KeyRiskFusionSnapshot {
    std::string key;
    bool available = false;
    StoredFusionResult latest_finalized_result;
    StoredFusionResult active_window;
};

class KeyRiskFusion {
 public:
    struct FusionSourceIdHash {
        std::size_t operator()(const FusionSourceId& value) const;
    };

    struct StoredSingleContribution {
        StoredDominantSingleProjection projection;
        double effective_score = 0.0;
    };

    struct StoredPatternContribution {
        StoredDominantPatternProjection projection;
    };

    struct KeyRiskWindowState {
        int64_t bucket_id = 0;
        bool finalized = false;
        std::unordered_map<FusionSourceId,
                           StoredSingleContribution,
                           FusionSourceIdHash> single_results_by_source_id;
        std::unordered_map<FusionSourceId,
                           StoredPatternContribution,
                           FusionSourceIdHash> relation_fusions_by_source_id;
        StoredFusionResult key_risk;
    };

    struct KeyRiskFusionState {
        std::vector<KeyRiskWindowState> windows;
    };

    struct ShardState {
        mutable std::mutex mutex;
        std::unordered_map<std::string, KeyRiskFusionState> states;
        size_t prune_cursor = 0;
    };

    static constexpr size_t kShardCount = 64;

    KeyRiskFusion() = default;
    ~KeyRiskFusion() = default;

    void UpdateSingleDetectorResult(int64_t ts,
                                    const FusionSourceId& source_id,
                                    const DetectorResult& result);
    void UpdateRelationFusionResult(int64_t ts,
                                    const FusionSourceId& source_id,
                                    const FusionResult& result);
    void RemoveTaskContributions(const std::string& task_id);

    int QueryKeyFusionSnapshot(const std::string& key,
                               KeyRiskFusionSnapshot* out_snapshot) const;
    int QueryKeyFusionSnapshotJson(const BaselineStringRef& key,
                                   std::string* out_json) const;
    size_t KeyCount() const;
    uint64_t PrunedKeyCount() const;
    int64_t IdlePruneBucketGap() const;

 private:
    size_t ShardIndex(const std::string& key) const;
    static void WriteSnapshotJson(const KeyRiskFusionSnapshot& snapshot,
                                  std::string* out_json);

    std::array<ShardState, kShardCount> shards_;
    std::atomic<uint64_t> prune_cursor_{0};
    std::atomic<int64_t> last_pruned_bucket_{-1};
    std::atomic<uint64_t> pruned_key_count_total_{0};
};

}  // namespace baseline
}  // namespace flowsql

#endif  // _FLOWSQL_PLUGINS_BASELINE_FUSION_KEY_RISK_FUSION_H_
