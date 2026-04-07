#include "framework/core/error_contract.h"

namespace flowsql {

const char* ToErrorCode(ErrorCodeId id) {
    switch (id) {
        case ErrorCodeId::kBatchSqlTextInvalid:
            return "BATCH_SQL_TEXT_INVALID";
        case ErrorCodeId::kSqlTextInvalid:
            return "SQL_TEXT_INVALID";
        case ErrorCodeId::kSqlAnalyzeClassifyFailed:
            return "SQL_ANALYZE_CLASSIFY_FAILED";
        case ErrorCodeId::kStreamSqlUseStreamApi:
            return "STREAM_SQL_USE_STREAM_API";
        case ErrorCodeId::kBatchSqlUseBatchApi:
            return "BATCH_SQL_USE_BATCH_API";
        case ErrorCodeId::kOpExecFail:
            return "OP_EXEC_FAIL";
        case ErrorCodeId::kStreamGroupModeInvalid:
            return "STREAM_GROUP_MODE_INVALID";
        case ErrorCodeId::kStreamGroupSqlTextInvalid:
            return "STREAM_GROUP_SQL_TEXT_INVALID";
        case ErrorCodeId::kStreamGroupDagTooLarge:
            return "STREAM_GROUP_DAG_TOO_LARGE";
        case ErrorCodeId::kStreamGroupDagInvalid:
            return "STREAM_GROUP_DAG_INVALID";
        case ErrorCodeId::kStreamGroupMixedTaskKind:
            return "STREAM_GROUP_MIXED_TASK_KIND";
        case ErrorCodeId::kStreamGroupNodeNotFound:
            return "STREAM_GROUP_NODE_NOT_FOUND";
        case ErrorCodeId::kStreamGroupDagCycleDetected:
            return "STREAM_GROUP_DAG_CYCLE_DETECTED";
        case ErrorCodeId::kStreamGroupBranchBuildFailed:
            return "STREAM_GROUP_BRANCH_BUILD_FAILED";
        case ErrorCodeId::kStreamGroupShareSetReadyTimeout:
            return "STREAM_GROUP_SHARE_SET_READY_TIMEOUT";
        case ErrorCodeId::kStreamGroupShareSetStartFailed:
            return "STREAM_GROUP_SHARE_SET_START_FAILED";
        case ErrorCodeId::kStreamGroupSinkCapabilityMismatch:
            return "STREAM_GROUP_SINK_CAPABILITY_MISMATCH";
        case ErrorCodeId::kStreamGroupSourceMismatch:
            return "STREAM_GROUP_SOURCE_MISMATCH";
        case ErrorCodeId::kStreamChannelMutating:
            return "STREAM_CHANNEL_MUTATING";
        case ErrorCodeId::kStreamSourceInUse:
            return "STREAM_SOURCE_IN_USE";
        case ErrorCodeId::kStreamChannelInUse:
            return "STREAM_CHANNEL_IN_USE";
        case ErrorCodeId::kStreamChannelVersionChanged:
            return "STREAM_CHANNEL_VERSION_CHANGED";
        case ErrorCodeId::kStreamLeaseFailed:
            return "STREAM_LEASE_FAILED";
        case ErrorCodeId::kStreamHubSelectorInvalid:
            return "STREAM_HUB_SELECTOR_INVALID";
        case ErrorCodeId::kStreamHubSelectorOutOfRange:
            return "STREAM_HUB_SELECTOR_OUT_OF_RANGE";
        case ErrorCodeId::kStreamHubSelectorNotAllowedMerge:
            return "STREAM_HUB_SELECTOR_NOT_ALLOWED_MERGE";
        case ErrorCodeId::kStreamHubSelectorNotAllowedInto:
            return "STREAM_HUB_SELECTOR_NOT_ALLOWED_INTO";
        case ErrorCodeId::kBlockStreamNotImplemented:
            return "BLOCK_STREAM_NOT_IMPLEMENTED";
        case ErrorCodeId::kStreamChannelRoleMismatch:
            return "STREAM_CHANNEL_ROLE_MISMATCH";
        case ErrorCodeId::kStreamFaninCapabilityMismatch:
            return "STREAM_FANIN_CAPABILITY_MISMATCH";
        case ErrorCodeId::kStreamSourceCapabilityMismatch:
            return "STREAM_SOURCE_CAPABILITY_MISMATCH";
        case ErrorCodeId::kStreamSinkCapabilityMismatch:
            return "STREAM_SINK_CAPABILITY_MISMATCH";
        case ErrorCodeId::kUnknown:
        default:
            return "UNKNOWN";
    }
}

const char* ToErrorStage(ErrorStageId id) {
    switch (id) {
        case ErrorStageId::kRequest:
            return "request";
        case ErrorStageId::kParse:
            return "parse";
        case ErrorStageId::kSourceResolve:
            return "source_resolve";
        case ErrorStageId::kSinkResolve:
            return "sink_resolve";
        case ErrorStageId::kLease:
            return "lease";
        case ErrorStageId::kFanin:
            return "fanin";
        case ErrorStageId::kExecute:
            return "execute";
        case ErrorStageId::kStatus:
            return "status";
        case ErrorStageId::kStop:
            return "stop";
        case ErrorStageId::kModify:
            return "modify";
        case ErrorStageId::kRemove:
            return "remove";
        case ErrorStageId::kDagValidate:
            return "dag_validate";
        case ErrorStageId::kBranchBuild:
            return "branch_build";
        case ErrorStageId::kCapabilityCheck:
            return "capability_check";
        case ErrorStageId::kShareSetValidate:
            return "share_set_validate";
        case ErrorStageId::kUnknown:
        default:
            return "unknown";
    }
}

}  // namespace flowsql
