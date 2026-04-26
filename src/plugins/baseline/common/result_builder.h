/*
 * Copyright (C) 2026 LIHUO
 *
 * Licensed under the MIT License. See LICENSE file in the project root
 * for full license information.
 *
 */

#ifndef _FLOWSQL_PLUGINS_BASELINE_COMMON_RESULT_BUILDER_H_
#define _FLOWSQL_PLUGINS_BASELINE_COMMON_RESULT_BUILDER_H_

#include <framework/interfaces/ibaseline_types.h>

#include <algorithm>

#include "plugins/baseline/config/runtime_config.h"
#include "plugins/baseline/model/series_state.h"

namespace flowsql {
namespace baseline {

inline void FillBaseResult(const SeriesUpdateResult& update,
                           DetectorResult* out) {
    if (!out) return;
    out->status = update.status;
    out->persistence = update.persistence;
    out->flags = update.flags;
}

inline double ClipUnit(double value) {
    return std::max(0.0, std::min(1.0, value));
}

inline double ComputePointScore(double raw_score) {
    const double score_warn = ScoreWarn();
    const double score_crit = ScoreCrit();
    if (raw_score <= score_warn) return 0.0;
    return ClipUnit((raw_score - score_warn) / (score_crit - score_warn));
}

inline BaselineDirection DirectionFromResidual(double residual) {
    if (residual > 0.0) return BaselineDirection::kUp;
    if (residual < 0.0) return BaselineDirection::kDown;
    return BaselineDirection::kUnknown;
}

inline BaselineReasonCode ReasonFromResidual(double residual,
                                             double normalized_score) {
    if (normalized_score <= 0.0) return BaselineReasonCode::kUnknown;
    return residual >= 0.0 ? BaselineReasonCode::kSpike
                           : BaselineReasonCode::kDrop;
}

inline BaselineSeverity SeverityFromNormalizedScore(double normalized_score) {
    if (normalized_score >= 0.85) return BaselineSeverity::kHigh;
    if (normalized_score >= 0.50) return BaselineSeverity::kMedium;
    if (normalized_score > 0.0) return BaselineSeverity::kLow;
    return BaselineSeverity::kInfo;
}

inline double ConfidenceBaseForProvider(BaselineProvider provider) {
    switch (provider) {
        case BaselineProvider::kFormal:
            return ConfidenceFormalBase();
        case BaselineProvider::kSource:
            return ConfidenceSourceBase();
        case BaselineProvider::kShadow:
            return ConfidenceShadowBase();
    }
    return 0.0;
}

}  // namespace baseline
}  // namespace flowsql

#endif  // _FLOWSQL_PLUGINS_BASELINE_COMMON_RESULT_BUILDER_H_
