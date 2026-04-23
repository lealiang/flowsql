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
#include <plugins/baseline/detector/ratio_detector_core.h>

using namespace flowsql;
using namespace flowsql::baseline;

namespace {

static BaselineStringRef Ref(const char* s) {
    return BaselineStringRef{s, static_cast<uint32_t>(std::char_traits<char>::length(s))};
}

static void TestRatioDetectorCoreSubmitAndSnapshot() {
    std::printf("[TEST] RatioDetectorCore submit and snapshot...\n");

    RatioDetectorCoreSpec spec;
    spec.owner_task_id = "ratio-task-1";
    spec.routed_feature_id = "svc_success_rate";
    spec.feature_type = "t2";
    spec.feature_profile = "rate_core";

    RatioDetectorCore core(spec);

    DetectorSubmitOutput submit;
    const RatioObservation obs{Ref("svc-a"), 10, 90.0, 100.0};
    assert(core.Submit(obs, &submit) == error::OK);
    assert(submit.detector_result.status == error::OK);
    assert(submit.rebuild_intent.required == false);
    assert(submit.rebuild_intent.routed_feature_id == "svc_success_rate");

    RatioSeriesSnapshot snapshot;
    assert(core.BuildSeriesSnapshot(Ref("svc-a"), &snapshot) == error::OK);
    assert(snapshot.series_state.observation_count == 1);
    assert(snapshot.series_state.last_bucket_id == 10);
    assert(snapshot.runtime_state.last_numerator == 90.0);
    assert(snapshot.runtime_state.last_denominator == 100.0);
    assert(snapshot.runtime_state.last_observed_ratio == 0.9);
    assert(snapshot.runtime_state.last_rho > 1.0);
    assert(snapshot.runtime_state.last_gate_score == true);
    assert(snapshot.runtime_state.last_gate_shift == true);
    assert(core.Size() == 1);

    std::printf("[PASS] RatioDetectorCore submit and snapshot\n");
}

static void TestRatioDetectorCoreMarkRebuildFailure() {
    std::printf("[TEST] RatioDetectorCore rebuild failure snapshot...\n");

    RatioDetectorCoreSpec spec;
    spec.owner_task_id = "ratio-task-2";
    spec.routed_feature_id = "svc_success_rate";
    spec.feature_type = "t2";
    spec.feature_profile = "rate_core";

    RatioDetectorCore core(spec);
    DetectorSubmitOutput submit;
    assert(core.Submit(RatioObservation{Ref("svc-b"), 12, 50.0, 80.0}, &submit) == error::OK);

    DetectorRebuildFailure failure;
    failure.key = "svc-b";
    failure.request_bucket_start = 7;
    failure.request_bucket_end = 12;
    failure.candidate_state = "fetch_failed";
    core.MarkRebuildFailure(failure);

    RatioSeriesSnapshot snapshot;
    assert(core.BuildSeriesSnapshot(Ref("svc-b"), &snapshot) == error::OK);
    assert(snapshot.runtime_state.formal_state.candidate_state == "fetch_failed");
    assert(snapshot.runtime_state.formal_state.last_replay_window.request_bucket_start == 7);
    assert(snapshot.runtime_state.formal_state.last_replay_window.request_bucket_end == 12);
    assert(snapshot.runtime_state.shift_rebuild_pending == false);

    std::printf("[PASS] RatioDetectorCore rebuild failure snapshot\n");
}

}  // namespace

int main() {
    TestRatioDetectorCoreSubmitAndSnapshot();
    TestRatioDetectorCoreMarkRebuildFailure();
    std::printf("[DONE] test_baseline_ratio_task\n");
    return 0;
}
