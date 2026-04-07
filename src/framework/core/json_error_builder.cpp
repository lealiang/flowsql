#include "framework/core/json_error_builder.h"

#include <rapidjson/stringbuffer.h>
#include <rapidjson/writer.h>

#include <algorithm>
#include <cstdint>
#include <iterator>

namespace flowsql {

const char* StreamProducerModeName(ProducerMode mode) {
    return mode == ProducerMode::MULTI ? "MULTI" : "SINGLE";
}

const char* StreamConsumerModeName(ConsumerMode mode) {
    return mode == ConsumerMode::MULTI ? "MULTI" : "SINGLE";
}

namespace {

void WriteCapabilitiesObject(rapidjson::Writer<rapidjson::StringBuffer>* w,
                             const StreamChannelCapabilities& caps) {
    if (!w) return;
    w->StartObject();
    w->Key("channel_type");
    w->String(caps.channel_type.c_str());
    w->Key("concurrency");
    w->StartObject();
    w->Key("put_mode");
    w->String(StreamProducerModeName(caps.concurrency.put_mode));
    w->Key("poll_mode");
    w->String(StreamConsumerModeName(caps.concurrency.poll_mode));
    w->Key("max_producers");
    w->Uint(caps.concurrency.max_producers);
    w->Key("max_consumers");
    w->Uint(caps.concurrency.max_consumers);
    w->Key("lock_free_put");
    w->Bool(caps.concurrency.lock_free_put);
    w->Key("lock_free_poll");
    w->Bool(caps.concurrency.lock_free_poll);
    w->Key("cancel_wakeup_guaranteed");
    w->Bool(caps.concurrency.cancel_wakeup_guaranteed);
    w->EndObject();
    w->EndObject();
}

void ComputeSetDiff(const std::vector<std::string>& expected,
                    const std::vector<std::string>& actual,
                    std::vector<std::string>* missing,
                    std::vector<std::string>* extra) {
    std::vector<std::string> expected_sorted = expected;
    std::vector<std::string> actual_sorted = actual;
    std::sort(expected_sorted.begin(), expected_sorted.end());
    std::sort(actual_sorted.begin(), actual_sorted.end());

    if (missing) {
        missing->clear();
        std::set_difference(expected_sorted.begin(), expected_sorted.end(),
                            actual_sorted.begin(), actual_sorted.end(),
                            std::back_inserter(*missing));
    }
    if (extra) {
        extra->clear();
        std::set_difference(actual_sorted.begin(), actual_sorted.end(),
                            expected_sorted.begin(), expected_sorted.end(),
                            std::back_inserter(*extra));
    }
}

}  // namespace

std::string BuildErrorJson(const std::string& error) {
    rapidjson::StringBuffer buf;
    rapidjson::Writer<rapidjson::StringBuffer> w(buf);
    w.StartObject();
    w.Key("error");
    w.String(error.c_str());
    w.EndObject();
    return buf.GetString();
}

std::string BuildErrorWithCodeJson(const std::string& error, const std::string& error_code) {
    rapidjson::StringBuffer buf;
    rapidjson::Writer<rapidjson::StringBuffer> w(buf);
    w.StartObject();
    w.Key("error");
    w.String(error.c_str());
    w.Key("error_code");
    w.String(error_code.c_str());
    w.EndObject();
    return buf.GetString();
}

std::string BuildErrorWithCodeJson(const std::string& error, ErrorCodeId error_code_id) {
    return BuildErrorWithCodeJson(error, ToErrorCode(error_code_id));
}

std::string BuildErrorWithCodeAndSqlIndexJson(const std::string& error,
                                              const std::string& error_code,
                                              std::size_t sql_index) {
    rapidjson::StringBuffer buf;
    rapidjson::Writer<rapidjson::StringBuffer> w(buf);
    w.StartObject();
    w.Key("error");
    w.String(error.c_str());
    w.Key("error_code");
    w.String(error_code.c_str());
    w.Key("sql_index");
    w.Uint64(static_cast<uint64_t>(sql_index));
    w.EndObject();
    return buf.GetString();
}

std::string BuildErrorWithCodeAndSqlIndexJson(const std::string& error,
                                              ErrorCodeId error_code_id,
                                              std::size_t sql_index) {
    return BuildErrorWithCodeAndSqlIndexJson(error, ToErrorCode(error_code_id), sql_index);
}

std::string BuildExecutionErrorJson(const std::string& error,
                                    const std::string& error_code,
                                    const std::string& error_stage) {
    rapidjson::StringBuffer buf;
    rapidjson::Writer<rapidjson::StringBuffer> w(buf);
    w.StartObject();
    w.Key("error");
    w.String(error.c_str());
    w.Key("error_code");
    w.String(error_code.c_str());
    w.Key("error_stage");
    w.String(error_stage.c_str());
    w.EndObject();
    return buf.GetString();
}

std::string BuildExecutionErrorJson(const std::string& error,
                                    ErrorCodeId error_code_id,
                                    const std::string& error_stage) {
    return BuildExecutionErrorJson(error, ToErrorCode(error_code_id), error_stage);
}

std::string BuildExecutionErrorJson(const std::string& error,
                                    ErrorCodeId error_code_id,
                                    ErrorStageId error_stage_id) {
    return BuildExecutionErrorJson(error, ToErrorCode(error_code_id), ToErrorStage(error_stage_id));
}

std::string BuildExecutionErrorWithSqlIndexJson(const std::string& error,
                                                const std::string& error_code,
                                                const std::string& error_stage,
                                                std::size_t sql_index) {
    rapidjson::StringBuffer buf;
    rapidjson::Writer<rapidjson::StringBuffer> w(buf);
    w.StartObject();
    w.Key("error");
    w.String(error.c_str());
    w.Key("error_code");
    w.String(error_code.c_str());
    w.Key("error_stage");
    w.String(error_stage.c_str());
    w.Key("sql_index");
    w.Uint64(static_cast<uint64_t>(sql_index));
    w.EndObject();
    return buf.GetString();
}

std::string BuildExecutionErrorWithSqlIndexJson(const std::string& error,
                                                ErrorCodeId error_code_id,
                                                const std::string& error_stage,
                                                std::size_t sql_index) {
    return BuildExecutionErrorWithSqlIndexJson(error, ToErrorCode(error_code_id), error_stage, sql_index);
}

std::string BuildExecutionErrorWithSqlIndexJson(const std::string& error,
                                                ErrorCodeId error_code_id,
                                                ErrorStageId error_stage_id,
                                                std::size_t sql_index) {
    return BuildExecutionErrorWithSqlIndexJson(
        error, ToErrorCode(error_code_id), ToErrorStage(error_stage_id), sql_index);
}

std::string BuildCapabilityMismatchJson(const std::string& error,
                                        const std::string& error_code,
                                        const StreamChannelCapabilities* source_caps,
                                        const StreamChannelCapabilities* sink_caps) {
    rapidjson::StringBuffer buf;
    rapidjson::Writer<rapidjson::StringBuffer> w(buf);
    w.StartObject();
    w.Key("error");
    w.String(error.c_str());
    w.Key("error_code");
    w.String(error_code.c_str());
    w.Key("error_stage");
    w.String("capability_check");
    w.Key("details");
    w.StartObject();
    w.Key("capabilities");
    w.StartObject();
    if (source_caps) {
        w.Key("source");
        WriteCapabilitiesObject(&w, *source_caps);
    }
    if (sink_caps) {
        w.Key("sink");
        WriteCapabilitiesObject(&w, *sink_caps);
    }
    w.EndObject();
    w.EndObject();
    w.EndObject();
    return buf.GetString();
}

std::string BuildCapabilityMismatchJson(const std::string& error,
                                        ErrorCodeId error_code_id,
                                        const StreamChannelCapabilities* source_caps,
                                        const StreamChannelCapabilities* sink_caps) {
    return BuildCapabilityMismatchJson(error, ToErrorCode(error_code_id), source_caps, sink_caps);
}

std::string BuildSinkCapabilityMismatchJson(const std::string& error,
                                            const std::string& error_stage,
                                            const std::string& sink_key,
                                            uint32_t required_writers,
                                            ProducerMode actual_put_mode,
                                            uint32_t actual_max_producers) {
    rapidjson::StringBuffer buf;
    rapidjson::Writer<rapidjson::StringBuffer> w(buf);
    w.StartObject();
    w.Key("error");
    w.String(error.c_str());
    w.Key("error_code");
    w.String(ToErrorCode(ErrorCodeId::kStreamGroupSinkCapabilityMismatch));
    w.Key("error_stage");
    w.String(error_stage.c_str());
    w.Key("sink_key");
    w.String(sink_key.c_str());
    w.Key("required");
    w.StartObject();
    w.Key("writers");
    w.Uint(required_writers);
    w.Key("put_mode");
    w.String("MULTI");
    w.EndObject();
    w.Key("actual");
    w.StartObject();
    w.Key("put_mode");
    w.String(StreamProducerModeName(actual_put_mode));
    w.Key("max_producers");
    w.Uint(actual_max_producers);
    w.EndObject();
    w.EndObject();
    return buf.GetString();
}

std::string BuildSinkCapabilityMismatchJson(const std::string& error,
                                            ErrorStageId error_stage_id,
                                            const std::string& sink_key,
                                            uint32_t required_writers,
                                            ProducerMode actual_put_mode,
                                            uint32_t actual_max_producers) {
    return BuildSinkCapabilityMismatchJson(error,
                                           ToErrorStage(error_stage_id),
                                           sink_key,
                                           required_writers,
                                           actual_put_mode,
                                           actual_max_producers);
}

std::string BuildSourceMismatchErrorJson(const std::string& error,
                                         const std::string& error_stage,
                                         const std::string& share_set_id,
                                         const std::string& node_id,
                                         const std::vector<std::string>& expected_keys,
                                         const std::vector<std::string>& actual_keys) {
    std::vector<std::string> missing_keys;
    std::vector<std::string> extra_keys;
    ComputeSetDiff(expected_keys, actual_keys, &missing_keys, &extra_keys);

    rapidjson::StringBuffer buf;
    rapidjson::Writer<rapidjson::StringBuffer> w(buf);
    w.StartObject();
    w.Key("error");
    w.String(error.c_str());
    w.Key("error_code");
    w.String(ToErrorCode(ErrorCodeId::kStreamGroupSourceMismatch));
    w.Key("error_stage");
    w.String(error_stage.c_str());
    if (!share_set_id.empty()) {
        w.Key("share_set_id");
        w.String(share_set_id.c_str());
    }
    if (!node_id.empty()) {
        w.Key("node_id");
        w.String(node_id.c_str());
    }
    w.Key("missing_keys");
    w.StartArray();
    for (const auto& key : missing_keys) {
        w.String(key.c_str());
    }
    w.EndArray();
    w.Key("extra_keys");
    w.StartArray();
    for (const auto& key : extra_keys) {
        w.String(key.c_str());
    }
    w.EndArray();
    w.EndObject();
    return buf.GetString();
}

std::string BuildSourceMismatchErrorJson(const std::string& error,
                                         ErrorStageId error_stage_id,
                                         const std::string& share_set_id,
                                         const std::string& node_id,
                                         const std::vector<std::string>& expected_keys,
                                         const std::vector<std::string>& actual_keys) {
    return BuildSourceMismatchErrorJson(error,
                                        ToErrorStage(error_stage_id),
                                        share_set_id,
                                        node_id,
                                        expected_keys,
                                        actual_keys);
}

}  // namespace flowsql
