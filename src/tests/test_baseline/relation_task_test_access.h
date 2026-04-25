/*
 * Copyright (C) 2026 LIHUO
 *
 * Licensed under the MIT License. See LICENSE file in the project root
 * for full license information.
 *
 */

#ifndef _FLOWSQL_TESTS_TEST_BASELINE_RELATION_TASK_TEST_ACCESS_H_
#define _FLOWSQL_TESTS_TEST_BASELINE_RELATION_TASK_TEST_ACCESS_H_

#include <plugins/baseline/task/relation_task.h>

namespace flowsql {
namespace baseline {

struct RelationTaskTestAccess {
    static void SeedMetricBasis(BaselineRelationTask* task,
                                const std::string& key,
                                const std::string& metric_name,
                                const RelationServiceBasis& service_basis) {
        if (!task) return;

        std::lock_guard<std::mutex> lock(task->runtime_mutex_);
        auto& runtime_state = task->runtime_by_key_[key];
        auto& metric_runtime = runtime_state.metrics_by_name[metric_name];
        metric_runtime.service_basis = service_basis;
        metric_runtime.eval_basis = RelationEvalBasis{};
        metric_runtime.eval_basis.has_incumbent = true;
        metric_runtime.eval_basis.compatibility = RelationLineageCompatibility::kIdentical;
        metric_runtime.eval_basis.basis = service_basis;
        runtime_state.last_lineage_compatibility = RelationLineageCompatibility::kIdentical;
    }
};

}  // namespace baseline
}  // namespace flowsql

#endif  // _FLOWSQL_TESTS_TEST_BASELINE_RELATION_TASK_TEST_ACCESS_H_
