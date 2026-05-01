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
#include <unordered_set>
#include <utility>
#include <vector>

namespace flowsql {
namespace baseline {

namespace {

struct RankedGroup {
    uint32_t group_idx = 0;
    double hist_share = 0.0;
    double active_ratio = 0.0;
};

bool RankedGroupOrder(const RankedGroup& lhs, const RankedGroup& rhs) {
    if (lhs.hist_share != rhs.hist_share) return lhs.hist_share > rhs.hist_share;
    if (lhs.active_ratio != rhs.active_ratio) return lhs.active_ratio > rhs.active_ratio;
    return lhs.group_idx < rhs.group_idx;
}

}  // namespace

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
    std::vector<RankedGroup> support_groups;
    support_groups.reserve(input.group_stats.size());
    for (const auto& stat : input.group_stats) {
        if (stat.hist_mass <= 0.0) continue;
        if (other_group_set.find(stat.group_idx) != other_group_set.end()) continue;

        RankedGroup group;
        group.group_idx = stat.group_idx;
        group.hist_share = stat.hist_mass / total_hist_mass;
        group.active_ratio =
            static_cast<double>(stat.active_bucket_count) /
            static_cast<double>(input.valid_bucket_count);

        if (group.hist_share >= input.support_policy.min_hist_share &&
            group.active_ratio >= input.support_policy.min_active_ratio) {
            support_groups.push_back(std::move(group));
        }
    }

    std::sort(support_groups.begin(), support_groups.end(), RankedGroupOrder);
    if (static_cast<int32_t>(support_groups.size()) > input.support_policy.k_support) {
        support_groups.resize(static_cast<size_t>(input.support_policy.k_support));
    }

    RelationServiceBasis basis;
    basis.basis_version = input.basis_version;
    basis.feature_base = input.feature_base;
    basis.metric_name = input.metric_name;
    basis.group_space_id = input.group_space_id;
    basis.group_space_version = input.group_space_version;
    basis.k_head = input.summary_policy.k_head;
    basis.other_group_idxs = std::move(other_group_idxs);
    basis.support_explicit.reserve(support_groups.size());
    for (const auto& group : support_groups) {
        basis.support_explicit.push_back(group.group_idx);
    }

    // `stable_head` 是 support 的更小冻结子集，用来跟踪“固定头部子分布”的内部比例变化。
    // `head_proto_q` 则是 stable_head 内部重新归一化后的历史原型分布，
    // 供后续 `stable_headk_mix_drift` 与当前混合比例做可比对的偏离量计算。
    std::vector<RankedGroup> stable_groups = support_groups;
    if (static_cast<int32_t>(stable_groups.size()) > input.summary_policy.k_stable) {
        stable_groups.resize(static_cast<size_t>(input.summary_policy.k_stable));
    }

    basis.stable_head.reserve(stable_groups.size());
    double stable_hist_share_sum = 0.0;
    for (const auto& group : stable_groups) {
        basis.stable_head.push_back(group.group_idx);
        stable_hist_share_sum += group.hist_share;
    }

    basis.head_proto_q.reserve(stable_groups.size());
    for (const auto& group : stable_groups) {
        const double normalized_share =
            stable_hist_share_sum > 0.0 ? group.hist_share / stable_hist_share_sum : 0.0;
        basis.head_proto_q.push_back(normalized_share);
    }

    *out_basis = std::move(basis);
    return error::OK;
}

}  // namespace baseline
}  // namespace flowsql
