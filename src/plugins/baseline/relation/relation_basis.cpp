/*
 * Copyright (C) 2026 LIHUO
 *
 * Licensed under the MIT License. See LICENSE file in the project root
 * for full license information.
 *
 */

#include "plugins/baseline/relation/relation_basis.h"

#include <common/error_code.h>

#include <algorithm>
#include <numeric>
#include <unordered_set>
#include <utility>
#include <vector>

namespace flowsql {
namespace baseline {

namespace {

struct CandidateGroup {
    uint32_t group_idx = 0;
    double hist_share = 0.0;
    double active_ratio = 0.0;
};

bool CandidateOrder(const CandidateGroup& lhs, const CandidateGroup& rhs) {
    if (lhs.hist_share != rhs.hist_share) return lhs.hist_share > rhs.hist_share;
    if (lhs.active_ratio != rhs.active_ratio) return lhs.active_ratio > rhs.active_ratio;
    return lhs.group_idx < rhs.group_idx;
}

}  // namespace

const char* RelationLineageCompatibilityName(RelationLineageCompatibility value) {
    switch (value) {
        case RelationLineageCompatibility::kIdentical:
            return "identical";
        case RelationLineageCompatibility::kCompatible:
            return "compatible";
        case RelationLineageCompatibility::kNewLineage:
            return "new_lineage";
    }
    return "compatible";
}

int RelationBasisBuilder::BuildServiceBasis(const RelationBasisBuildInput& input,
                                            RelationServiceBasis* out_basis) {
    if (!out_basis) return error::BAD_REQUEST;
    *out_basis = RelationServiceBasis{};

    if (input.feature_base.empty() || input.metric_name.empty() ||
        input.group_space_id.empty()) {
        return error::BAD_REQUEST;
    }
    if (input.valid_bucket_count == 0) return error::BAD_REQUEST;

    std::vector<uint32_t> other_group_idxs = input.other_group_idxs;
    std::sort(other_group_idxs.begin(), other_group_idxs.end());
    other_group_idxs.erase(std::unique(other_group_idxs.begin(), other_group_idxs.end()),
                           other_group_idxs.end());
    const std::unordered_set<uint32_t> other_group_set(other_group_idxs.begin(),
                                                       other_group_idxs.end());

    double total_hist_mass = 0.0;
    for (const auto& stat : input.group_stats) {
        if (stat.hist_mass > 0.0) total_hist_mass += stat.hist_mass;
    }
    if (total_hist_mass <= 0.0) return error::BAD_REQUEST;

    // `support_explicit` 表示“历史上既有足够质量占比，又有足够活跃期覆盖”的 group。
    // 先按这两个门槛做硬筛，再按历史份额排序截断到 k_support，
    // 这样后续 summary 提取才能在有界空间里稳定运行。
    std::vector<CandidateGroup> support_candidates;
    support_candidates.reserve(input.group_stats.size());
    for (const auto& stat : input.group_stats) {
        if (stat.hist_mass <= 0.0) continue;
        if (other_group_set.find(stat.group_idx) != other_group_set.end()) continue;

        CandidateGroup candidate;
        candidate.group_idx = stat.group_idx;
        candidate.hist_share = stat.hist_mass / total_hist_mass;
        candidate.active_ratio =
            static_cast<double>(stat.active_bucket_count) /
            static_cast<double>(input.valid_bucket_count);

        if (candidate.hist_share >= input.support_policy.min_hist_share &&
            candidate.active_ratio >= input.support_policy.min_active_ratio) {
            support_candidates.push_back(std::move(candidate));
        }
    }

    std::sort(support_candidates.begin(), support_candidates.end(), CandidateOrder);
    if (static_cast<int32_t>(support_candidates.size()) > input.support_policy.k_support) {
        support_candidates.resize(static_cast<size_t>(input.support_policy.k_support));
    }

    RelationServiceBasis basis;
    basis.basis_version = input.basis_version;
    basis.feature_base = input.feature_base;
    basis.metric_name = input.metric_name;
    basis.group_space_id = input.group_space_id;
    basis.group_space_version = input.group_space_version;
    basis.k_head = input.summary_policy.k_head;
    basis.other_group_idxs = std::move(other_group_idxs);
    basis.support_explicit.reserve(support_candidates.size());
    for (const auto& candidate : support_candidates) {
        basis.support_explicit.push_back(candidate.group_idx);
    }

    // `stable_head` 是 support 的更小冻结子集，用来跟踪“固定头部子分布”的内部比例变化。
    // `head_proto_q` 则是 stable_head 内部重新归一化后的历史原型分布，
    // 供后续 `stable_headk_mix_drift` 与当前混合比例做可比对的偏离量计算。
    std::vector<CandidateGroup> stable_candidates = support_candidates;
    if (static_cast<int32_t>(stable_candidates.size()) > input.summary_policy.k_stable) {
        stable_candidates.resize(static_cast<size_t>(input.summary_policy.k_stable));
    }

    basis.stable_head.reserve(stable_candidates.size());
    double stable_hist_share_sum = 0.0;
    for (const auto& candidate : stable_candidates) {
        basis.stable_head.push_back(candidate.group_idx);
        stable_hist_share_sum += candidate.hist_share;
    }

    basis.head_proto_q.reserve(stable_candidates.size());
    for (const auto& candidate : stable_candidates) {
        const double normalized_share =
            stable_hist_share_sum > 0.0 ? candidate.hist_share / stable_hist_share_sum : 0.0;
        basis.head_proto_q.push_back(normalized_share);
    }

    *out_basis = std::move(basis);
    return error::OK;
}

RelationLineageCompatibility RelationBasisBuilder::DetermineCompatibility(
    const RelationServiceBasis* incumbent_basis,
    const RelationTaskSpec& task_spec) {
    if (!incumbent_basis) return RelationLineageCompatibility::kCompatible;
    if (incumbent_basis->feature_base != task_spec.feature_base) {
        return RelationLineageCompatibility::kNewLineage;
    }
    if (incumbent_basis->group_space_id != task_spec.group_space_id) {
        return RelationLineageCompatibility::kNewLineage;
    }
    std::vector<uint32_t> task_other_group_idxs = task_spec.other_group_idxs;
    std::sort(task_other_group_idxs.begin(), task_other_group_idxs.end());
    task_other_group_idxs.erase(
        std::unique(task_other_group_idxs.begin(), task_other_group_idxs.end()),
        task_other_group_idxs.end());
    if (incumbent_basis->other_group_idxs != task_other_group_idxs) {
        return RelationLineageCompatibility::kNewLineage;
    }

    const std::string task_group_space_version =
        task_spec.group_space_version.value_or("");

    if (incumbent_basis->group_space_version == task_group_space_version) {
        return RelationLineageCompatibility::kIdentical;
    }

    if (incumbent_basis->group_space_version.empty() || !task_spec.group_space_version.has_value()) {
        return RelationLineageCompatibility::kCompatible;
    }

    return RelationLineageCompatibility::kNewLineage;
}

int RelationBasisBuilder::BuildEvalBasis(const RelationServiceBasis* incumbent_basis,
                                         const RelationTaskSpec& task_spec,
                                         RelationEvalBasis* out_eval_basis) {
    if (!out_eval_basis) return error::BAD_REQUEST;
    *out_eval_basis = RelationEvalBasis{};
    // `EvalBasis` 的职责不是服务，而是保证 candidate / incumbent 在同一摘要语义下比较。
    // 只要旧 lineage 仍然兼容，就沿用 incumbent 的 basis 作为共同投影空间；
    // 一旦判定为新 lineage，就停止直接比较，让新任务按冷启动处理。
    out_eval_basis->has_incumbent = (incumbent_basis != nullptr);
    out_eval_basis->compatibility = DetermineCompatibility(incumbent_basis, task_spec);
    if (incumbent_basis &&
        out_eval_basis->compatibility != RelationLineageCompatibility::kNewLineage) {
        out_eval_basis->basis = *incumbent_basis;
    }
    return error::OK;
}

}  // namespace baseline
}  // namespace flowsql
