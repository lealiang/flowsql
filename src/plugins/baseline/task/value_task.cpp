/*
 * Copyright (C) 2026 LIHUO
 *
 * Licensed under the MIT License. See LICENSE file in the project root
 * for full license information.
 *
 */

#include "value_task.h"

#include <common/error_code.h>

#include <memory>
#include <rapidjson/stringbuffer.h>
#include <rapidjson/writer.h>

#include "baseline_task_base.h"
#include "plugins/baseline/fusion/key_risk_fusion.h"
#include "plugins/baseline/model/event_calendar_matcher.h"
#include "plugins/baseline/model/formal_predictor.h"
#include "plugins/baseline/rebuild/candidate_builder.h"
#include "plugins/baseline/rebuild/candidate_validator.h"
#include "plugins/baseline/rebuild/formal_model_trainer.h"
#include "plugins/baseline/rebuild/rebuild_queue.h"
#include "plugins/baseline/rebuild/rebuild_worker.h"

namespace flowsql {
namespace baseline {

struct ValueHistoryBinding {
    mutable std::mutex mutex;
    IBaselineValueHistoryReader* reader = nullptr;

    bool HasReader() const {
        std::lock_guard<std::mutex> lock(mutex);
        return reader != nullptr;
    }
};

namespace {

std::string CopyKey(const BaselineStringRef& key) {
    if (!key.data || key.size == 0) return "";
    return std::string(key.data, key.size);
}

const EventCalendarSpec* TaskEventCalendar(const BaselineTaskSpec& spec) {
    return spec.event_calendar_spec ? &(*spec.event_calendar_spec) : nullptr;
}

const char* EventStatusForSnapshot(int predict_rc,
                                   const FormalPrediction& prediction) {
    if (predict_rc != error::OK || !prediction.ready) return "unavailable";
    return EventCalendarStatusName(prediction.event_status);
}

std::shared_ptr<ValueFormalModel> TrainFullValueModel(
    const ValueFeatureProfile& profile,
    const ValueReplaySeries& replay,
    uint64_t model_version,
    const BaselineTaskSpec* task_spec,
    const EventCalendarSpec* event_calendar_spec,
    const CompiledEventCalendar* compiled_event_calendar) {
    ValueFormalTrainResult train_result;
    const ValueFormalTrainInput input{
        &profile,
        &replay,
        replay.points.size(),
        model_version,
        0,
        replay.window,
        task_spec,
        task_spec ? task_spec->delta : 0,
        task_spec ? task_spec->tz : "",
        event_calendar_spec,
        compiled_event_calendar};
    if (FormalModelTrainer::TrainValue(input, &train_result) !=
        FormalTrainFailureCode::kNone) {
        return nullptr;
    }
    return train_result.model;
}

bool CompileSeriesEventCalendar(const BaselineTaskSpec& spec,
                                const std::string& series_key,
                                CompiledEventCalendar* out_calendar) {
    if (!out_calendar || !spec.event_calendar_spec) return false;
    BaselineTaskSpec series_spec = spec;
    series_spec.key = series_key;
    std::string err;
    return CompileEventCalendar(*spec.event_calendar_spec, series_spec, out_calendar, &err) ==
           error::OK;
}

}  // namespace

BaselineValueTask::BaselineValueTask(TaskRegistry* registry,
                                     RebuildQueue* rebuild_queue,
                                     std::string task_id,
                                     const BaselineTaskSpec& spec,
                                     KeyRiskFusion* key_risk_fusion)
    : BaselineTaskBase(registry,
                       rebuild_queue,
                       std::move(task_id),
                       BaselineTaskKind::kValue,
                       spec.name,
                       spec.config_json),
      spec_(spec),
      history_binding_(std::make_shared<ValueHistoryBinding>()),
      key_risk_fusion_(key_risk_fusion) {
    ValueDetectorCoreSpec core_spec;
    core_spec.owner_task_id = TaskId();
    core_spec.routed_feature_id = spec_.feature;
    core_spec.feature_type = spec_.feature_type;
    core_spec.feature_profile = spec_.feature_profile;
    core_spec.event_calendar_spec = spec_.event_calendar_spec;
    core_spec.baseline_source_configs = spec_.baseline_source_configs;
    core_ = std::make_shared<ValueDetectorCore>(core_spec);

    rebuild_runtime_ = std::make_shared<RebuildTaskRuntime>(
        TaskId(),
        [this](const RebuildRequest& request) { return ExecuteRebuild(request); });
}

const char* BaselineValueTask::Id() const { return BaselineTaskBase::Id(); }
const char* BaselineValueTask::Name() const { return BaselineTaskBase::Name(); }
BaselineTaskKind BaselineValueTask::Kind() const { return BaselineTaskBase::Kind(); }
const char* BaselineValueTask::ConfigJson() const {
    return BaselineTaskBase::ConfigJson();
}

int BaselineValueTask::QueryTaskSnapshotJson(std::string* out_json) const {
    if (!out_json) return error::BAD_REQUEST;

    RebuildTaskRuntimeSnapshot rebuild_snapshot;
    if (rebuild_runtime_) rebuild_runtime_->Snapshot(&rebuild_snapshot);

    const auto& profile = core_->profile();

    rapidjson::StringBuffer buf;
    rapidjson::Writer<rapidjson::StringBuffer> writer(buf);
    writer.StartObject();
    writer.Key("task_id");
    writer.String(Id());
    writer.Key("name");
    writer.String(Name());
    writer.Key("kind");
    writer.String("value");
    writer.Key("feature_type");
    writer.String(profile.feature_type.c_str());
    writer.Key("feature_profile");
    writer.String(profile.feature_profile.c_str());
    writer.Key("transform_name");
    writer.String(profile.transform_name.c_str());
    writer.Key("baseline_source_config_count");
    writer.Uint64(spec_.baseline_source_configs.size());
    writer.Key("series_count");
    writer.Uint64(core_ ? core_->Size() : 0);
    writer.Key("event_calendar_present");
    writer.Bool(spec_.event_calendar_spec.has_value());
    writer.Key("event_calendar_id");
    writer.String(spec_.event_calendar_spec ? spec_.event_calendar_spec->calendar_id.c_str()
                                            : "");
    writer.Key("event_calendar_version");
    writer.String(spec_.event_calendar_spec
                      ? spec_.event_calendar_spec->calendar_version.c_str()
                      : "");
    writer.Key("event_calendar_entry_count");
    writer.Uint64(spec_.event_calendar_spec ? spec_.event_calendar_spec->entries.size() : 0);
    writer.Key("reader_bound");
    writer.Bool(history_binding_ && history_binding_->HasReader());
    writer.Key("rebuild_pending");
    writer.Uint64(rebuild_snapshot.pending_count);
    writer.Key("rebuild_inflight");
    writer.Uint64(rebuild_snapshot.inflight_count);
    writer.Key("rebuild_completed");
    writer.Uint64(rebuild_snapshot.completed_count);
    writer.Key("last_rebuild_status");
    writer.Int(rebuild_snapshot.last_status);
    writer.Key("last_rebuild_reason");
    writer.String(rebuild_snapshot.last_reason.c_str());
    writer.Key("last_rebuild_key");
    writer.String(rebuild_snapshot.last_key.c_str());
    writer.EndObject();
    *out_json = buf.GetString();
    return error::OK;
}

int BaselineValueTask::QuerySeriesSnapshotJson(const BaselineStringRef& key,
                                               std::string* out_json) const {
    if (!out_json) return error::BAD_REQUEST;

    ValueSeriesSnapshot snapshot;
    int rc = core_->BuildSeriesSnapshot(key, &snapshot);
    if (rc != error::OK) return rc;

    rapidjson::StringBuffer buf;
    rapidjson::Writer<rapidjson::StringBuffer> writer(buf);
    writer.StartObject();
    writer.Key("task_id");
    writer.String(Id());
    writer.Key("key");
    const std::string key_copy = CopyKey(key);
    writer.String(key_copy.c_str());
    writer.Key("model_state");
    writer.String(snapshot.runtime_state.model_state.c_str());
    writer.Key("baseline_source_kind");
    writer.String(BaselineSourceKindName(
        snapshot.runtime_state.baseline_source.selected_kind));
    writer.Key("baseline_source_key");
    writer.String(snapshot.runtime_state.baseline_source.selected_source_key.c_str());
    writer.Key("task_calendar_present");
    writer.Bool(spec_.event_calendar_spec.has_value());
    writer.Key("task_calendar_id");
    writer.String(spec_.event_calendar_spec ? spec_.event_calendar_spec->calendar_id.c_str()
                                            : "");
    writer.Key("task_calendar_version");
    writer.String(spec_.event_calendar_spec
                      ? spec_.event_calendar_spec->calendar_version.c_str()
                      : "");
    writer.Key("last_bucket_id");
    writer.Int64(snapshot.series_state.last_bucket_id);
    writer.Key("observation_count");
    writer.Uint64(snapshot.series_state.observation_count);
    writer.Key("persistence");
    writer.Uint(snapshot.series_state.persistence);
    writer.Key("last_sample_count");
    writer.Uint64(snapshot.runtime_state.last_sample_count);
    writer.Key("last_value");
    writer.Double(snapshot.runtime_state.last_value);
    writer.Key("last_x");
    writer.Double(snapshot.runtime_state.last_x);
    writer.Key("last_rho");
    writer.Double(snapshot.runtime_state.last_rho);
    writer.Key("last_gate_score");
    writer.Bool(snapshot.runtime_state.last_gate_score);
    writer.Key("last_gate_shift");
    writer.Bool(snapshot.runtime_state.last_gate_shift);
    writer.Key("last_p_shift");
    writer.Double(snapshot.runtime_state.last_p_shift);
    writer.Key("last_shift_confirmed");
    writer.Bool(snapshot.runtime_state.last_shift_confirmed);
    writer.Key("drift_direction");
    writer.String(DriftDirectionName(snapshot.runtime_state.drift_state.direction));
    writer.Key("shadow_active");
    writer.Bool(snapshot.runtime_state.shadow_state.active);
    writer.Key("shadow_ref_kind");
    writer.String(ShadowRefKindName(snapshot.runtime_state.shadow_state.ref_kind));
    writer.Key("shadow_ref_source_key");
    writer.String(snapshot.runtime_state.shadow_state.ref_source_key.c_str());
    writer.Key("shadow_ref_model_version");
    writer.Uint64(snapshot.runtime_state.shadow_state.ref_model_version);
    writer.Key("shadow_delta");
    writer.Double(snapshot.runtime_state.shadow_state.delta);
    writer.Key("formal_ready");
    writer.Bool(snapshot.runtime_state.formal_state.formal_ready);
    writer.Key("formal_model_kind");
    writer.String(snapshot.runtime_state.formal_state.formal_model_kind.c_str());
    writer.Key("formal_model_version");
    writer.Uint64(snapshot.runtime_state.formal_state.formal_model_version);
    writer.Key("formal_calendar_present");
    writer.Bool(snapshot.formal_calendar_present);
    writer.Key("formal_calendar_id");
    writer.String(snapshot.formal_calendar_present
                      ? snapshot.runtime_state.formal_model->metadata.calendar_id.c_str()
                      : "");
    writer.Key("formal_calendar_version");
    writer.String(snapshot.formal_calendar_present
                      ? snapshot.runtime_state.formal_model->metadata.calendar_version.c_str()
                      : "");
    writer.Key("candidate_generation");
    writer.Uint64(snapshot.runtime_state.formal_state.candidate_generation);
    writer.Key("candidate_model_kind");
    writer.String(snapshot.runtime_state.formal_state.candidate_model_kind.c_str());
    writer.Key("candidate_model_version");
    writer.Uint64(snapshot.runtime_state.formal_state.candidate_model_version);
    writer.Key("candidate_state");
    writer.String(snapshot.runtime_state.formal_state.candidate_state.c_str());
    writer.Key("switch_state");
    writer.String(snapshot.runtime_state.formal_state.switch_state.c_str());
    writer.Key("last_candidate_loss");
    writer.Double(snapshot.runtime_state.formal_state.last_candidate_loss);
    writer.Key("last_incumbent_loss");
    writer.Double(snapshot.runtime_state.formal_state.last_incumbent_loss);
    writer.Key("last_validation_count");
    writer.Uint64(snapshot.runtime_state.formal_state.last_validation_count);
    writer.Key("candidate_calendar_present");
    writer.Bool(snapshot.candidate_calendar_present);
    writer.Key("candidate_calendar_id");
    writer.String(snapshot.candidate_calendar_present
                      ? snapshot.runtime_state.candidate_model->metadata.calendar_id.c_str()
                      : "");
    writer.Key("candidate_calendar_version");
    writer.String(snapshot.candidate_calendar_present
                      ? snapshot.runtime_state.candidate_model->metadata.calendar_version.c_str()
                      : "");
    writer.Key("formal_predict_ready");
    writer.Bool(snapshot.formal_predict_status == error::OK &&
                snapshot.formal_prediction.ready);
    writer.Key("formal_predict_bucket_id");
    writer.Int64(snapshot.formal_prediction.bucket_id);
    writer.Key("formal_predict_value");
    writer.Double(snapshot.formal_prediction.value);
    writer.Key("formal_predict_sigma_ref");
    writer.Double(snapshot.formal_prediction.sigma_ref);
    writer.Key("formal_event_enabled");
    writer.Bool(snapshot.formal_predict_status == error::OK &&
                snapshot.formal_prediction.ready &&
                snapshot.formal_prediction.event_enabled);
    writer.Key("formal_event_status");
    writer.String(EventStatusForSnapshot(snapshot.formal_predict_status,
                                         snapshot.formal_prediction));
    writer.Key("candidate_predict_ready");
    writer.Bool(snapshot.candidate_predict_status == error::OK &&
                snapshot.candidate_prediction.ready);
    writer.Key("candidate_predict_bucket_id");
    writer.Int64(snapshot.candidate_prediction.bucket_id);
    writer.Key("candidate_predict_value");
    writer.Double(snapshot.candidate_prediction.value);
    writer.Key("candidate_predict_sigma_ref");
    writer.Double(snapshot.candidate_prediction.sigma_ref);
    writer.Key("candidate_event_enabled");
    writer.Bool(snapshot.candidate_predict_status == error::OK &&
                snapshot.candidate_prediction.ready &&
                snapshot.candidate_prediction.event_enabled);
    writer.Key("candidate_event_status");
    writer.String(EventStatusForSnapshot(snapshot.candidate_predict_status,
                                         snapshot.candidate_prediction));
    writer.Key("last_rebuild_bucket_start");
    writer.Int64(snapshot.runtime_state.formal_state.last_replay_window.request_bucket_start);
    writer.Key("last_rebuild_bucket_end");
    writer.Int64(snapshot.runtime_state.formal_state.last_replay_window.request_bucket_end);
    writer.Key("last_replay_observation_count");
    writer.Uint64(snapshot.runtime_state.formal_state.last_replay_window.observation_count);
    writer.Key("last_replay_first_bucket_id");
    writer.Int64(snapshot.runtime_state.formal_state.last_replay_window.first_bucket_id);
    writer.Key("last_replay_last_bucket_id");
    writer.Int64(snapshot.runtime_state.formal_state.last_replay_window.last_bucket_id);
    writer.Key("last_train_observation_count");
    writer.Uint64(snapshot.runtime_state.formal_state.last_train_window.observation_count);
    writer.Key("last_train_first_bucket_id");
    writer.Int64(snapshot.runtime_state.formal_state.last_train_window.first_bucket_id);
    writer.Key("last_train_last_bucket_id");
    writer.Int64(snapshot.runtime_state.formal_state.last_train_window.last_bucket_id);
    writer.Key("last_holdout_observation_count");
    writer.Uint64(snapshot.runtime_state.formal_state.last_holdout_window.observation_count);
    writer.Key("last_holdout_first_bucket_id");
    writer.Int64(snapshot.runtime_state.formal_state.last_holdout_window.first_bucket_id);
    writer.Key("last_holdout_last_bucket_id");
    writer.Int64(snapshot.runtime_state.formal_state.last_holdout_window.last_bucket_id);
    writer.EndObject();
    *out_json = buf.GetString();
    return error::OK;
}

int BaselineValueTask::RequestRebuild(const BaselineStringRef& key,
                                      BaselineRebuildReason reason) {
    if (!key.data || key.size == 0) return error::BAD_REQUEST;

    SeriesState series_state;
    const int state_rc = core_->GetSeriesState(key, &series_state);

    std::lock_guard<std::mutex> lock(mutex_);
    if (EnsureOpenLocked() != error::OK) return error::UNAVAILABLE;
    if (!rebuild_queue_ || !rebuild_runtime_) return error::UNAVAILABLE;
    if (!rebuild_runtime_->PrepareEnqueue()) return error::UNAVAILABLE;

    RebuildRequest request;
    request.task_kind = BaselineTaskKind::kValue;
    request.task_id = TaskId();
    request.feature_name = spec_.feature;
    request.key = CopyKey(key);
    request.rebuild_reason = reason;
    request.bucket_start_hint = 0;
    request.bucket_end = (state_rc == error::OK) ? series_state.last_bucket_id : 0;
    request.runtime = rebuild_runtime_;

    const int rc = rebuild_queue_->Push(request);
    if (rc != error::OK) rebuild_runtime_->OnCanceled(request);
    return rc;
}

int BaselineValueTask::Close() {
    return BaselineTaskBase::Close();
}

int BaselineValueTask::SetHistoryReader(IBaselineValueHistoryReader* reader) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (EnsureOpenLocked() != error::OK) return error::UNAVAILABLE;
    if (rebuild_runtime_ && !rebuild_runtime_->CanSwapReader()) return error::CONFLICT;
    if (history_binding_) {
        std::lock_guard<std::mutex> binding_lock(history_binding_->mutex);
        history_binding_->reader = reader;
    }
    return error::OK;
}

int BaselineValueTask::ExecuteRebuild(const RebuildRequest& request) {
    IBaselineValueHistoryReader* reader = nullptr;
    {
        std::lock_guard<std::mutex> lock(history_binding_->mutex);
        reader = history_binding_->reader;
    }
    if (!reader) {
        core_->MarkRebuildFailure(
            DetectorRebuildFailure{request.key,
                                   request.bucket_start_hint,
                                   request.bucket_end,
                                   "fetch_failed",
                                   "rebuild_blocked"});
        return error::UNAVAILABLE;
    }

    const BaselineStringRef key_ref{
        request.key.c_str(),
        static_cast<uint32_t>(request.key.size())};
    const HistoryFetchRequest fetch_req{
        key_ref,
        BaselineStringRef{spec_.feature.c_str(), static_cast<uint32_t>(spec_.feature.size())},
        request.bucket_start_hint,
        request.bucket_end};

    ValueReplayRunner runner(request.key);
    const int fetch_rc = reader->Fetch(fetch_req, [&runner](const ValueObservation& obs) {
        return runner.Push(obs);
    });
    if (fetch_rc != error::OK) {
        core_->MarkRebuildFailure(DetectorRebuildFailure{
            request.key,
            request.bucket_start_hint,
            request.bucket_end,
            fetch_rc == error::BAD_REQUEST ? "replay_failed" : "fetch_failed"});
        return fetch_rc;
    }

    ValueReplaySeries replay;
    runner.Finalize(request.bucket_start_hint, request.bucket_end, &replay);

    ValueRebuildContext rebuild_context;
    core_->BuildRebuildContext(request.key, &rebuild_context);

    CompiledEventCalendar compiled_event_calendar;
    const CompiledEventCalendar* compiled_event_calendar_ptr =
        CompileSeriesEventCalendar(spec_, request.key, &compiled_event_calendar)
            ? &compiled_event_calendar
            : nullptr;

    BaselineTaskSpec series_task_spec = spec_;
    series_task_spec.key = request.key;

    ValueCandidateBuildResult build_result;
    CandidateBuilder::BuildValue(
        core_->profile(),
        replay,
        rebuild_context.next_model_version,
        &series_task_spec,
        spec_.delta,
        spec_.tz,
        TaskEventCalendar(spec_),
        compiled_event_calendar_ptr,
        &build_result);
    CandidateValidationResult validation_result;
    std::shared_ptr<ValueFormalModel> full_model;
    if (build_result.status == CandidateBuildStatus::kTrained &&
        build_result.candidate_model) {
        validation_result = CandidateValidator::ValidateValue(
            core_->profile(),
            replay,
            build_result.holdout_window,
            build_result.candidate_model.get(),
            rebuild_context.incumbent_shadow_state.active
                ? nullptr
                : rebuild_context.incumbent_formal_model.get(),
            rebuild_context.incumbent_shadow_state.active
                ? &rebuild_context.incumbent_shadow_state
                : nullptr);
        if (validation_result.pass) {
            full_model = TrainFullValueModel(
                core_->profile(),
                replay,
                build_result.candidate_model_version,
                &series_task_spec,
                TaskEventCalendar(spec_),
                compiled_event_calendar_ptr);
            if (!full_model) {
                validation_result.pass = false;
                validation_result.status = CandidateValidationStatus::kFailed;
            }
        }
    }

    ValueApplyFormalModelResult apply_result;
    apply_result.replay_window = build_result.replay_window;
    apply_result.train_window = build_result.train_window;
    apply_result.holdout_window = build_result.holdout_window;
    apply_result.candidate_loss = validation_result.candidate_loss;
    apply_result.incumbent_loss = validation_result.incumbent_loss;
    apply_result.validation_count = validation_result.validation_count;
    if (build_result.status == CandidateBuildStatus::kTrained &&
        build_result.candidate_model) {
        apply_result.candidate_trained = true;
        apply_result.candidate_generation =
            build_result.candidate_model->metadata.model_version;
        apply_result.switch_state =
            validation_result.pass && full_model
                ? (validation_result.status ==
                           CandidateValidationStatus::kBypassNoIncumbent
                       ? "direct_apply"
                       : "formal_apply")
                : CandidateValidationStatusName(validation_result.status);
        apply_result.full_model = full_model;
    } else {
        apply_result.candidate_state =
            CandidateBuildStatusName(build_result.status);
    }

    core_->ApplyFormalModel(request.key, apply_result);
    return error::OK;
}

int BaselineValueTask::SubmitObservation(const ValueObservation& obs,
                                         DetectorResult* out) {
    if (!out) return error::BAD_REQUEST;
    *out = DetectorResult{};

    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (EnsureOpenLocked() != error::OK) {
            out->status = error::UNAVAILABLE;
            return error::UNAVAILABLE;
        }
    }

