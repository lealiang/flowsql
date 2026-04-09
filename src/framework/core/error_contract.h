/*
 * Copyright (C) 2026 LIHUO
 *
 * Licensed under the MIT License. See LICENSE file in the project root
 * for full license information.
 *
 */

#ifndef _FLOWSQL_FRAMEWORK_CORE_ERROR_CONTRACT_H_
#define _FLOWSQL_FRAMEWORK_CORE_ERROR_CONTRACT_H_

namespace flowsql {

enum class ErrorCodeId {
    kUnknown = 0,
    kBatchSqlTextInvalid,
    kSqlTextInvalid,
    kSqlAnalyzeClassifyFailed,
    kStreamSqlUseStreamApi,
    kBatchSqlUseBatchApi,
    kOpExecFail,
    kStreamGroupModeInvalid,
    kStreamGroupSqlTextInvalid,
    kStreamGroupDagTooLarge,
    kStreamGroupDagInvalid,
    kStreamGroupMixedTaskKind,
    kStreamGroupNodeNotFound,
    kStreamGroupDagCycleDetected,
    kStreamGroupBranchBuildFailed,
    kStreamGroupShareSetReadyTimeout,
    kStreamGroupShareSetStartFailed,
    kStreamGroupSinkCapabilityMismatch,
    kStreamGroupSourceMismatch,
    kStreamChannelMutating,
    kStreamSourceInUse,
    kStreamChannelInUse,
    kStreamChannelVersionChanged,
    kStreamLeaseFailed,
    kStreamHubSelectorInvalid,
    kStreamHubSelectorOutOfRange,
    kStreamHubSelectorNotAllowedMerge,
    kStreamHubSelectorNotAllowedInto,
    kBlockStreamNotImplemented,
    kStreamChannelRoleMismatch,
    kStreamFaninCapabilityMismatch,
    kStreamSourceCapabilityMismatch,
    kStreamSinkCapabilityMismatch,
    kSharedSourceHubCreateFailed,
    kSharedSourceSubscribeFailed,
    kSharedSourceModeMismatch,
    kSharedSourceWhereMismatch,
    kSharedSourceFilterUnsupported,
    kSharedSourceReadyTimeout,
    kSharedSourceInternalError,
};

enum class ErrorStageId {
    kUnknown = 0,
    kRequest,
    kParse,
    kSourceResolve,
    kSinkResolve,
    kLease,
    kFanin,
    kExecute,
    kStatus,
    kStop,
    kModify,
    kRemove,
    kDagValidate,
    kBranchBuild,
    kCapabilityCheck,
    kShareSetValidate,
};

const char* ToErrorCode(ErrorCodeId id);
const char* ToErrorStage(ErrorStageId id);

}  // namespace flowsql

#endif  // _FLOWSQL_FRAMEWORK_CORE_ERROR_CONTRACT_H_
