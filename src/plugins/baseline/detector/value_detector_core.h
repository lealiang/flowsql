/*
 * Copyright (C) 2026 LIHUO
 *
 * Licensed under the MIT License. See LICENSE file in the project root
 * for full license information.
 *
 */

#ifndef _FLOWSQL_PLUGINS_BASELINE_DETECTOR_VALUE_DETECTOR_CORE_H_
#define _FLOWSQL_PLUGINS_BASELINE_DETECTOR_VALUE_DETECTOR_CORE_H_

#include <framework/interfaces/ibaseline_types.h>

#include <cmath>
#include <cstdint>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

#include "plugins/baseline/detector/detector_common.h"
#include "plugins/baseline/model/drift_state.h"
#include "plugins/baseline/model/event_calendar_spec.h"
#include "plugins/baseline/model/formal_model.h"
#include "plugins/baseline/model/formal_model_state.h"
#include "plugins/baseline/model/formal_predictor.h"
#include "plugins/baseline/model/series_override.h"
#include "plugins/baseline/model/series_state.h"
#include "plugins/baseline/model/series_store.h"
#include "plugins/baseline/model/shadow_state.h"
#include "plugins/baseline/rebuild/replay_runner.h"

namespace flowsql {
namespace baseline {

struct ValueFeatureProfile {
    std::string feature_type;
    std::string feature_profile;
    std::string transform_name = "log1p";
    bool is_t1b = false;
    uint32_t n_train_min = 0;
    uint32_t n_score_min = 0;
    uint32_t n_shift_min = 0;
    double kappa_sample = 0.0;
};

// Value detector 目前统一采用 log1p 变换。这里保留 profile 形参，
// 是为了后续按设计切换变换函数时，不需要回退到 task 层改调用链。
inline double TransformValueObservation(const ValueFeatureProfile& profile,
                                        double value) {
    (void)profile;
    return std::log1p(value);
}

struct ValueSeriesRuntimeState {
    uint64_t last_sample_count = 0;
    double last_value = 0.0;
    double last_x = 0.0;
    double last_rho = 1.0;
    bool last_gate_score = true;
    bool last_gate_shift = true;
    double last_p_shift = 0.0;
    bool last_shift_confirmed = false;
    std::string model_state = "cold_start";
    BaselineSourceDecision baseline_source;
    FormalModelState formal_state;
    DriftState drift_state;
    ValueShadowState shadow_state;
    bool shift_rebuild_pending = false;
    std::shared_ptr<ValueReplaySeries> candidate_replay;
    std::shared_ptr<ValueFormalModel> formal_model;
    std::shared_ptr<ValueFormalModel> candidate_model;
};

struct ValueDetectorCoreSpec {
    std::string owner_task_id;
    std::string routed_feature_id;
    std::string feature_type;
    std::string feature_profile;
    std::optional<EventCalendarSpec> event_calendar_spec;
    std::vector<SeriesOverride> series_overrides;
};

struct ValueSeriesSnapshot {
    SeriesState series_state;
    ValueSeriesRuntimeState runtime_state;
    FormalPrediction formal_prediction;
    int formal_predict_status = 0;
    FormalPrediction candidate_prediction;
    int candidate_predict_status = 0;
    bool formal_calendar_present = false;
    bool candidate_calendar_present = false;
};

struct ValueRebuildContext {
    uint64_t next_model_version = 1;
    std::shared_ptr<ValueFormalModel> incumbent_formal_model;
    ValueShadowState incumbent_shadow_state;
};

struct ValueApplyFormalModelResult {
    ReplayWindowSummary replay_window;
    ReplayWindowSummary train_window;
    ReplayWindowSummary holdout_window;
    double candidate_loss = 0.0;
    double incumbent_loss = 0.0;
    uint64_t validation_count = 0;
    bool candidate_trained = false;
    uint64_t candidate_generation = 0;
    std::string candidate_state = "none";
    std::string switch_state = "none";
    bool replace_formal_model = false;
    std::shared_ptr<ValueFormalModel> full_model;
};

class ValueDetectorCore {
 public:
    explicit ValueDetectorCore(const ValueDetectorCoreSpec& spec);

    int Submit(const ValueObservation& obs, DetectorSubmitOutput* out_submit);
    int BuildSeriesSnapshot(const BaselineStringRef& key,
                            ValueSeriesSnapshot* out_snapshot) const;
    int GetSeriesState(const BaselineStringRef& key, SeriesState* out_state) const;
    int BuildRebuildContext(const std::string& key, ValueRebuildContext* out_context) const;
    void ApplyFormalModel(const std::string& key,
                          const ValueApplyFormalModelResult& apply_result);
    void MarkRebuildFailure(const DetectorRebuildFailure& failure);
    void ClearPendingRebuild(const std::string& key);
    void Clear();
    size_t Size() const;

    const ValueFeatureProfile& profile() const { return profile_; }
    const std::optional<EventCalendarSpec>& event_calendar_spec() const {
        return spec_.event_calendar_spec;
    }
    const std::string& routed_feature_id() const { return spec_.routed_feature_id; }

 private:
    ValueDetectorCoreSpec spec_;
    ValueFeatureProfile profile_;
    SeriesStore series_store_;
    mutable std::mutex runtime_mutex_;
    std::unordered_map<std::string, ValueSeriesRuntimeState> runtime_by_key_;
    std::unordered_map<std::string, BaselineSourceConfig> series_override_map_;
};

}  // namespace baseline
}  // namespace flowsql

#endif  // _FLOWSQL_PLUGINS_BASELINE_DETECTOR_VALUE_DETECTOR_CORE_H_
