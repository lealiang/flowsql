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

#include "plugins/baseline/model/series_state.h"

namespace flowsql {
namespace baseline {

constexpr double kScoreWarn = 3.0;
constexpr double kScoreCrit = 5.0;
constexpr double kConfidenceFormalBase = 0.8;
constexpr double kConfidenceSourceBase = 0.6;
constexpr double kConfidenceShadowBase = 0.5;

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
    if (raw_score <= kScoreWarn) return 0.0;
    return ClipUnit((raw_score - kScoreWarn) / (kScoreCrit - kScoreWarn));
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
            return kConfidenceFormalBase;
        case BaselineProvider::kSource:
            return kConfidenceSourceBase;
        case BaselineProvider::kShadow:
            return kConfidenceShadowBase;
    }
    return 0.0;
}

}  // namespace baseline
}  // namespace flowsql

#endif  // _FLOWSQL_PLUGINS_BASELINE_COMMON_RESULT_BUILDER_H_
