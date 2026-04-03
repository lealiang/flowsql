#ifndef _FLOWSQL_FRAMEWORK_BUILTIN_STREAM_TCP_SERVICE_MERGE_STREAM_OPERATOR_H_
#define _FLOWSQL_FRAMEWORK_BUILTIN_STREAM_TCP_SERVICE_MERGE_STREAM_OPERATOR_H_

#include <framework/interfaces/ioperator.h>
#include <framework/interfaces/istream_channel.h>
#include <framework/interfaces/istream_operator.h>

#include <atomic>
#include <cstdint>
#include <memory>
#include <string>
#include <unordered_map>

namespace flowsql {

class TcpServiceMergeStreamOperator : public IOperator, public IStreamOperator {
 public:
    TcpServiceMergeStreamOperator() = default;
    ~TcpServiceMergeStreamOperator() override = default;

    std::string Category() override { return "builtin"; }
    std::string Name() override { return "tcp_service_merge_stream"; }
    std::string Description() override {
        return "Merge TCP session records into service access records by clientIP";
    }
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
    struct ServiceKey {
        std::string client_ip;
        std::string server_ip;
        int32_t server_port = 0;

        bool operator==(const ServiceKey& rhs) const {
            return client_ip == rhs.client_ip &&
                   server_ip == rhs.server_ip &&
                   server_port == rhs.server_port;
        }
    };

    struct ServiceKeyHash {
        size_t operator()(const ServiceKey& key) const {
            size_t h1 = std::hash<std::string>{}(key.client_ip);
            size_t h2 = std::hash<std::string>{}(key.server_ip);
            size_t h3 = std::hash<int32_t>{}(key.server_port);
            return h1 ^ (h2 << 1) ^ (h3 << 2);
        }
    };

    struct ServiceValue {
        int64_t bps = 0;
        int64_t pps = 0;
    };

    int BuildOutputBatch(std::shared_ptr<arrow::RecordBatch>* out);

    std::shared_ptr<IStreamChannel> output_;
    std::shared_ptr<arrow::Schema> output_schema_;
    std::unordered_map<ServiceKey, ServiceValue, ServiceKeyHash> agg_;

    int client_ip_idx_ = -1;
    int server_ip_idx_ = -1;
    int server_port_idx_ = -1;
    int bps_idx_ = -1;
    int pps_idx_ = -1;

    bool schema_ready_ = false;
    int64_t last_ts_ms_ = 0;
    std::string with_params_json_;
    std::string last_error_;
    std::atomic<int64_t> processed_rows_{0};
    std::atomic<int64_t> output_rows_{0};
};

}  // namespace flowsql

#endif  // _FLOWSQL_FRAMEWORK_BUILTIN_STREAM_TCP_SERVICE_MERGE_STREAM_OPERATOR_H_
