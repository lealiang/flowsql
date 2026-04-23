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

#include <functional>
#include <string>

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
    virtual const char* ConfigJson() const = 0;

    virtual int QueryTaskSnapshotJson(std::string* out_json) const = 0;
    virtual int QuerySeriesSnapshotJson(const BaselineStringRef& key,
                                        std::string* out_json) const = 0;

    virtual int RequestRebuild(const BaselineStringRef& key,
                               BaselineRebuildReason reason) = 0;

    virtual int Close() = 0;
};

interface IBaselineValueHistoryReader {
    virtual ~IBaselineValueHistoryReader() = default;
    virtual int Fetch(const HistoryFetchRequest& req,
                      std::function<int(const ValueObservation&)> on_point) = 0;
};

interface IBaselineRatioHistoryReader {
    virtual ~IBaselineRatioHistoryReader() = default;
    virtual int Fetch(const HistoryFetchRequest& req,
                      std::function<int(const RatioObservation&)> on_point) = 0;
};

interface IBaselineRelationHistoryReader {
    virtual ~IBaselineRelationHistoryReader() = default;
    virtual int Fetch(const HistoryFetchRequest& req,
                      std::function<int(const RelationObservationBlock&)> on_block) = 0;
};

interface IBaselineValueTask : public IBaselineTask {
    virtual int SetHistoryReader(IBaselineValueHistoryReader* reader) = 0;
    virtual int SubmitObservation(const ValueObservation& obs,
                                  DetectorResult* out) = 0;
};

interface IBaselineRatioTask : public IBaselineTask {
    virtual int SetHistoryReader(IBaselineRatioHistoryReader* reader) = 0;
    virtual int SubmitObservation(const RatioObservation& obs,
                                  DetectorResult* out) = 0;
};

interface IBaselineRelationTask : public IBaselineTask {
    virtual int SetHistoryReader(IBaselineRelationHistoryReader* reader) = 0;
    virtual int SubmitBlock(const RelationObservationBlock& block,
                            DetectorResult* out) = 0;
};

interface IBaselineService {
    virtual ~IBaselineService() = default;

    virtual int CreateValueTask(const char* config_json,
                                IBaselineValueTask** out) = 0;
    virtual int CreateRatioTask(const char* config_json,
                                IBaselineRatioTask** out) = 0;
    virtual int CreateRelationTask(const char* config_json,
                                   IBaselineRelationTask** out) = 0;

    virtual void ListTasks(std::function<void(const char* task_id,
                                              const char* task_name,
                                              BaselineTaskKind kind)> cb) = 0;

    virtual int QueryServiceStatsJson(std::string* out_json) const = 0;
};

}  // namespace flowsql

#endif  // _FLOWSQL_FRAMEWORK_INTERFACES_IBASELINE_SERVICE_H_
