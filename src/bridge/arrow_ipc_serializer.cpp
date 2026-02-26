#include "arrow_ipc_serializer.h"

#include <cstdio>

namespace flowsql {
namespace bridge {

int ArrowIpcSerializer::Serialize(const std::shared_ptr<arrow::RecordBatch>& batch, std::string* out) {
    if (!batch || !out) return -1;

    auto buffer_stream = arrow::io::BufferOutputStream::Create().ValueOrDie();

    auto writer_result = arrow::ipc::MakeStreamWriter(buffer_stream, batch->schema());
    if (!writer_result.ok()) {
        printf("ArrowIpcSerializer::Serialize: MakeStreamWriter failed: %s\n",
               writer_result.status().ToString().c_str());
        return -1;
    }
    auto writer = *writer_result;

    auto status = writer->WriteRecordBatch(*batch);
    if (!status.ok()) {
        printf("ArrowIpcSerializer::Serialize: WriteRecordBatch failed: %s\n", status.ToString().c_str());
        return -1;
    }

    status = writer->Close();
    if (!status.ok()) {
        printf("ArrowIpcSerializer::Serialize: Close failed: %s\n", status.ToString().c_str());
        return -1;
    }

    auto buffer_result = buffer_stream->Finish();
    if (!buffer_result.ok()) {
        printf("ArrowIpcSerializer::Serialize: Finish failed: %s\n", buffer_result.status().ToString().c_str());
        return -1;
    }
    auto buffer = *buffer_result;

    out->assign(reinterpret_cast<const char*>(buffer->data()), buffer->size());
    return 0;
}

int ArrowIpcSerializer::Deserialize(const std::string& data, std::shared_ptr<arrow::RecordBatch>* out) {
    if (data.empty() || !out) return -1;

    auto buffer = arrow::Buffer::Wrap(data.data(), data.size());
    auto buffer_reader = std::make_shared<arrow::io::BufferReader>(buffer);

    auto reader_result = arrow::ipc::RecordBatchStreamReader::Open(buffer_reader);
    if (!reader_result.ok()) {
        printf("ArrowIpcSerializer::Deserialize: Open failed: %s\n", reader_result.status().ToString().c_str());
        return -1;
    }
    auto reader = *reader_result;

    auto batch_result = reader->Next();
    if (!batch_result.ok()) {
        printf("ArrowIpcSerializer::Deserialize: Next failed: %s\n", batch_result.status().ToString().c_str());
        return -1;
    }
    *out = *batch_result;

    if (!*out) {
        printf("ArrowIpcSerializer::Deserialize: empty stream\n");
        return -1;
    }
    return 0;
}

}  // namespace bridge
}  // namespace flowsql
