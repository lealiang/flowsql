#ifndef _FLOWSQL_FRAMEWORK_BUILTIN_STREAM_PASSTHROUGH_STREAM_OPERATOR_H_
#define _FLOWSQL_FRAMEWORK_BUILTIN_STREAM_PASSTHROUGH_STREAM_OPERATOR_H_

#include <framework/interfaces/ioperator.h>
#include <framework/interfaces/istream_channel.h>
#include <framework/interfaces/istream_operator.h>

#include <atomic>
#include <memory>
#include <string>

namespace flowsql {

class PassthroughStreamOperator : public IOperator, public IStreamOperator {
 public:
    PassthroughStreamOperator() = default;
    ~PassthroughStreamOperator() override = default;

    // IOperator / IStreamOperator metadata
    std::string Category() override { return "builtin"; }
    std::string Name() override { return "passthrough_stream"; }
    std::string Description() override { return "Pass-through stream operator"; }
    OperatorPosition Position() override { return OperatorPosition::DATA; }

    // IOperator compatibility path（非流式路径不使用）
    int Work(IChannel* in, IChannel* out) override;

    // shared Configure
    int Configure(const char* key, const char* value) override;

    // IStreamOperator
    int Init(const char* with_params_json, const StreamSinkContext& sink_ctx) override;
    int OnSchemaReady(std::shared_ptr<arrow::Schema> schema) override;
    int Process(const arrow::RecordBatch& batch, int64_t ts_ms) override;
    int Tick(int64_t current_ms) override;
    int Flush() override;
    std::string GetStats() override;
    std::string LastError() override { return last_error_; }

    ParallelStrategy GetParallelStrategy() const override {
        return ParallelStrategy::STATELESS;
    }
    int GetParallelism() const override { return parallelism_; }

 private:
    std::shared_ptr<IStreamChannel> output_;
    std::atomic<int64_t> processed_rows_{0};
    std::atomic<int64_t> output_rows_{0};
    int parallelism_ = 1;
    std::string with_params_json_;
    std::string last_error_;
};

}  // namespace flowsql

#endif  // _FLOWSQL_FRAMEWORK_BUILTIN_STREAM_PASSTHROUGH_STREAM_OPERATOR_H_
