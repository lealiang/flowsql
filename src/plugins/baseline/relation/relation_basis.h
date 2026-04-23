/*
 * Copyright (C) 2026 LIHUO
 *
 * Licensed under the MIT License. See LICENSE file in the project root
 * for full license information.
 *
 */

#ifndef _FLOWSQL_PLUGINS_BASELINE_RELATION_RELATION_BASIS_H_
#define _FLOWSQL_PLUGINS_BASELINE_RELATION_RELATION_BASIS_H_

#include <cstdint>
#include <string>
#include <vector>

#include "plugins/baseline/model/task_spec.h"

namespace flowsql {
namespace baseline {

enum class RelationLineageCompatibility : int32_t {
    kIdentical = 0,
    kCompatible = 1,
    kNewLineage = 2,
};

const char* RelationLineageCompatibilityName(RelationLineageCompatibility value);

struct RelationGroupHistoryStat {
    uint32_t group_idx = 0;
    double hist_mass = 0.0;
    uint64_t active_bucket_count = 0;
};

struct RelationBasisBuildInput {
    uint64_t basis_version = 1;
    std::string feature_base;
    std::string metric_name;
    std::string group_space_id;
    std::string group_space_version;
    RelationSupportPolicySpec support_policy;
    RelationSummaryPolicySpec summary_policy;
    uint64_t valid_bucket_count = 0;
    std::vector<RelationGroupHistoryStat> group_stats;
};

struct RelationServiceBasis {
    uint64_t basis_version = 0;
    std::string feature_base;
    std::string metric_name;
    std::string group_space_id;
    std::string group_space_version;
    int32_t k_head = 0;
    std::vector<uint32_t> support_explicit;
    std::vector<uint32_t> stable_head;
    std::vector<double> head_proto_q;
};

struct RelationEvalBasis {
    bool has_incumbent = false;
    RelationLineageCompatibility compatibility =
        RelationLineageCompatibility::kCompatible;
    RelationServiceBasis basis;
};

class RelationBasisBuilder {
 public:
    static int BuildServiceBasis(const RelationBasisBuildInput& input,
                                 RelationServiceBasis* out_basis);

    static RelationLineageCompatibility DetermineCompatibility(
        const RelationServiceBasis* incumbent_basis,
        const RelationTaskSpec& task_spec);

    static int BuildEvalBasis(const RelationServiceBasis* incumbent_basis,
                              const RelationTaskSpec& task_spec,
                              RelationEvalBasis* out_eval_basis);
};

}  // namespace baseline
}  // namespace flowsql

#endif  // _FLOWSQL_PLUGINS_BASELINE_RELATION_RELATION_BASIS_H_
