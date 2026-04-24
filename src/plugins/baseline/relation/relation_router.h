/*
 * Copyright (C) 2026 LIHUO
 *
 * Licensed under the MIT License. See LICENSE file in the project root
 * for full license information.
 *
 */

#ifndef _FLOWSQL_PLUGINS_BASELINE_RELATION_RELATION_ROUTER_H_
#define _FLOWSQL_PLUGINS_BASELINE_RELATION_RELATION_ROUTER_H_

#include <optional>
#include <string>
#include <vector>

#include <framework/interfaces/ibaseline_service.h>
#include <framework/interfaces/ibaseline_types.h>

#include "plugins/baseline/model/task_spec.h"
#include "plugins/baseline/relation/relation_summary_extractor.h"

namespace flowsql {
namespace baseline {

enum class RelationRoutedDetectorKind : int32_t {
    kValue = 0,
    kRatio = 1,
};

enum class RelationSummaryKind : int32_t {
    kEntropyShannon = 0,
    kTop1Share = 1,
    kHeadKShare = 2,
    kOutOfSupportShare = 3,
    kDistinctGroupCount = 4,
    kStableGShare = 5,
    kStableHeadCoverage = 6,
    kStableHeadMixDrift = 7,
};

struct RelationRoutedFeatureSpec {
    int32_t local_slot = 0;
    std::string metric_name;
    std::string feature;
    std::string routed_feature_id;
    RelationRoutedDetectorKind detector_kind = RelationRoutedDetectorKind::kValue;
    RelationSummaryKind summary_kind = RelationSummaryKind::kEntropyShannon;
    std::string feature_type;
    std::string feature_profile;
    std::optional<std::string> transform_kind;
    int64_t delta = 0;
    std::string tz;
    std::optional<BaselineSourceConfig> baseline_source_config;
    std::optional<EventCalendarSpec> event_calendar_spec;
    int stable_index = -1;
};

class RelationRouter {
 public:
    static void BuildRoutedFeatureSpecs(const RelationTaskSpec& spec,
                                        const RelationServiceBasis& basis,
                                        const RelationTaskClockSpec& clock_spec,
                                        const BaselineStringRef& key,
                                        const EventCalendarSpec* event_calendar_spec,
                                        IBaselineSourceResolver* source_resolver,
                                        std::vector<RelationRoutedFeatureSpec>* out_specs);

    static bool BuildValueObservation(const RelationRoutedFeatureSpec& feature_spec,
                                      const BaselineStringRef& key,
                                      int64_t bucket_id,
                                      const RelationMetricSummary& summary,
                                      ValueObservation* out_observation);

    static bool BuildRatioObservation(const RelationRoutedFeatureSpec& feature_spec,
                                      const BaselineStringRef& key,
                                      int64_t bucket_id,
                                      const RelationMetricSummary& summary,
                                      RatioObservation* out_observation);
};

}  // namespace baseline
}  // namespace flowsql

#endif  // _FLOWSQL_PLUGINS_BASELINE_RELATION_RELATION_ROUTER_H_
