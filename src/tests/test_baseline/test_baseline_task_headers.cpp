/*
 * Copyright (C) 2026 LIHUO
 *
 * Licensed under the MIT License. See LICENSE file in the project root
 * for full license information.
 *
 */

#include <cstdio>
#include <memory>
#include <type_traits>

#include <plugins/baseline/task/baseline_task_base.h>
#include <plugins/baseline/task/relation_task.h>
#include <plugins/baseline/task/ratio_task.h>
#include <plugins/baseline/task/value_task.h>

using namespace flowsql::baseline;

namespace {

void TestTaskHeaderContracts() {
    std::printf("[TEST] Task header contracts...\n");

    static_assert(std::is_base_of_v<BaselineTaskBase, BaselineValueTask>);
    static_assert(std::is_base_of_v<BaselineTaskBase, BaselineRatioTask>);
    static_assert(std::is_base_of_v<BaselineTaskBase, BaselineRelationTask>);

    std::shared_ptr<ValueDetectorCore> value_core;
    std::shared_ptr<RatioDetectorCore> ratio_core;
    std::shared_ptr<RebuildTaskRuntime> rebuild_runtime;
    (void)value_core;
    (void)ratio_core;
    (void)rebuild_runtime;

    std::printf("[PASS] Task header contracts\n");
}

}  // namespace

int main() {
    TestTaskHeaderContracts();
    return 0;
}
