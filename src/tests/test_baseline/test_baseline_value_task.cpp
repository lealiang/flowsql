/*
 * Copyright (C) 2026 LIHUO
 *
 * Licensed under the MIT License. See LICENSE file in the project root
 * for full license information.
 *
 */

#include <cassert>
#include <cstdio>
#include <string>

#include <common/error_code.h>
#include <framework/interfaces/ibaseline_types.h>
#include <plugins/baseline/detector/detector_common.h>
#include <plugins/baseline/detector/value_detector_core.h>

using namespace flowsql;
using namespace flowsql::baseline;

namespace {

static BaselineStringRef Ref(const char* s) {
    return BaselineStringRef{s, static_cast<uint32_t>(std::char_traits<char>::length(s))};
}

static void TestValueDetectorCoreSubmitAndSnapshot() {
    std::printf("[TEST] ValueDetectorCore submit and snapshot...\n");

    ValueDetectorCoreSpec spec;
    spec.owner_task_id = "value-task-1";
    spec.routed_feature_id = "svc_latency";
    spec.feature_type = "t1a";
    spec.feature_profile = "default";

    ValueDetectorCore core(spec);

    DetectorSubmitOutput submit;
    const ValueObservation obs{Ref("svc-a"), 10, 9.0, 0};
    assert(core.Submit(obs, &submit) == error::OK);
    assert(submit.detector_result.status == error::OK);
    assert(submit.rebuild_intent.required == false);
    assert(submit.rebuild_intent.routed_feature_id == "svc_latency");

    ValueSeriesSnapshot snapshot;
    assert(core.BuildSeriesSnapshot(Ref("svc-a"), &snapshot) == error::OK);
    assert(snapshot.series_state.observation_count == 1);
    assert(snapshot.series_state.last_bucket_id == 10);
    assert(snapshot.runtime_state.last_value == 9.0);
    assert(snapshot.runtime_state.last_x > 0.0);
    assert(snapshot.runtime_state.last_rho == 1.0);
    assert(snapshot.runtime_state.last_gate_score == true);
    assert(snapshot.runtime_state.last_gate_shift == true);
    assert(core.Size() == 1);

    std::printf("[PASS] ValueDetectorCore submit and snapshot\n");
}

static void TestValueDetectorCoreMarkRebuildFailure() {
    std::printf("[TEST] ValueDetectorCore rebuild failure snapshot...\n");

    ValueDetectorCoreSpec spec;
    spec.owner_task_id = "value-task-2";
    spec.routed_feature_id = "svc_latency";
    spec.feature_type = "t1a";
    spec.feature_profile = "default";

    ValueDetectorCore core(spec);
    DetectorSubmitOutput submit;
    assert(core.Submit(ValueObservation{Ref("svc-b"), 12, 4.0, 0}, &submit) == error::OK);

    DetectorRebuildFailure failure;
    failure.key = "svc-b";
    failure.request_bucket_start = 7;
    failure.request_bucket_end = 12;
    failure.candidate_state = "fetch_failed";
    core.MarkRebuildFailure(failure);

    ValueSeriesSnapshot snapshot;
    assert(core.BuildSeriesSnapshot(Ref("svc-b"), &snapshot) == error::OK);
    assert(snapshot.runtime_state.formal_state.candidate_state == "fetch_failed");
    assert(snapshot.runtime_state.formal_state.last_replay_window.request_bucket_start == 7);
    assert(snapshot.runtime_state.formal_state.last_replay_window.request_bucket_end == 12);
    assert(snapshot.runtime_state.shift_rebuild_pending == false);

    std::printf("[PASS] ValueDetectorCore rebuild failure snapshot\n");
}

}  // namespace

int main() {
    TestValueDetectorCoreSubmitAndSnapshot();
    TestValueDetectorCoreMarkRebuildFailure();
    std::printf("[DONE] test_baseline_value_task\n");
    return 0;
}
