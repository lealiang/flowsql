/*
 * Copyright (C) 2026 LIHUO
 *
 * Licensed under the MIT License. See LICENSE file in the project root
 * for full license information.
 *
 */

#ifndef _FLOWSQL_PLUGINS_BASELINE_FUSION_RELATION_PATTERN_FUSION_H_
#define _FLOWSQL_PLUGINS_BASELINE_FUSION_RELATION_PATTERN_FUSION_H_

#include <cstdint>
#include <string>
#include <vector>

#include <framework/interfaces/ibaseline_types.h>

#include "plugins/baseline/fusion/fusion_types.h"
#include "plugins/baseline/relation/relation_router.h"

namespace flowsql {
namespace baseline {

enum class PatternCode : int32_t {
    kSupportEscape = 0,
    kHeadConcentration = 1,
    kLegacyHeadDilution = 2,
    kStableHeadMixShift = 3,
    kUnknown = 4,
};

constexpr int32_t kRelationPatternLocalSlotBase = 1000000;

const char* PatternCodeName(PatternCode code);
PatternCode PatternCodeFromName(const std::string& name);
double PatternWeight(PatternCode code);
int32_t PatternLocalSlot(PatternCode code);

struct FusionSingleContribution {
    int32_t local_slot = 0;
    std::string metric_name;
    RelationSummaryKind summary_kind = RelationSummaryKind::kEntropyShannon;
    DetectorResult detector_result;
};

struct FusionPatternContribution {
    PatternCode pattern = PatternCode::kUnknown;
    int32_t local_slot = 0;
    StoredDominantPatternProjection projection;
};

struct RelationPatternFusionInput {
    std::string key;
    int64_t bucket_id = 0;
    std::string feature_base;
    std::vector<FusionSingleContribution> singles;
};

struct RelationPatternFusionOutput {
    StoredFusionResult fusion_result;
    std::vector<FusionPatternContribution> pattern_contributions;
};

class RelationPatternFusion {
 public:
    static int Compute(const RelationPatternFusionInput& input,
                       RelationPatternFusionOutput* out);
};

}  // namespace baseline
}  // namespace flowsql

#endif  // _FLOWSQL_PLUGINS_BASELINE_FUSION_RELATION_PATTERN_FUSION_H_
