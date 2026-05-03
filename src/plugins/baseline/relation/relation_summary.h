/*
 * Copyright (C) 2026 LIHUO
 *
 * Licensed under the MIT License. See LICENSE file in the project root
 * for full license information.
 *
 */

#ifndef _FLOWSQL_PLUGINS_BASELINE_RELATION_RELATION_SUMMARY_H_
#define _FLOWSQL_PLUGINS_BASELINE_RELATION_RELATION_SUMMARY_H_

#include <framework/interfaces/ibaseline_types.h>

#include <cstdint>
#include <string>
#include <vector>

#include "plugins/baseline/model/task_spec.h"
#include "plugins/baseline/relation/relation_basis.h"

namespace flowsql {
namespace baseline {

struct RelationProjectedSummary {
    std::string metric_name;
    std::string summary_name;
    BaselineTaskKind task_kind = BaselineTaskKind::kValue;
    std::string feature_type;
    uint64_t basis_version = 0;
    bool basis_scoped = false;
    bool active_count_from_upstream = false;
    double value = 0.0;
    double numerator = 0.0;
    double denominator = 0.0;
};

struct RelationSummaryProjectionOptions {
    RelationSummaryPolicySpec summary_policy;
    std::vector<uint32_t> other_group_idxs;
    const RelationServiceBasis* basis = nullptr;
    bool include_basis_scoped = true;
};

bool ProjectRelationMetricSummaries(
    const RelationBootstrapBlock& block,
    std::size_t metric_index,
    const std::string& metric_name,
    const RelationSummaryProjectionOptions& options,
    std::vector<RelationProjectedSummary>* out_summaries);

bool ProjectRelationMetricSummaries(
    const RelationRollingObservation& obs,
    std::size_t metric_index,
    const std::string& metric_name,
    const RelationSummaryProjectionOptions& options,
    std::vector<RelationProjectedSummary>* out_summaries);

}  // namespace baseline
}  // namespace flowsql

#endif  // _FLOWSQL_PLUGINS_BASELINE_RELATION_RELATION_SUMMARY_H_
