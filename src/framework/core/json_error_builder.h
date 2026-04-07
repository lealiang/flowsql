/*
 * Copyright (C) 2026 LIHUO
 *
 * Licensed under the MIT License. See LICENSE file in the project root
 * for full license information.
 *
 */

#ifndef _FLOWSQL_FRAMEWORK_CORE_JSON_ERROR_BUILDER_H_
#define _FLOWSQL_FRAMEWORK_CORE_JSON_ERROR_BUILDER_H_

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

#include <framework/core/error_contract.h>
#include <framework/interfaces/istream_channel.h>

namespace flowsql {

std::string BuildErrorJson(const std::string& error);
std::string BuildErrorWithCodeJson(const std::string& error, const std::string& error_code);
std::string BuildErrorWithCodeJson(const std::string& error, ErrorCodeId error_code_id);
std::string BuildErrorWithCodeAndSqlIndexJson(const std::string& error,
                                              const std::string& error_code,
                                              std::size_t sql_index);
std::string BuildErrorWithCodeAndSqlIndexJson(const std::string& error,
                                              ErrorCodeId error_code_id,
                                              std::size_t sql_index);
std::string BuildExecutionErrorJson(const std::string& error,
                                    const std::string& error_code,
                                    const std::string& error_stage);
std::string BuildExecutionErrorJson(const std::string& error,
                                    ErrorCodeId error_code_id,
                                    const std::string& error_stage);
std::string BuildExecutionErrorJson(const std::string& error,
                                    ErrorCodeId error_code_id,
                                    ErrorStageId error_stage_id);
std::string BuildExecutionErrorWithSqlIndexJson(const std::string& error,
                                                const std::string& error_code,
                                                const std::string& error_stage,
                                                std::size_t sql_index);
std::string BuildExecutionErrorWithSqlIndexJson(const std::string& error,
                                                ErrorCodeId error_code_id,
                                                const std::string& error_stage,
                                                std::size_t sql_index);
std::string BuildExecutionErrorWithSqlIndexJson(const std::string& error,
                                                ErrorCodeId error_code_id,
                                                ErrorStageId error_stage_id,
                                                std::size_t sql_index);
const char* StreamProducerModeName(ProducerMode mode);
const char* StreamConsumerModeName(ConsumerMode mode);
std::string BuildCapabilityMismatchJson(const std::string& error,
                                        const std::string& error_code,
                                        const StreamChannelCapabilities* source_caps,
                                        const StreamChannelCapabilities* sink_caps);
std::string BuildCapabilityMismatchJson(const std::string& error,
                                        ErrorCodeId error_code_id,
                                        const StreamChannelCapabilities* source_caps,
                                        const StreamChannelCapabilities* sink_caps);
std::string BuildSinkCapabilityMismatchJson(const std::string& error,
                                            const std::string& error_stage,
                                            const std::string& sink_key,
                                            uint32_t required_writers,
                                            ProducerMode actual_put_mode,
                                            uint32_t actual_max_producers);
std::string BuildSinkCapabilityMismatchJson(const std::string& error,
                                            ErrorStageId error_stage_id,
                                            const std::string& sink_key,
                                            uint32_t required_writers,
                                            ProducerMode actual_put_mode,
                                            uint32_t actual_max_producers);
std::string BuildSourceMismatchErrorJson(const std::string& error,
                                         const std::string& error_stage,
                                         const std::string& share_set_id,
                                         const std::string& node_id,
                                         const std::vector<std::string>& expected_keys,
                                         const std::vector<std::string>& actual_keys);
std::string BuildSourceMismatchErrorJson(const std::string& error,
                                         ErrorStageId error_stage_id,
                                         const std::string& share_set_id,
                                         const std::string& node_id,
                                         const std::vector<std::string>& expected_keys,
                                         const std::vector<std::string>& actual_keys);

}  // namespace flowsql

#endif  // _FLOWSQL_FRAMEWORK_CORE_JSON_ERROR_BUILDER_H_
