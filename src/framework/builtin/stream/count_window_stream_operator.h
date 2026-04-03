#ifndef _FLOWSQL_FRAMEWORK_BUILTIN_STREAM_COUNT_WINDOW_STREAM_OPERATOR_H_
#define _FLOWSQL_FRAMEWORK_BUILTIN_STREAM_COUNT_WINDOW_STREAM_OPERATOR_H_

#include <framework/interfaces/ioperator.h>
#include <framework/interfaces/istream_channel.h>
#include <framework/interfaces/istream_operator.h>

#include <atomic>
#include <memory>
#include <string>

namespace flowsql {

class CountWindowStreamOperator : public IOperator, public IStreamOperator {
 public:
    CountWindowStreamOperator() = default;
    ~CountWindowStreamOperator() override = default;

    std::string Category() override { return "builtin"; }
    std::string Name() override { return "count_window_stream"; }
    std::string Description() override { return "Windowed stream row counter"; }
    OperatorPosition Position() override { return OperatorPosition::DATA; }

    int Work(IChannel* in, IChannel* out) override;
    int Configure(const char* key, const char* value) override;

    int Init(const char* with_params_json, const StreamSinkContext& sink_ctx) override;
    int OnSchemaReady(std::shared_ptr<arrow::Schema> schema) override;
    int Process(const arrow::RecordBatch& batch, int64_t ts_ms) override;
    int Tick(int64_t current_ms) override;
    int Flush() override;
    std::string GetStats() override;
    std::string LastError() override { return last_error_; }

    ParallelStrategy GetParallelStrategy() const override {
        return ParallelStrategy::NONE;
    }

 private:
    int EmitSummary(int64_t ts_ms);

    std::shared_ptr<IStreamChannel> output_;
    std::shared_ptr<arrow::Schema> summary_schema_;

    std::atomic<int64_t> total_rows_{0};
    std::atomic<int64_t> pending_rows_{0};
    std::atomic<int64_t> output_rows_{0};

    int64_t window_rows_ = 1000;
    int64_t flush_interval_ms_ = 1000;
    int64_t last_emit_ms_ = 0;
    std::string with_params_json_;
    std::string last_error_;
};

}  // namespace flowsql

#endif  // _FLOWSQL_FRAMEWORK_BUILTIN_STREAM_COUNT_WINDOW_STREAM_OPERATOR_H_
