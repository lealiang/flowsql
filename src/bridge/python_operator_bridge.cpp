#include "python_operator_bridge.h"

#include <cstdio>

#include <rapidjson/document.h>

#include "arrow_ipc_serializer.h"
#include "framework/core/dataframe.h"
#include "framework/interfaces/idataframe_channel.h"

namespace flowsql {
namespace bridge {

PythonOperatorBridge::PythonOperatorBridge(const OperatorMeta& meta, const std::string& host, int port)
    : meta_(meta), host_(host), port_(port) {
    client_ = std::make_unique<httplib::Client>(host, port);
    client_->set_connection_timeout(5);
    client_->set_read_timeout(30);
    client_->set_write_timeout(10);
    client_->set_keep_alive(true);
}

int PythonOperatorBridge::Work(IChannel* in, IChannel* out) {
    last_error_.clear();

    // 1. dynamic_cast 到 IDataFrameChannel
    auto* df_in = dynamic_cast<IDataFrameChannel*>(in);
    auto* df_out = dynamic_cast<IDataFrameChannel*>(out);
    if (!df_in || !df_out) {
        last_error_ = "channel type mismatch (expected IDataFrameChannel)";
        printf("PythonOperatorBridge[%s.%s]: %s\n",
               meta_.catelog.c_str(), meta_.name.c_str(), last_error_.c_str());
        return -1;
    }

    // 2. 从输入通道读取 DataFrame
    DataFrame in_frame;
    if (df_in->Read(&in_frame) != 0) {
        last_error_ = "Read from input channel failed";
        printf("PythonOperatorBridge[%s.%s]: %s\n",
               meta_.catelog.c_str(), meta_.name.c_str(), last_error_.c_str());
        return -1;
    }

    // 3. DataFrame → Arrow RecordBatch → IPC 序列化
    auto batch = in_frame.ToArrow();
    if (!batch) {
        last_error_ = "ToArrow() returned null";
        printf("PythonOperatorBridge[%s.%s]: %s\n",
               meta_.catelog.c_str(), meta_.name.c_str(), last_error_.c_str());
        return -1;
    }

    std::string ipc_data;
    if (ArrowIpcSerializer::Serialize(batch, &ipc_data) != 0) {
        last_error_ = "Arrow IPC serialize failed";
        printf("PythonOperatorBridge[%s.%s]: %s\n",
               meta_.catelog.c_str(), meta_.name.c_str(), last_error_.c_str());
        return -1;
    }

    // 4. HTTP POST 到 Python Worker
    std::string path = "/work/" + meta_.catelog + "/" + meta_.name;
    auto res = client_->Post(path, ipc_data, "application/vnd.apache.arrow.stream");

    if (!res) {
        last_error_ = "HTTP POST to Python Worker failed (connection error)";
        printf("PythonOperatorBridge[%s.%s]: %s\n",
               meta_.catelog.c_str(), meta_.name.c_str(), last_error_.c_str());
        return -1;
    }

    if (res->status != 200) {
        // 尝试从 Worker 响应中提取错误详情
        last_error_ = "Python Worker returned HTTP " + std::to_string(res->status);
        if (!res->body.empty()) {
            // FastAPI 错误格式: {"detail": "..."}
            rapidjson::Document doc;
            doc.Parse(res->body.c_str());
            if (!doc.HasParseError() && doc.IsObject() && doc.HasMember("detail") && doc["detail"].IsString()) {
                last_error_ += ": " + std::string(doc["detail"].GetString());
            } else {
                last_error_ += ": " + res->body;
            }
        }
        printf("PythonOperatorBridge[%s.%s]: %s\n",
               meta_.catelog.c_str(), meta_.name.c_str(), last_error_.c_str());
        return -1;
    }

    // 5. 反序列化响应 → DataFrame → 写入输出通道
    std::shared_ptr<arrow::RecordBatch> result_batch;
    if (ArrowIpcSerializer::Deserialize(res->body, &result_batch) != 0) {
        last_error_ = "Deserialize Arrow IPC response failed";
        printf("PythonOperatorBridge[%s.%s]: %s\n",
               meta_.catelog.c_str(), meta_.name.c_str(), last_error_.c_str());
        return -1;
    }

    DataFrame out_frame;
    out_frame.FromArrow(result_batch);

    if (df_out->Write(&out_frame) != 0) {
        last_error_ = "Write to output channel failed";
        printf("PythonOperatorBridge[%s.%s]: %s\n",
               meta_.catelog.c_str(), meta_.name.c_str(), last_error_.c_str());
        return -1;
    }

    return 0;
}

int PythonOperatorBridge::Configure(const char* key, const char* value) {
    if (!key || !value) return -1;

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