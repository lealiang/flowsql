/*
 * Copyright (C) 2026 LIHUO
 *
 * Licensed under the MIT License. See LICENSE file in the project root
 * for full license information.
 *
 */

#ifndef _FLOWSQL_FRAMEWORK_INTERFACES_IBASELINE_SERVICE_H_
#define _FLOWSQL_FRAMEWORK_INTERFACES_IBASELINE_SERVICE_H_

#include <common/guid.h>
#include <common/typedef.h>
#include <framework/interfaces/ibaseline_types.h>

#include <memory>
#include <string_view>
#include <utility>

namespace flowsql {

// {f27073e4-98c7-4749-bd3d-83bd1b0d3a20}
const Guid IID_BASELINE_SERVICE = {
    0xf27073e4, 0x98c7, 0x4749, {0xbd, 0x3d, 0x83, 0xbd, 0x1b, 0x0d, 0x3a, 0x20}
};

interface IBaselineTask {
    virtual ~IBaselineTask() = default;

    virtual const char* Id() const = 0;
    virtual const char* Name() const = 0;
    virtual BaselineTaskKind Kind() const = 0;

    virtual BaselineSerializationResult ExportConfig(
        BaselineSerializationFormat format) const = 0;

    virtual BaselineSerializationResult QueryTaskSnapshot(
        BaselineSerializationFormat format) const = 0;

    virtual BaselineSerializationResult QuerySeriesSnapshot(
        std::string_view series_key,
        BaselineSerializationFormat format) const = 0;

    virtual BaselineStatus Close() = 0;
};

interface IBaselineValueTask : public IBaselineTask {
    virtual RollingBaselineResult SubmitObservation(
        const ValueRollingObservation& obs,
        const RollingSubmitOptions& options) = 0;
    virtual RollingPrediction PredictRolling(
        std::string_view series_key,
        int64_t bucket_id) const = 0;

    virtual BootstrapTrainResult Bootstrap(const ValueBootstrapInput& input) = 0;
    virtual BootstrapPrediction PredictBootstrap(
        std::string_view series_key,
        int64_t bucket_id,
        const BootstrapPredictionOptions& options) const = 0;
    virtual BaselineSerializationResult ExportBootstrapArtifact(
        BaselineSerializationFormat format) const = 0;
    virtual BaselineStatus LoadBootstrapArtifact(
        std::string_view content,
        BaselineSerializationFormat format) = 0;
    virtual BaselineSerializationResult ExportBootstrapSeed(
        BaselineSerializationFormat format) const = 0;
};

interface IBaselineRatioTask : public IBaselineTask {
    virtual RollingBaselineResult SubmitObservation(
        const RatioRollingObservation& obs,
        const RollingSubmitOptions& options) = 0;
    virtual RollingPrediction PredictRolling(
        std::string_view series_key,
        int64_t bucket_id) const = 0;

    virtual BootstrapTrainResult Bootstrap(const RatioBootstrapInput& input) = 0;
    virtual BootstrapPrediction PredictBootstrap(
        std::string_view series_key,
        int64_t bucket_id,
        const BootstrapPredictionOptions& options) const = 0;
    virtual BaselineSerializationResult ExportBootstrapArtifact(
        BaselineSerializationFormat format) const = 0;
    virtual BaselineStatus LoadBootstrapArtifact(
        std::string_view content,
        BaselineSerializationFormat format) = 0;
    virtual BaselineSerializationResult ExportBootstrapSeed(
        BaselineSerializationFormat format) const = 0;
};

interface IBaselineRelationTask : public IBaselineTask {
    virtual RelationRollingResult SubmitObservation(
        const RelationRollingObservation& obs,
        const RelationRollingSubmitOptions& options) = 0;
    virtual RollingPrediction PredictRoutedSummary(
        const RelationRoutedSummaryQuery& query,
        int64_t bucket_id) const = 0;
    virtual BaselineSerializationResult QueryRoutedSummarySnapshot(
        const RelationRoutedSummaryQuery& query,
        BaselineSerializationFormat format) const = 0;

    virtual BootstrapTrainResult Bootstrap(const RelationBootstrapInput& input) = 0;
    virtual BaselineSerializationResult ExportBootstrapArtifact(
        BaselineSerializationFormat format) const = 0;
    virtual BaselineStatus LoadBootstrapArtifact(
        std::string_view content,
        BaselineSerializationFormat format) = 0;
    virtual BaselineSerializationResult ExportBootstrapSeed(
        BaselineSerializationFormat format) const = 0;
    virtual BaselineSerializationResult QueryBootstrapBasis(
        BaselineSerializationFormat format) const = 0;
};

interface IBaselineService {
    virtual ~IBaselineService() = default;

    virtual std::pair<BaselineStatus, std::shared_ptr<IBaselineValueTask>>
    CreateValueTask(std::string_view config_content,
                    BaselineSerializationFormat format) = 0;

    virtual std::pair<BaselineStatus, std::shared_ptr<IBaselineRatioTask>>
    CreateRatioTask(std::string_view config_content,
                    BaselineSerializationFormat format) = 0;

    virtual std::pair<BaselineStatus, std::shared_ptr<IBaselineRelationTask>>
    CreateRelationTask(std::string_view config_content,
                       BaselineSerializationFormat format) = 0;

    virtual BaselineSerializationResult QueryServiceSnapshot(
        BaselineSerializationFormat format) const = 0;
};

}  // namespace flowsql

#endif  // _FLOWSQL_FRAMEWORK_INTERFACES_IBASELINE_SERVICE_H_
