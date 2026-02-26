#include "python_operator_bridge.h"

#include <cstdio>

#include "arrow_ipc_serializer.h"

namespace flowsql {
namespace bridge {

PythonOperatorBridge::PythonOperatorBridge(const OperatorMeta& meta, const std::string& host, int port)
    : meta_(meta), host_(host), port_(port) {
    std::string addr = host + ":" + std::to_string(port);
    client_ = std::make_unique<httplib::Client>(host, port);
    client_->set_connection_timeout(5);     // 连接超时 5s
    client_->set_read_timeout(30);          // 读取超时 30s
    client_->set_write_timeout(10);         // 写入超时 10s
    client_->set_keep_alive(true);          // keep-alive 复用连接
}

int PythonOperatorBridge::Work(IDataFrame* in, IDataFrame* out) {
    if (!in || !out) return -1;

    // 1. in->ToArrow() 零拷贝获取 RecordBatch
    auto batch = in->ToArrow();
    if (!batch) {
        printf("PythonOperatorBridge[%s.%s]: ToArrow() returned null\n",
               meta_.catelog.c_str(), meta_.name.c_str());
        return -1;
    }

    // 2. 序列化为 Arrow IPC
    std::string ipc_data;
    if (ArrowIpcSerializer::Serialize(batch, &ipc_data) != 0) {
        printf("PythonOperatorBridge[%s.%s]: Serialize failed\n",
               meta_.catelog.c_str(), meta_.name.c_str());
        return -1;
    }

    // 3. HTTP POST 到 Python Worker
    std::string path = "/work/" + meta_.catelog + "/" + meta_.name;
    auto res = client_->Post(path, ipc_data, "application/vnd.apache.arrow.stream");

    if (!res) {
        printf("PythonOperatorBridge[%s.%s]: HTTP POST failed (connection error)\n",
               meta_.catelog.c_str(), meta_.name.c_str());
        return -1;
    }

    if (res->status != 200) {
        printf("PythonOperatorBridge[%s.%s]: HTTP %d: %s\n",
               meta_.catelog.c_str(), meta_.name.c_str(), res->status, res->body.c_str());
        return -1;
    }

    // 4. 反序列化响应
    std::shared_ptr<arrow::RecordBatch> result_batch;
    if (ArrowIpcSerializer::Deserialize(res->body, &result_batch) != 0) {
        printf("PythonOperatorBridge[%s.%s]: Deserialize response failed\n",
               meta_.catelog.c_str(), meta_.name.c_str());
        return -1;
    }

    // 5. 零拷贝写入 out DataFrame
    out->FromArrow(result_batch);
    return 0;
}

int PythonOperatorBridge::Configure(const char* key, const char* value) {
    if (!key || !value) return -1;

    // 转发配置到 Python Worker
    std::string path = "/configure/" + meta_.catelog + "/" + meta_.name;
    std::string body = std::string("{\"key\":\"") + key + "\",\"value\":\"" + value + "\"}";
    auto res = client_->Post(path, body, "application/json");

    if (!res || res->status != 200) {
        printf("PythonOperatorBridge[%s.%s]: Configure failed\n",
               meta_.catelog.c_str(), meta_.name.c_str());
        return -1;
    }
    return 0;
}

}  // namespace bridge
}  // namespace flowsql
