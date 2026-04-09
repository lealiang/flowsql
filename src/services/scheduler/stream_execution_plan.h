/*
 * Copyright (C) 2026 LIHUO
 *
 * Licensed under the MIT License. See LICENSE file in the project root
 * for full license information.
 *
 */

#ifndef _FLOWSQL_SCHEDULER_STREAM_EXECUTION_PLAN_H_
#define _FLOWSQL_SCHEDULER_STREAM_EXECUTION_PLAN_H_

#include <framework/core/sql_parser.h>
#include <framework/interfaces/ichannel.h>
#include <framework/interfaces/ioperator.h>
#include <framework/interfaces/istream_channel.h>
#include <framework/interfaces/istream_operator.h>

#include <functional>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

namespace flowsql {
namespace scheduler {

struct StreamExecutionPlan {
    SqlStatement stmt;
    std::string runtime_task_id;
    std::string lease_owner_id;
    bool skip_lease_acquire = false;

    std::vector<OperatorRef> parsed_ops;
    std::unordered_map<std::string, std::string> with_params;

    std::vector<std::shared_ptr<IChannel>> source_channel_holders;
    std::vector<std::shared_ptr<IStreamChannel>> source_channels;
    std::vector<std::string> source_keys;
    std::vector<std::string> resolved_sources;
    std::string source_expand_rule = "explicit";
    std::string shared_hub_key;
    std::string where_signature;

    std::shared_ptr<IChannel> sink_channel;
    std::string sink_type;
    std::string db_type;
    std::string db_name;
    std::string table_name;
    StreamSinkContext sink_ctx;
    std::vector<std::string> sink_keys;

    std::vector<std::string> lease_keys;
    std::unordered_map<std::string, uint64_t> version_snapshot;

    std::shared_ptr<IStreamChannel> source;
    std::shared_ptr<IOperator> first_operator_holder;
    std::shared_ptr<IStreamOperator> first_stream_operator;

    ParallelStrategy strategy = ParallelStrategy::NONE;
    int parallelism = 1;
    StreamChannelCapabilities source_caps;
    StreamChannelCapabilities sink_caps;
};

class LeaseToken {
 public:
    LeaseToken() = default;
    explicit LeaseToken(std::function<void()> releaser);
    LeaseToken(LeaseToken&& other) noexcept;
    LeaseToken& operator=(LeaseToken&& other) noexcept;
    LeaseToken(const LeaseToken&) = delete;
    LeaseToken& operator=(const LeaseToken&) = delete;
    ~LeaseToken();

    void Commit();
    void Reset();
    bool Active() const { return active_; }

 private:
    std::function<void()> releaser_;
    bool active_ = false;
};

}  // namespace scheduler
}  // namespace flowsql

#endif  // _FLOWSQL_SCHEDULER_STREAM_EXECUTION_PLAN_H_
