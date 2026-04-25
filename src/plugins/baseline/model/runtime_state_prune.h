/*
 * Copyright (C) 2026 LIHUO
 *
 * Licensed under the MIT License. See LICENSE file in the project root
 * for full license information.
 *
 */

#ifndef _FLOWSQL_PLUGINS_BASELINE_MODEL_RUNTIME_STATE_PRUNE_H_
#define _FLOWSQL_PLUGINS_BASELINE_MODEL_RUNTIME_STATE_PRUNE_H_

#include <cstddef>
#include <cstdint>

namespace flowsql {
namespace baseline {

constexpr int64_t kRuntimeIdlePruneBucketGap = 4096;
constexpr size_t kRuntimeIdlePruneScanLimit = 32;

inline bool RuntimeStateIdleBeyondGap(int64_t last_bucket_id, int64_t current_bucket_id) {
    return last_bucket_id > 0 && current_bucket_id > last_bucket_id &&
           (current_bucket_id - last_bucket_id) > kRuntimeIdlePruneBucketGap;
}

// 热路径只做有界 opportunistic prune：每次最多扫描固定数量条目，避免把提交路径拖成全量清理。
template <typename Map, typename Predicate>
uint64_t PruneBoundedStateMap(Map* states,
                              size_t* cursor,
                              size_t scan_limit,
                              Predicate&& should_prune) {
    if (!states || !cursor || states->empty() || scan_limit == 0) {
        if (cursor) *cursor = 0;
        return 0;
    }

    size_t offset = *cursor % states->size();
    auto it = states->begin();
    for (size_t skipped = 0; skipped < offset && it != states->end(); ++skipped) {
        ++it;
    }

    size_t scanned = 0;
    uint64_t pruned = 0;
    while (scanned < scan_limit && !states->empty()) {
        if (it == states->end()) it = states->begin();
        auto current = it++;
        ++scanned;
        if (!should_prune(current->second)) continue;
        states->erase(current);
        ++pruned;
    }

    *cursor = states->empty() ? 0 : ((offset + scanned) % states->size());
    return pruned;
}

}  // namespace baseline
}  // namespace flowsql

#endif  // _FLOWSQL_PLUGINS_BASELINE_MODEL_RUNTIME_STATE_PRUNE_H_
