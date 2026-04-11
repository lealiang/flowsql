/*
 * Copyright (C) 2026 LIHUO
 *
 * Licensed under the MIT License. See LICENSE file in the project root
 * for full license information.
 *
 */

#ifndef _FLOWSQL_FRAMEWORK_BUILTIN_DATAFRAME_DATAFRAME_DISPATCH_STREAM_OPERATOR_H_
#define _FLOWSQL_FRAMEWORK_BUILTIN_DATAFRAME_DATAFRAME_DISPATCH_STREAM_OPERATOR_H_

#include <framework/core/dataframe.h>
#include <framework/interfaces/ioperator.h>
#include <framework/interfaces/istream_channel.h>

#include <cstdint>
#include <memory>
#include <string>
#include <unordered_set>
#include <vector>

namespace flowsql {

class DataframeDispatchStreamOperator : public IOperator {
 public:
    DataframeDispatchStreamOperator() = default;
    ~DataframeDispatchStreamOperator() override = default;

    std::string Category() override { return "builtin"; }
    std::string Name() override { return "dataframe_dispatch_stream"; }
    std::string Description() override { return "Dispatch dataframe rows into stream_hub partitions"; }
    OperatorPosition Position() override { return OperatorPosition::DATA; }

    int Work(IChannel* in, IChannel* out) override;
    int Configure(const char* key, const char* value) override;
    std::string LastError() override { return last_error_; }

 private:
    enum class DispatchStrategy {
        kRoundRobin,
        kHash,
        kRange,
    };

    struct DispatchConfig {
        bool has_strategy = false;
        DispatchStrategy strategy = DispatchStrategy::kRange;
        bool has_field_name = false;
        std::string field_name;
        bool has_range_rows = false;
        int64_t range_rows = 0;
        int64_t emit_batch_rows = 256;
        std::unordered_set<std::string> keys;
    };

    struct EffectiveConfig {
        DispatchStrategy strategy = DispatchStrategy::kRange;
        std::string field_name;
        bool range_auto = true;
        int64_t range_rows = 0;
        int64_t emit_batch_rows = 256;
    };

    int ResolveSinkPartitions(IChannel* out,
                              std::vector<std::shared_ptr<IStreamChannel>>* partitions);
    int ResolveEffectiveConfig(const DispatchConfig& in, EffectiveConfig* out);
    int DispatchRoundRobin(const DataFrame& data,
                           const EffectiveConfig& cfg,
                           const std::vector<std::shared_ptr<IStreamChannel>>& partitions);
    int DispatchHash(const DataFrame& data,
                     const EffectiveConfig& cfg,
                     const std::vector<std::shared_ptr<IStreamChannel>>& partitions);
    int DispatchRange(const DataFrame& data,
                      const EffectiveConfig& cfg,
                      const std::vector<std::shared_ptr<IStreamChannel>>& partitions);
    int FlushPartitionBuffer(DataFrame* buffer,
                             const std::vector<Field>& schema,
                             IStreamChannel* partition,
                             int64_t ts_ms);

    static uint64_t HashFieldValue(const FieldValue& value);
    static void Fnv1aUpdate(uint64_t* state, const void* data, size_t len);
    static std::string ToLowerAscii(std::string s);
    static int64_t NowMs();
    void SetError(const char* code, const std::string& message);

    DispatchConfig cfg_;
    std::string invalid_key_;
    std::string last_error_;
};

}  // namespace flowsql

#endif  // _FLOWSQL_FRAMEWORK_BUILTIN_DATAFRAME_DATAFRAME_DISPATCH_STREAM_OPERATOR_H_
