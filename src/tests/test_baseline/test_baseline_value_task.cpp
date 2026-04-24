/*
 * Copyright (C) 2026 LIHUO
 *
 * Licensed under the MIT License. See LICENSE file in the project root
 * for full license information.
 *
 */

#include <cassert>
#include <cmath>
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

static bool NearlyEqual(double lhs, double rhs, double eps = 1e-9) {
    return std::fabs(lhs - rhs) <= eps;
}

static std::shared_ptr<ValueFormalModel> BuildFormalModel(double beta0,
                                                          double sigma_ref,
                                                          uint64_t version,
                                                          ModelReadiness readiness =
                                                              ModelReadiness::kMonthposReady) {
    auto model = std::make_shared<ValueFormalModel>();
    model->metadata.kind = FormalModelKind::kValueBaseline;
    model->metadata.model_version = version;
    model->readiness = readiness;
    model->transform_name = "log1p";
    model->delta = 60;
    model->tz = "UTC";
    model->train_start = 0;
    model->train_end = 0;
    model->confidence_base_at_train = 1.0;
    model->sigma_ref = sigma_ref;
    model->core_block.beta0 = beta0;
    return model;
}

static void ApplyFormalModel(ValueDetectorCore* core,
                             const char* key,
                             std::shared_ptr<ValueFormalModel> model) {
    assert(core != nullptr);
    ValueApplyFormalModelResult apply_result;
    apply_result.candidate_trained = true;
    apply_result.candidate_generation = model ? model->metadata.model_version : 0;
    apply_result.switch_state = "formal_apply";
    apply_result.full_model = std::move(model);
    core->ApplyFormalModel(key, apply_result);
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

static void TestValueDetectorCoreValueEvidenceAndIdentity() {
    std::printf("[TEST] ValueDetectorCore value evidence and identity...\n");

    ValueDetectorCoreSpec spec;
    spec.owner_task_id = "value-task-3";
    spec.routed_feature_id = "bytes_total";
    spec.feature_type = "t1a";
    spec.feature_profile = "traffic";

    ValueDetectorCore core(spec);
    ApplyFormalModel(&core, "svc-c", BuildFormalModel(std::log1p(4.0), 2.0, 1));

    DetectorSubmitOutput submit;
    const ValueObservation obs{Ref("svc-c"), 20, 9.0, 0};
    assert(core.Submit(obs, &submit) == error::OK);

    assert(submit.detector_result.status == error::OK);
    assert(submit.detector_result.key.data != nullptr);
    assert(std::string(submit.detector_result.key.data,
                       submit.detector_result.key.size) == "svc-c");
    assert(submit.detector_result.feature.data != nullptr);
    assert(std::string(submit.detector_result.feature.data,
                       submit.detector_result.feature.size) == "bytes_total");
    assert(submit.detector_result.feature_type.data != nullptr);
    assert(std::string(submit.detector_result.feature_type.data,
                       submit.detector_result.feature_type.size) == "t1a");
    assert(submit.detector_result.ts == 20);
    assert(submit.detector_result.provider == BaselineProvider::kFormal);
    assert(NearlyEqual(submit.detector_result.confidence, 1.0));
    assert(submit.detector_result.evidence.kind == BaselineEvidenceKind::kValue);
    assert(NearlyEqual(submit.detector_result.evidence.value.y_t, 9.0));
    assert(NearlyEqual(submit.detector_result.evidence.value.x_t, std::log1p(9.0)));
    assert(NearlyEqual(submit.detector_result.evidence.value.baseline_mu_t, std::log1p(4.0)));
    assert(NearlyEqual(submit.detector_result.evidence.value.resid_r_t,
                       std::log1p(9.0) - std::log1p(4.0)));
    assert(NearlyEqual(submit.detector_result.evidence.value.z_t,
                       (std::log1p(9.0) - std::log1p(4.0)) / 2.0));
    assert(submit.detector_result.evidence.value.field_flags == 0);
    assert(submit.detector_result.evidence.value.baseline_source_kind ==
           BaselineSourceKind::kSelf);
    assert(submit.detector_result.evidence.value.model_state == BaselineModelState::kFormal);
    assert(!submit.detector_result.evidence.value.shadow_active);

    std::printf("[PASS] ValueDetectorCore value evidence and identity\n");
}

static void TestValueDetectorCoreT1bSampleCountAndConfidence() {
    std::printf("[TEST] ValueDetectorCore t1b sample count and confidence...\n");

    ValueDetectorCoreSpec spec;
    spec.owner_task_id = "value-task-4";
    spec.routed_feature_id = "avg_rtt";
    spec.feature_type = "t1b";
    spec.feature_profile = "cont_core";

    ValueDetectorCore core(spec);
    ApplyFormalModel(&core, "svc-d", BuildFormalModel(std::log1p(10.0), 2.0, 1));

    DetectorSubmitOutput submit;
    const ValueObservation obs{Ref("svc-d"), 30, 10.0, 50};
    assert(core.Submit(obs, &submit) == error::OK);

    const double rho = std::sqrt(2.0);
    assert(submit.detector_result.provider == BaselineProvider::kFormal);
    assert(NearlyEqual(submit.detector_result.confidence, 1.0 / rho));
    assert(submit.detector_result.evidence.kind == BaselineEvidenceKind::kValue);
    assert((submit.detector_result.evidence.value.field_flags &
            kBaselineEvidenceHasSampleCount) != 0);
    assert((submit.detector_result.evidence.value.field_flags &
            kBaselineEvidenceHasSigmaEff) != 0);
    assert(submit.detector_result.evidence.value.sample_count == 50);
    assert(NearlyEqual(submit.detector_result.evidence.value.sigma_eff_t, 2.0 * rho));
    assert(submit.detector_result.evidence.value.model_state == BaselineModelState::kFormal);

    std::printf("[PASS] ValueDetectorCore t1b sample count and confidence\n");
}

static void TestValueDetectorCoreConfiguredSourceConfidence() {
    std::printf("[TEST] ValueDetectorCore configured source confidence...\n");

    ValueDetectorCoreSpec spec;
    spec.owner_task_id = "value-task-5";
    spec.routed_feature_id = "bytes_total";
    spec.feature_type = "t1a";
    spec.feature_profile = "traffic";
    SeriesBaselineSourceConfig source_config;
    source_config.key = "svc-target";
    source_config.config.sources.push_back(BaselineSourceRef{"svc-source"});
    spec.baseline_source_configs.push_back(source_config);

    ValueDetectorCore core(spec);
    ApplyFormalModel(&core, "svc-source", BuildFormalModel(std::log1p(7.0), 1.0, 1));

    DetectorSubmitOutput submit;
    const ValueObservation obs{Ref("svc-target"), 40, 7.0, 0};
    assert(core.Submit(obs, &submit) == error::OK);

    assert(submit.detector_result.provider == BaselineProvider::kSource);
    assert(NearlyEqual(submit.detector_result.confidence, 0.8));
    assert(submit.detector_result.evidence.kind == BaselineEvidenceKind::kValue);
    assert((submit.detector_result.evidence.value.field_flags &
            kBaselineEvidenceHasSourceKey) != 0);
    assert(submit.detector_result.evidence.value.baseline_source_kind ==
           BaselineSourceKind::kConfiguredSource);
    assert(submit.detector_result.evidence.value.baseline_source_key.data != nullptr);
    assert(std::string(submit.detector_result.evidence.value.baseline_source_key.data,
                       submit.detector_result.evidence.value.baseline_source_key.size) ==
           "svc-source");
    assert(submit.detector_result.evidence.value.model_state ==
           BaselineModelState::kConfiguredSource);

    std::printf("[PASS] ValueDetectorCore configured source confidence\n");
}

static void TestValueDetectorCoreShadowConfidenceAndReason() {
    std::printf("[TEST] ValueDetectorCore shadow confidence and reason...\n");

    ValueDetectorCoreSpec spec;
    spec.owner_task_id = "value-task-6";
    spec.routed_feature_id = "bytes_total";
    spec.feature_type = "t1a";
    spec.feature_profile = "traffic";

    ValueDetectorCore core(spec);
    ApplyFormalModel(&core, "svc-shadow", BuildFormalModel(std::log1p(1.0), 1.0, 1));

    DetectorSubmitOutput first;
    DetectorSubmitOutput second;
    DetectorSubmitOutput third;
    assert(core.Submit(ValueObservation{Ref("svc-shadow"), 201, 64.0, 0}, &first) == error::OK);
    assert(core.Submit(ValueObservation{Ref("svc-shadow"), 202, 64.0, 0}, &second) == error::OK);
    assert(core.Submit(ValueObservation{Ref("svc-shadow"), 203, 64.0, 0}, &third) == error::OK);

    assert((first.detector_result.flags & kBaselineFlagShadowActive) == 0);
    assert((second.detector_result.flags & kBaselineFlagShadowActive) == 0);
    assert((third.detector_result.flags & kBaselineFlagShadowActive) != 0);
    assert(third.detector_result.provider == BaselineProvider::kShadow);
    assert(NearlyEqual(third.detector_result.confidence, 0.8));
    assert(third.detector_result.reason == BaselineReasonCode::kBaselineShiftUp);
    assert(third.detector_result.evidence.kind == BaselineEvidenceKind::kValue);
    assert(third.detector_result.evidence.value.shadow_active);
    assert(third.detector_result.evidence.value.model_state == BaselineModelState::kShadow);

    std::printf("[PASS] ValueDetectorCore shadow confidence and reason\n");
}

}  // namespace

int main() {
    TestValueDetectorCoreSubmitAndSnapshot();
    TestValueDetectorCoreMarkRebuildFailure();
    TestValueDetectorCoreValueEvidenceAndIdentity();
    TestValueDetectorCoreT1bSampleCountAndConfidence();
    TestValueDetectorCoreConfiguredSourceConfidence();
    TestValueDetectorCoreShadowConfidenceAndReason();
    std::printf("[DONE] test_baseline_value_task\n");
    return 0;
}