    DetectorSubmitOutput submit_output;
    const int rc = core_->Submit(obs, &submit_output);
    *out = submit_output.detector_result;
    if (rc != error::OK) return rc;

    if (key_risk_fusion_) {
        key_risk_fusion_->UpdateSingleDetectorResult(
            obs.bucket_id,
            FusionSourceId{TaskId(), FusionSourceKind::kDirectSingle, 0},
            *out);
    }

    if (submit_output.rebuild_intent.required) {
        RebuildRequest request;
        request.task_kind = BaselineTaskKind::kValue;
        request.task_id = TaskId();
        request.feature_name = spec_.feature;
        request.key = CopyKey(obs.key);
        request.rebuild_reason = submit_output.rebuild_intent.reason;
        request.bucket_start_hint = submit_output.rebuild_intent.rebuild_start_hint;
        request.bucket_end = submit_output.rebuild_intent.bucket_end;
        request.runtime = rebuild_runtime_;

        std::lock_guard<std::mutex> lock(mutex_);
        if (EnsureOpenLocked() == error::OK && rebuild_queue_ && rebuild_runtime_ &&
            rebuild_runtime_->PrepareEnqueue()) {
            const int push_rc = rebuild_queue_->Push(request);
            if (push_rc == error::OK) {
                out->flags |= kBaselineFlagRebuildQueued;
            } else {
                rebuild_runtime_->OnCanceled(request);
                core_->ClearPendingRebuild(request.key);
            }
        } else {
            core_->ClearPendingRebuild(request.key);
        }
    }

    return error::OK;
}

void BaselineValueTask::OnClosingLocked() {
    if (history_binding_) {
        std::lock_guard<std::mutex> binding_lock(history_binding_->mutex);
        history_binding_->reader = nullptr;
    }
    if (rebuild_queue_) rebuild_queue_->CancelTask(TaskId());
    if (rebuild_runtime_) rebuild_runtime_->CloseAndWait();
    if (core_) core_->Clear();
    if (key_risk_fusion_) key_risk_fusion_->RemoveTaskContributions(TaskId());
}

}  // namespace baseline
}  // namespace flowsql
