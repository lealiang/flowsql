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
#define private public
#include <plugins/baseline/detector/ratio_detector_core.h>
#undef private

using namespace flowsql;
using namespace flowsql::baseline;

namespace {

static BaselineStringRef Ref(const char* s) {
    return BaselineStringRef{s, static_cast<uint32_t>(std::char_traits<char>::length(s))};
}

static bool NearlyEqual(double lhs, double rhs, double eps = 1e-9) {
    return std::fabs(lhs - rhs) <= eps;
}

static double Logit(double p) {
    return std::log(p / (1.0 - p));
}

static std::shared_ptr<RatioFormalModel> BuildFormalModel(
    double p_hat,
    uint64_t version,
    ModelReadiness readiness = ModelReadiness::kMonthposReady) {
    auto model = std::make_shared<RatioFormalModel>();
    model->metadata.kind = FormalModelKind::kRatioBaseline;
    model->metadata.model_version = version;
    model->readiness = readiness;
    model->transform_name = "logit";
    model->delta = 60;
    model->tz = "UTC";
    model->train_start = 0;
    model->train_end = 0;
    model->feature_profile = "rate_core";
    model->m0 = p_hat;
    model->alpha0 = 2.0 * p_hat;
    model->beta0 = 2.0 * (1.0 - p_hat);
    model->confidence_base_at_train = 1.0;
    model->core_block.beta0 = Logit(p_hat);
    return model;
}

static void ApplyFormalModel(RatioDetectorCore* core,
                             const char* key,
                             std::shared_ptr<RatioFormalModel> model) {
    assert(core != nullptr);
    RatioApplyFormalModelResult apply_result;
    apply_result.candidate_trained = true;
    apply_result.candidate_generation = model ? model->metadata.model_version : 0;
    apply_result.candidate_state = RebuildCandidateState::kAccepted;
    apply_result.switch_state = RebuildSwitchState::kFormalApplied;
    apply_result.full_model = std::move(model);
    core->ApplyFormalModel(key, apply_result);
}

static std::shared_ptr<const CompiledEventCalendar> BuildCompiledCalendar(const char* feature,
                                                                          const char* key) {
    EventCalendarSpec calendar;
    calendar.calendar_id = "ops-calendar";
    calendar.calendar_version = "v1";
    calendar.entries.push_back(EventCalendarEntry{
        "deploy",
        "key_feature",
        "absolute_utc",
        20 * 60,
        21 * 60,
        true,
        feature,
        key,
        ""});

    BaselineTaskSpec task_spec;
    task_spec.key = key;
    task_spec.feature = feature;
    task_spec.delta = 60;
    task_spec.tz = "UTC";

    CompiledEventCalendar compiled;
    std::string err;
    assert(CompileEventCalendar(calendar, task_spec, &compiled, &err) == error::OK);
    return std::make_shared<CompiledEventCalendar>(std::move(compiled));
}

static void TestRatioDetectorCoreSubmitAndSnapshot() {
    std::printf("[TEST] RatioDetectorCore submit and snapshot...\n");

    RatioDetectorCoreSpec spec;
    spec.owner_task_id = "ratio-task-1";
    spec.routed_feature_id = "svc_success_rate";
    spec.feature_type = "ratio";
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
    spec.feature_type = "ratio";
    spec.feature_profile = "rate_core";

    RatioDetectorCore core(spec);
    DetectorSubmitOutput submit;
    assert(core.Submit(RatioObservation{Ref("svc-b"), 12, 50.0, 80.0}, &submit) == error::OK);

    DetectorRebuildFailure failure;
    failure.key = "svc-b";
    failure.request_bucket_start = 7;
    failure.request_bucket_end = 12;
    failure.candidate_state = RebuildCandidateState::kFailed;
    failure.switch_state = RebuildSwitchState::kRebuildBlocked;
    failure.failure_reason = RebuildFailureReason::kUnavailable;
    core.MarkRebuildFailure(failure);

    RatioSeriesSnapshot snapshot;
    assert(core.BuildSeriesSnapshot(Ref("svc-b"), &snapshot) == error::OK);
    assert(snapshot.runtime_state.formal_state.candidate_state ==
           RebuildCandidateState::kFailed);
    assert(snapshot.runtime_state.formal_state.switch_state ==
           RebuildSwitchState::kIdle);
    assert(snapshot.runtime_state.formal_state.failure_reason ==
           RebuildFailureReason::kUnavailable);
    assert(snapshot.runtime_state.formal_state.last_replay_window.request_bucket_start == 7);
    assert(snapshot.runtime_state.formal_state.last_replay_window.request_bucket_end == 12);
    assert(snapshot.runtime_state.shift_rebuild_pending == false);

    std::printf("[PASS] RatioDetectorCore rebuild failure snapshot\n");
}

static void TestRatioDetectorCoreRatioEvidenceAndIdentity() {
    std::printf("[TEST] RatioDetectorCore ratio evidence and identity...\n");

    RatioDetectorCoreSpec spec;
    spec.owner_task_id = "ratio-task-3";
    spec.routed_feature_id = "svc_success_rate";
    spec.feature_type = "ratio";
    spec.feature_profile = "rate_core";

    RatioDetectorCore core(spec);
    ApplyFormalModel(&core, "svc-c", BuildFormalModel(0.8, 1));

    DetectorSubmitOutput submit;
    const RatioObservation obs{Ref("svc-c"), 20, 90.0, 100.0};
    assert(core.Submit(obs, &submit) == error::OK);

    const double p_smooth = (90.0 + 1.6) / (100.0 + 1.6 + 0.4);
    const double x_t = Logit(p_smooth);
    const double var_eff = 1.5 * 100.0 * 0.8 * 0.2;
    const double r_t = (90.0 - 100.0 * 0.8) / std::sqrt(var_eff);
    const double rho_t = std::sqrt(1.0 + 50.0 / 100.0);

    assert(submit.detector_result.status == error::OK);
    assert(submit.detector_result.key.data != nullptr);
    assert(std::string(submit.detector_result.key.data,
                       submit.detector_result.key.size) == "svc-c");
    assert(submit.detector_result.feature.data != nullptr);
    assert(std::string(submit.detector_result.feature.data,
                       submit.detector_result.feature.size) == "svc_success_rate");
    assert(submit.detector_result.feature_type.data != nullptr);
    assert(std::string(submit.detector_result.feature_type.data,
                       submit.detector_result.feature_type.size) == "ratio");
    assert(submit.detector_result.ts == 20);
    assert(submit.detector_result.provider == BaselineProvider::kFormal);
    assert(NearlyEqual(submit.detector_result.confidence, 1.0 / rho_t));
    assert(submit.detector_result.evidence.kind == BaselineEvidenceKind::kRatio);
    assert(NearlyEqual(submit.detector_result.evidence.ratio.numerator, 90.0));
    assert(NearlyEqual(submit.detector_result.evidence.ratio.denominator, 100.0));
    assert(NearlyEqual(submit.detector_result.evidence.ratio.p_smooth, p_smooth));
    assert(NearlyEqual(submit.detector_result.evidence.ratio.x_t, x_t));
    assert(NearlyEqual(submit.detector_result.evidence.ratio.p_hat_t, 0.8));
    assert(NearlyEqual(submit.detector_result.evidence.ratio.var_eff_t, var_eff));
    assert(NearlyEqual(submit.detector_result.evidence.ratio.r_t, r_t));
    assert(NearlyEqual(submit.detector_result.evidence.ratio.rho_t, rho_t));
    assert(submit.detector_result.evidence.ratio.baseline_source_kind ==
           BaselineSourceKind::kSelf);
    assert(submit.detector_result.evidence.ratio.model_state ==
           BaselineModelState::kFormal);
    assert(!submit.detector_result.evidence.ratio.shadow_active);

    std::printf("[PASS] RatioDetectorCore ratio evidence and identity\n");
}

static void TestRatioDetectorCoreConfiguredSourceConfidence() {
    std::printf("[TEST] RatioDetectorCore configured source confidence...\n");

    RatioDetectorCoreSpec spec;
    spec.owner_task_id = "ratio-task-4";
    spec.routed_feature_id = "svc_success_rate";
    spec.feature_type = "ratio";
    spec.feature_profile = "rate_core";
    SeriesBaselineSourceConfig source_config;
    source_config.key = "svc-target";
    source_config.config.sources.push_back(BaselineSourceRef{"svc-source"});
    spec.baseline_source_configs.push_back(source_config);

    RatioDetectorCore core(spec);
    ApplyFormalModel(&core, "svc-source", BuildFormalModel(0.7, 1));

    DetectorSubmitOutput submit;
    const RatioObservation obs{Ref("svc-target"), 40, 70.0, 100.0};
    assert(core.Submit(obs, &submit) == error::OK);

    const double rho_t = std::sqrt(1.0 + 50.0 / 100.0);
    assert(submit.detector_result.provider == BaselineProvider::kSource);
    assert(NearlyEqual(submit.detector_result.confidence, 0.8 / rho_t));
    assert(submit.detector_result.evidence.kind == BaselineEvidenceKind::kRatio);
    assert((submit.detector_result.evidence.ratio.field_flags &
            kBaselineEvidenceHasSourceKey) != 0);
    assert(submit.detector_result.evidence.ratio.baseline_source_kind ==
           BaselineSourceKind::kConfiguredSource);
    assert(submit.detector_result.evidence.ratio.baseline_source_key.data != nullptr);
    assert(std::string(submit.detector_result.evidence.ratio.baseline_source_key.data,
                       submit.detector_result.evidence.ratio.baseline_source_key.size) ==
           "svc-source");
    assert(submit.detector_result.evidence.ratio.model_state ==
           BaselineModelState::kConfiguredSource);

    std::printf("[PASS] RatioDetectorCore configured source confidence\n");
}

static void TestRatioDetectorCoreUsesShardedRuntimeStorage() {
    std::printf("[TEST] RatioDetectorCore uses sharded runtime storage...\n");

    RatioDetectorCoreSpec spec;
    spec.owner_task_id = "ratio-task-shards";
    spec.routed_feature_id = "success_rate";
    spec.feature_type = "ratio";
    spec.feature_profile = "rate_core";

    RatioDetectorCore core(spec);
    assert(RatioDetectorCore::kShardCount > 1);

    std::string key_a = "svc-ratio-shard-a";
    std::string key_b = "svc-ratio-shard-b";
    size_t shard_a = core.RuntimeShardIndex(key_a);
    size_t shard_b = core.RuntimeShardIndex(key_b);
    for (int i = 0; shard_a == shard_b && i < 256; ++i) {
        key_b = "svc-ratio-shard-b-" + std::to_string(i);
        shard_b = core.RuntimeShardIndex(key_b);
    }
    assert(shard_a != shard_b);

    DetectorSubmitOutput submit;
    assert(core.Submit(RatioObservation{BaselineStringRef{key_a.c_str(),
                                                          static_cast<uint32_t>(key_a.size())},
                                       10,
                                       95.0,
                                       100.0},
                       &submit) == error::OK);
    assert(core.Submit(RatioObservation{BaselineStringRef{key_b.c_str(),
                                                          static_cast<uint32_t>(key_b.size())},
                                       10,
                                       97.0,
                                       100.0},
                       &submit) == error::OK);

    assert(core.runtime_shards_[shard_a].states.find(key_a) !=
           core.runtime_shards_[shard_a].states.end());
    assert(core.runtime_shards_[shard_b].states.find(key_b) !=
           core.runtime_shards_[shard_b].states.end());

    std::printf("[PASS] RatioDetectorCore uses sharded runtime storage\n");
}

static void TestRatioDetectorCoreKeyFeatureEventCalendar() {
    std::printf("[TEST] RatioDetectorCore key_feature event calendar...\n");

    RatioDetectorCoreSpec spec;
    spec.owner_task_id = "ratio-task-5";
    spec.routed_feature_id = "svc_success_rate";
    spec.feature_type = "ratio";
    spec.feature_profile = "rate_core";
    spec.delta = 60;
    spec.tz = "UTC";
    spec.compiled_event_calendar = BuildCompiledCalendar("svc_success_rate", "svc-match");

    RatioDetectorCore core(spec);

    auto model = BuildFormalModel(0.3, 1, ModelReadiness::kCoreNoMonthReady);
    model->metadata.calendar_id = "ops-calendar";
    model->metadata.calendar_version = "v1";
    model->event_block.enabled = true;
    model->event_block.calendar_id = "ops-calendar";
    model->event_block.calendar_version = "v1";
    model->event_block.active_event_codes = {"deploy"};
    model->event_block.coeff = {1.0};

    ApplyFormalModel(&core, "svc-match", model);
    ApplyFormalModel(&core, "svc-other", model);

    DetectorSubmitOutput match_submit;
    DetectorSubmitOutput other_submit;
    assert(core.Submit(RatioObservation{Ref("svc-match"), 20, 30.0, 100.0}, &match_submit) ==
           error::OK);
    assert(core.Submit(RatioObservation{Ref("svc-other"), 20, 30.0, 100.0}, &other_submit) ==
           error::OK);

    assert(match_submit.detector_result.evidence.kind == BaselineEvidenceKind::kRatio);
    assert(other_submit.detector_result.evidence.kind == BaselineEvidenceKind::kRatio);
    assert(match_submit.detector_result.evidence.ratio.p_hat_t >
           other_submit.detector_result.evidence.ratio.p_hat_t);
    assert(NearlyEqual(other_submit.detector_result.evidence.ratio.p_hat_t, 0.3));

    std::printf("[PASS] RatioDetectorCore key_feature event calendar\n");
}

static void TestRatioDetectorCoreProfileDifferenceIsEffective() {
    std::printf("[TEST] RatioDetectorCore profile difference is effective...\n");

    RatioDetectorCoreSpec rate_spec;
    rate_spec.owner_task_id = "ratio-task-4a";
    rate_spec.routed_feature_id = "svc_success_rate";
    rate_spec.feature_type = "ratio";
    rate_spec.feature_profile = "rate_core";

    RatioDetectorCoreSpec bursty_spec = rate_spec;
    bursty_spec.owner_task_id = "ratio-task-4b";
    bursty_spec.feature_profile = "ratio_bursty";

    RatioDetectorCore rate_core(rate_spec);
    RatioDetectorCore ratio_bursty(bursty_spec);
    ApplyFormalModel(&rate_core, "svc-profile", BuildFormalModel(0.5, 1));
    ApplyFormalModel(&ratio_bursty, "svc-profile", BuildFormalModel(0.5, 1));

    DetectorSubmitOutput rate_submit;
    DetectorSubmitOutput bursty_submit;
    const RatioObservation obs{Ref("svc-profile"), 45, 30.0, 40.0};
    assert(rate_core.Submit(obs, &rate_submit) == error::OK);
    assert(ratio_bursty.Submit(obs, &bursty_submit) == error::OK);

    const double rate_rho = std::sqrt(1.0 + 50.0 / 40.0);
    const double bursty_rho = std::sqrt(1.0 + 100.0 / 40.0);
    assert(rate_submit.detector_result.raw_score > 0.0);
    assert(bursty_submit.detector_result.raw_score == 0.0);
    assert(NearlyEqual(rate_submit.detector_result.confidence, 0.7 / rate_rho));
    assert(NearlyEqual(bursty_submit.detector_result.confidence, 0.7 / bursty_rho));
    assert(rate_submit.detector_result.confidence > bursty_submit.detector_result.confidence);

    std::printf("[PASS] RatioDetectorCore profile difference is effective\n");
}

static void TestRatioDetectorCoreColdStartUsesNoneProvider() {
    std::printf("[TEST] RatioDetectorCore cold start uses none provider...\n");

    RatioDetectorCoreSpec spec;
    spec.owner_task_id = "ratio-task-5";
    spec.routed_feature_id = "svc_success_rate";
    spec.feature_type = "ratio";
    spec.feature_profile = "rate_core";

    RatioDetectorCore core(spec);

    DetectorSubmitOutput submit;
    const RatioObservation obs{Ref("svc-none"), 50, 8.0, 10.0};
    assert(core.Submit(obs, &submit) == error::OK);

    assert(submit.detector_result.provider == BaselineProvider::kNone);
    assert(submit.detector_result.raw_score == 0.0);
    assert(submit.detector_result.normalized_score == 0.0);
    assert(submit.detector_result.confidence == 0.0);
    assert(submit.detector_result.reason_code == BaselineReasonCode::kUnknown);
    assert(submit.detector_result.evidence.kind == BaselineEvidenceKind::kNone);

    std::printf("[PASS] RatioDetectorCore cold start uses none provider\n");
}

static void TestRatioDetectorCoreShadowConfidenceAndReason() {
    std::printf("[TEST] RatioDetectorCore shadow confidence and reason...\n");

    RatioDetectorCoreSpec spec;
    spec.owner_task_id = "ratio-task-6";
    spec.routed_feature_id = "svc_success_rate";
    spec.feature_type = "ratio";
    spec.feature_profile = "rate_core";

    RatioDetectorCore core(spec);
    ApplyFormalModel(&core, "svc-shadow", BuildFormalModel(0.1, 1));

    DetectorSubmitOutput first;
    DetectorSubmitOutput second;
    DetectorSubmitOutput third;
    DetectorSubmitOutput fourth;
    DetectorSubmitOutput fifth;
    assert(core.Submit(RatioObservation{Ref("svc-shadow"), 201, 90.0, 100.0}, &first) == error::OK);
    assert(core.Submit(RatioObservation{Ref("svc-shadow"), 202, 90.0, 100.0}, &second) == error::OK);
    assert(core.Submit(RatioObservation{Ref("svc-shadow"), 203, 90.0, 100.0}, &third) == error::OK);
    assert(core.Submit(RatioObservation{Ref("svc-shadow"), 204, 90.0, 100.0}, &fourth) == error::OK);
    assert(core.Submit(RatioObservation{Ref("svc-shadow"), 205, 90.0, 100.0}, &fifth) == error::OK);

    const double rho_t = std::sqrt(1.0 + 50.0 / 100.0);
    assert((first.detector_result.flags & kBaselineFlagShadowActive) == 0);
    assert((second.detector_result.flags & kBaselineFlagShadowActive) == 0);
    assert((third.detector_result.flags & kBaselineFlagShadowActive) == 0);
    assert((fourth.detector_result.flags & kBaselineFlagShadowActive) == 0);
    assert((fifth.detector_result.flags & kBaselineFlagShadowActive) != 0);
    assert(fifth.detector_result.provider == BaselineProvider::kShadow);
    assert(NearlyEqual(fifth.detector_result.confidence, 0.8 / rho_t));
    assert(fifth.detector_result.reason == BaselineReasonCode::kBaselineShiftUp);
    assert(fifth.detector_result.evidence.kind == BaselineEvidenceKind::kRatio);
    assert(fifth.detector_result.evidence.ratio.shadow_active);
    assert(fifth.detector_result.evidence.ratio.model_state == BaselineModelState::kShadow);

    std::printf("[PASS] RatioDetectorCore shadow confidence and reason\n");
}

static void TestRatioDetectorCorePrunesIdleKeys() {
    std::printf("[TEST] RatioDetectorCore prunes idle keys...\n");

    RatioDetectorCoreSpec spec;
    spec.owner_task_id = "ratio-task-prune";
    spec.routed_feature_id = "svc_success_rate";
    spec.feature_type = "ratio";
    spec.feature_profile = "rate_core";

    RatioDetectorCore core(spec);

    DetectorSubmitOutput submit;
    assert(core.Submit(RatioObservation{Ref("svc-stale-a"), 1, 8.0, 10.0}, &submit) ==
           error::OK);
    assert(core.Submit(RatioObservation{Ref("svc-stale-b"), 1, 7.0, 10.0}, &submit) ==
           error::OK);

    for (int64_t bucket = 5000; bucket < 5070; ++bucket) {
        assert(core.Submit(
                   RatioObservation{Ref("svc-hot"),
                                    bucket,
                                    static_cast<double>((bucket % 50) + 40),
                                    100.0},
                   &submit) == error::OK);
    }

    assert(core.IdlePruneBucketGap() > 0);
    assert(core.PrunedKeyCount() >= 2);
    assert(core.Size() == 1);

    RatioSeriesSnapshot hot_snapshot;
    assert(core.BuildSeriesSnapshot(Ref("svc-hot"), &hot_snapshot) == error::OK);
    assert(core.BuildSeriesSnapshot(Ref("svc-stale-a"), &hot_snapshot) == error::NOT_FOUND);
    assert(core.BuildSeriesSnapshot(Ref("svc-stale-b"), &hot_snapshot) == error::NOT_FOUND);

    std::printf("[PASS] RatioDetectorCore prunes idle keys\n");
}

}  // namespace

int main() {
    TestRatioDetectorCoreSubmitAndSnapshot();
    TestRatioDetectorCoreMarkRebuildFailure();
    TestRatioDetectorCoreRatioEvidenceAndIdentity();
    TestRatioDetectorCoreConfiguredSourceConfidence();
    TestRatioDetectorCoreUsesShardedRuntimeStorage();
    TestRatioDetectorCoreKeyFeatureEventCalendar();
    TestRatioDetectorCoreProfileDifferenceIsEffective();
    TestRatioDetectorCoreColdStartUsesNoneProvider();
    TestRatioDetectorCoreShadowConfidenceAndReason();
    TestRatioDetectorCorePrunesIdleKeys();
    std::printf("[DONE] test_baseline_ratio_task\n");
    return 0;
}
