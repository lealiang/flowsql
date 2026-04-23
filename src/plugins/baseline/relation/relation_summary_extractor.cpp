/*
 * Copyright (C) 2026 LIHUO
 *
 * Licensed under the MIT License. See LICENSE file in the project root
 * for full license information.
 *
 */

#include "plugins/baseline/relation/relation_summary_extractor.h"

#include <common/error_code.h>

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace flowsql {
namespace baseline {

namespace {

void InsertTopMass(double mass, std::size_t limit, std::vector<double>* top_masses) {
    if (!top_masses || limit == 0) return;
    if (top_masses->size() < limit) {
        top_masses->push_back(mass);
        return;
    }

    auto min_it = std::min_element(top_masses->begin(), top_masses->end());
    if (min_it != top_masses->end() && mass > *min_it) {
        *min_it = mass;
    }
}

}  // namespace

int RelationSummaryExtractor::ExtractMetricSummary(
    const RelationObservationBlock& block,
    uint32_t metric_index,
    const RelationServiceBasis& basis,
    RelationMetricSummary* out_summary) {
    if (!out_summary) return error::BAD_REQUEST;
    *out_summary = RelationMetricSummary{};

    if (!block.metrics || metric_index >= block.metric_count) {
        return error::BAD_REQUEST;
    }
    if (block.nnz > 0 && (!block.group_idx || !block.metrics[metric_index].values)) {
        return error::BAD_REQUEST;
    }

    const RelationMetricBlock& metric = block.metrics[metric_index];
    out_summary->total = metric.total;
    if (metric.total <= 0.0) return error::OK;

    std::unordered_set<uint32_t> support_set(
        basis.support_explicit.begin(), basis.support_explicit.end());
    std::unordered_map<uint32_t, std::size_t> stable_index_by_group;
    stable_index_by_group.reserve(basis.stable_head.size());
    out_summary->stable_g_shares.assign(basis.stable_head.size(), 0.0);
    for (std::size_t i = 0; i < basis.stable_head.size(); ++i) {
        stable_index_by_group.emplace(basis.stable_head[i], i);
    }

    double support_mass = 0.0;
    double top1_mass = 0.0;
    std::vector<double> top_masses;
    top_masses.reserve(static_cast<std::size_t>(std::max(0, basis.k_head)));

    // 这里同时构建两类摘要特征：
    // 1. 与 group 身份解耦的分布摘要，例如 entropy / top1 / headk；
    // 2. 绑定冻结 stable_head 的可比摘要，例如 stable_g_shares / mix_drift。
    // 两者组合后，既能检测整体集中度变化，也能检测固定头部内部比例漂移。
    for (uint32_t i = 0; i < block.nnz; ++i) {
        const double mass = metric.values[i];
        if (mass <= 0.0) continue;

        const double share = mass / metric.total;
        out_summary->entropy_shannon -= share * std::log(share);
        if (mass > top1_mass) top1_mass = mass;
        InsertTopMass(mass, static_cast<std::size_t>(std::max(0, basis.k_head)), &top_masses);

        const uint32_t group_idx = block.group_idx[i];
        if (support_set.find(group_idx) != support_set.end()) {
            support_mass += mass;
        }

        auto stable_it = stable_index_by_group.find(group_idx);
        if (stable_it != stable_index_by_group.end()) {
            out_summary->stable_g_shares[stable_it->second] = share;
        }
    }

    out_summary->valid = true;
    out_summary->top1_share = top1_mass / metric.total;
    double head_mass = 0.0;
    for (double mass : top_masses) head_mass += mass;
    out_summary->headk_share = head_mass / metric.total;
    out_summary->out_of_support_share =
        std::max(0.0, metric.total - support_mass) / metric.total;

    for (double share : out_summary->stable_g_shares) {
        out_summary->stable_headk_coverage += share;
    }

    // `RelationMetricBlock` 目前没有显式 presence bit。这里把
    // active_count == 0 且 total > 0 视为“未提供”，避免把缺失误判成 0 个活跃 group。
    if (metric.active_count > 0 || metric.total == 0.0) {
        out_summary->has_distinct_group_count = true;
        out_summary->distinct_group_count = static_cast<double>(metric.active_count);
    }

    if (basis.stable_head.size() >= 2 &&
        basis.head_proto_q.size() == basis.stable_head.size() &&
        out_summary->stable_headk_coverage > 0.0) {
        out_summary->has_stable_headk_mix_drift = true;
        // 先把当前 stable_head 里的份额重新归一化到 coverage=1，
        // 再与历史 `head_proto_q` 做 total variation distance。
        // 这样度量的是“头部内部配比是否变了”，而不是头部总覆盖率本身。
        const double coverage = out_summary->stable_headk_coverage;
        double total_variation = 0.0;
        for (std::size_t i = 0; i < basis.stable_head.size(); ++i) {
            const double current_mix = out_summary->stable_g_shares[i] / coverage;
            total_variation += std::fabs(current_mix - basis.head_proto_q[i]);
        }
        out_summary->stable_headk_mix_drift = 0.5 * total_variation;
    }

    return error::OK;
}

}  // namespace baseline
}  // namespace flowsql
