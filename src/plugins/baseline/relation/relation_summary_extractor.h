/*
 * Copyright (C) 2026 LIHUO
 *
 * Licensed under the MIT License. See LICENSE file in the project root
 * for full license information.
 *
 */

#ifndef _FLOWSQL_PLUGINS_BASELINE_RELATION_RELATION_SUMMARY_EXTRACTOR_H_
#define _FLOWSQL_PLUGINS_BASELINE_RELATION_RELATION_SUMMARY_EXTRACTOR_H_

#include <vector>

#include <framework/interfaces/ibaseline_types.h>

#include "plugins/baseline/relation/relation_basis.h"

namespace flowsql {
namespace baseline {

struct RelationMetricSummary {
    bool valid = false;
    double total = 0.0;
    double entropy_shannon = 0.0;
    double top1_share = 0.0;
    double headk_share = 0.0;
    double out_of_support_share = 0.0;
    bool has_distinct_group_count = false;
    double distinct_group_count = 0.0;
    std::vector<double> stable_g_shares;
    double stable_headk_coverage = 0.0;
    bool has_stable_headk_mix_drift = false;
    double stable_headk_mix_drift = 0.0;
};

class RelationSummaryExtractor {
 public:
    static int ExtractMetricSummary(const RelationObservationBlock& block,
                                    uint32_t metric_index,
                                    const RelationServiceBasis& basis,
                                    RelationMetricSummary* out_summary);
};

}  // namespace baseline
}  // namespace flowsql

#endif  // _FLOWSQL_PLUGINS_BASELINE_RELATION_RELATION_SUMMARY_EXTRACTOR_H_
