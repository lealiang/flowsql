/*
 * Copyright (C) 2026 LIHUO
 *
 * Licensed under the MIT License. See LICENSE file in the project root
 * for full license information.
 *
 */

#ifndef _FLOWSQL_PLUGINS_BASELINE_BOOTSTRAP_BOOTSTRAP_ENGINE_H_
#define _FLOWSQL_PLUGINS_BASELINE_BOOTSTRAP_BOOTSTRAP_ENGINE_H_

#include <cstdint>
#include <string_view>

#include "bootstrap_types.h"
#include "plugins/baseline/model/task_spec.h"

namespace flowsql {
namespace baseline {

struct CompiledEventCalendar;

class __attribute__((visibility("default"))) BootstrapEngine {
 public:
    BootstrapTrainResult TrainValue(const BaselineTaskSpec& spec,
                                    const ValueBootstrapInput& input,
                                    BootstrapArtifact* out_artifact,
                                    const CompiledEventCalendar* compiled_event_calendar =
                                        nullptr) const;

    BootstrapTrainResult TrainRatio(const BaselineTaskSpec& spec,
                                    const RatioBootstrapInput& input,
                                    BootstrapArtifact* out_artifact,
                                    const CompiledEventCalendar* compiled_event_calendar =
                                        nullptr) const;

    BootstrapTrainResult TrainRelation(const RelationTaskCreateSpec& spec,
                                       const RelationBootstrapInput& input,
                                       BootstrapArtifact* out_artifact,
                                       const CompiledEventCalendar* compiled_event_calendar =
                                           nullptr) const;

    BootstrapPrediction PredictValue(const BootstrapArtifact& artifact,
                                     int64_t bucket_id,
                                     const BootstrapPredictionOptions& options,
                                     const BaselineTaskSpec* task_spec = nullptr,
                                     const CompiledEventCalendar* compiled_event_calendar =
                                         nullptr) const;

    BootstrapPredictionSequence PredictValueSequence(
        const BootstrapArtifact& artifact,
        int64_t start_bucket_id,
        uint32_t point_count,
        const BootstrapPredictionOptions& options,
        const BaselineTaskSpec* task_spec = nullptr,
        const CompiledEventCalendar* compiled_event_calendar = nullptr) const;

    BootstrapPrediction PredictRatio(const BootstrapArtifact& artifact,
                                     int64_t bucket_id,
                                     const BootstrapPredictionOptions& options,
                                     const BaselineTaskSpec* task_spec = nullptr,
                                     const CompiledEventCalendar* compiled_event_calendar =
                                         nullptr) const;

    BootstrapPredictionSequence PredictRatioSequence(
        const BootstrapArtifact& artifact,
        int64_t start_bucket_id,
        uint32_t point_count,
        const BootstrapPredictionOptions& options,
        const BaselineTaskSpec* task_spec = nullptr,
        const CompiledEventCalendar* compiled_event_calendar = nullptr) const;

    BaselineStatus ExportSeed(const BootstrapArtifact& artifact,
                              BootstrapSeed* out_seed) const;

    BaselineSerializationResult ExportArtifact(
        const BootstrapArtifact& artifact,
        BaselineSerializationFormat format) const;

    BaselineStatus LoadArtifact(std::string_view content,
                                BaselineSerializationFormat format,
                                BootstrapArtifact* out_artifact) const;

    BaselineStatus ValidateArtifactCompatibility(
        const BootstrapArtifact& artifact,
        const BaselineTaskSpec& spec,
        BootstrapArtifactKind expected_kind) const;

    BaselineStatus ValidateArtifactCompatibility(
        const BootstrapArtifact& artifact,
        const RelationTaskCreateSpec& spec) const;

    BaselineSerializationResult ExportSeed(
        const BootstrapSeed& seed,
        BaselineSerializationFormat format) const;
};

}  // namespace baseline
}  // namespace flowsql

#endif  // _FLOWSQL_PLUGINS_BASELINE_BOOTSTRAP_BOOTSTRAP_ENGINE_H_
