# B7 Relation Fusion Runtime State Cleanup 设计

## 1. 背景与结论

B5 已实现 Relation pattern fusion，把同一个 Relation source 下多个 routed summary 的 rolling 结果融合成 `RelationFusionResult`。当前运行时状态存在两个长期运行风险：

1. `BaselineRelationTask::fusion_states_` 使用 `std::unordered_map<std::string, RelationFusionRuntimeState>` 按 source series key 常驻保存状态。只要新的 source 持续进入，map 会持续增长。
2. 单个 `RelationFusionRuntimeState` 内部的 `persistence_by_evidence_dir` 会保存 evidence persistence。basis version、active basis stable head 或 expected summary universe 变化后，旧 evidence key 可能滞留。

本文只设计 Relation fusion runtime state 的生命周期治理，不修改 B5 的风险公式、pattern 语义或 routed summary rolling 算法。

核心结论：

- source 级 fusion state 必须有数量上界、TTL 和清理指标。
- evidence persistence 必须跟随当前 expected evidence universe 收口，不能因为 basis version 变化无限累积。
- 清理放在 `SubmitObservation()` 的非 const 热路径中惰性执行；snapshot 只读状态和计数，不触发清理。
- 当前 Baseline 目标契约是同 task 非并发调用，因此本设计不引入新的 task 内部锁。

---

## 2. Source 的定义

本文中的 `source` 指 Relation task 输入侧的 `RelationRollingObservation.series_key`，也就是一个 Relation block 所属的业务对象或观测对象。

它不是 group，也不是 routed summary。

示例：

```text
source_series_key = "svc-a"
feature_base      = "client_mix"
metric            = "bps"
group_idx         = [client_asn / peer_id / region_id ...]
```

如果业务每分钟向 Relation task 提交一次 `svc-a` 的客户端构成分布和 `bps` 指标，那么：

- `svc-a` 是 source。
- `group_idx` 中的每个客户端、ASN、地域或 peer 是 group。
- `svc-a::client_mix::bps::out_of_support_share::ratio::basis:7` 这类派生序列是 routed summary series。
- `fusion_states_["svc-a"]` 是该 source 的 Relation fusion runtime state，用来保存最近一次 fusion result 和 evidence persistence。

再举一个链路流量场景：

```text
source_series_key = "link:edge-01:wan0"
feature_base      = "peer_mix"
metric            = "mbps"
group_idx         = [peer_asn_id ...]
```

这里的 source 是 `link:edge-01:wan0`，表示这条链路的 peer mix 关系结构；每个 peer ASN 是 group；fusion state 只保存这条链路内部多个 routed summary 的融合状态。

因此，`fusion_state_max_sources` 限制的是单个 Relation task 内最多保留多少个不同的 `source_series_key` 的 fusion state。

---

## 3. 目标与非目标

### 3.1 目标

- 控制 `fusion_states_` 的长期内存上界。
- 控制单 source 内 `persistence_by_evidence_dir` 的 key 上界。
- 保持 active source 不被正常清理误删。
- 清理成本有固定预算，禁止在热路径全表扫描。
- 通过 task snapshot 暴露当前状态规模和累计清理量。
- 不改变 `RelationFusionResult` 的业务语义。

### 3.2 非目标

- 不实现跨 task 的全局 source 配额。
- 不把 relation fusion state 持久化到 bootstrap artifact 或 seed。
- 不增加后台清理线程。
- 不引入 per-source mutex 或更细粒度并发模型。
- 不把 cleanup 设计成精确 LRU；首版只要求有界、低成本、可观测。

---

## 4. 配置设计

新增配置仍放在 `baseline.rolling_config.relation_rolling.relation_fusion` 下。

建议默认：

```yaml
relation_rolling:
  relation_fusion:
    enable_relation_fusion: true
    fusion_z_score_cap: 5.0
    fusion_min_evidence_score: 0.20
    fusion_persistence_window: 2
    fusion_warming_weight: 0.25
    fusion_degraded_weight: 0.25
    fusion_support_weight: 0.50
    fusion_oppose_weight: 0.50
    basic_pattern_weight: 0.70
    stable_head_pattern_weight: 0.85
    dominant_single_cap: 3
    dominant_pattern_cap: 2

    fusion_state_ttl_buckets: 20160
    fusion_state_max_sources: 4096
    fusion_state_cleanup_interval_updates: 512
    fusion_state_cleanup_scan_limit: 256
    fusion_persistence_max_keys_per_source: 512
```

### 4.1 配置落点与闭环

B7 配置必须进入统一运行时配置模板：

```text
src/plugins/baseline/config/baseline-config-template.yaml
```

字段位置固定在：

```text
baseline.rolling_config.relation_rolling.relation_fusion
```

配置处理必须同时闭合以下 5 个落点，禁止只改其中一处：

1. `rolling/rolling_config.h`：`BaselineRelationFusionConfig` 增加字段和 C++ 默认值。
2. `rolling/rolling_config.cpp`：`DefaultBaselineRollingConfig()` / `ValidateBaselineRollingConfig()` 覆盖默认和非法值校验。
3. `config/runtime_config.cpp`：`ParseRollingConfig()` 读取字段，strict schema 白名单允许字段。
4. `config/baseline-config-template.yaml`：模板写出完整默认字段，作为配置样例和运维入口。
5. `src/tests/test_baseline/test_baseline_rolling_config.cpp`：覆盖默认值、运行时 override、非法值拒绝和模板 strict schema。

原因是当前 runtime config 使用 strict schema。若只把字段写进 `baseline-config-template.yaml`，但 parser / strict schema 不认识这些 key，模板反而会成为不可加载配置；若只改 C++ 默认值而不写模板，运维侧无法发现和覆盖该参数。

模板中建议直接写展开后的默认值：

```yaml
fusion_state_ttl_buckets: 20160
```

不要在 YAML 中写表达式 `2 * week_buckets`。如果后续希望 TTL 随 `week_buckets` 自动变化，应在 C++ resolved config 中处理；模板仍保持机器可读的具体数值。

### 4.2 默认值说明

| 配置 | 默认值 | 说明 |
| --- | ---: | --- |
| `fusion_state_ttl_buckets` | `20160` | 默认 14 天。按当前默认 `bucket_seconds = 60`，等于 `2 * week_buckets`。实现时建议在 resolved config 中用 `2 * week_buckets` 生成默认值，模板中写出展开值。 |
| `fusion_state_max_sources` | `4096` | 单个 Relation task 最多保留 4096 个 source fusion state。该值比 20000 更保守，适合作为默认防护上限。 |
| `fusion_state_cleanup_interval_updates` | `512` | 每 512 次 fusion update 做一次常规清理扫描。 |
| `fusion_state_cleanup_scan_limit` | `256` | 每次 cleanup 最多检查 256 个 source state，避免热路径全表扫描。 |
| `fusion_persistence_max_keys_per_source` | `512` | 单个 source 内最多保留 512 个 evidence persistence key。当前 B5 expected evidence universe 通常远小于该值，512 主要防 basis version 或 stable head 变化造成异常累积。 |

`fusion_state_max_sources = 4096` 的内存口径：

- 单 source state 约数 KB 到十几 KB，取决于 `last_result` 中字符串和 vector 数量，以及 `persistence_by_evidence_dir` key 数量。
- 4096 个 source 的核心状态约为数十 MB 级别；加上 `unordered_map`、字符串和 allocator overhead，仍可能达到更高的几十 MB。
- 该默认值是保护上限，不是推荐常驻规模。小规格部署可以调到 1024；高基数生产场景应结合 snapshot 指标和进程内存上限评估后调大。

### 4.3 校验规则

`ValidateBaselineRollingConfig()` 应新增校验：

```text
fusion_state_ttl_buckets >= 1
fusion_state_max_sources >= 1
fusion_state_cleanup_interval_updates >= 1
fusion_state_cleanup_scan_limit >= 1
fusion_persistence_max_keys_per_source >= 1
```

推荐额外约束：

- `fusion_state_cleanup_scan_limit <= fusion_state_max_sources`，避免一次 cleanup 预算大于状态容量。
- `fusion_persistence_max_keys_per_source >= dominant_single_cap`，避免配置互相矛盾。

---

## 5. 状态模型

### 5.1 Runtime state 扩展

`RelationFusionRuntimeState` 增加生命周期字段：

```cpp
struct RelationFusionRuntimeState {
    int64_t last_bucket_id = 0;
    bool has_last_bucket = false;
    int64_t last_touched_bucket_id = 0;
    uint64_t last_touched_update_seq = 0;
    std::unordered_map<std::string, uint32_t> persistence_by_evidence_dir;
    RelationFusionResult last_result;
};
```

字段语义：

- `last_bucket_id` 仍表示 fusion 算法接受并推进的最后一个 bucket。
- `last_touched_bucket_id` 表示该 source 最近一次成功推进生命周期的 bucket，用于 TTL。
- `last_touched_update_seq` 表示 Relation task 内部递增的 fusion update 序号，用于容量淘汰时近似判断新旧。

`RelationFusionRuntimeState` 不维护独立 mutex。它只在同一个 Relation task 的串行调用上下文中读写。

### 5.2 Task 级 cleanup 状态

`BaselineRelationTask` 增加：

```cpp
uint64_t fusion_update_seq_ = 0;
uint64_t fusion_cleanup_bucket_cursor_ = 0;
uint64_t fusion_state_evicted_total_ = 0;
uint64_t fusion_state_evicted_ttl_total_ = 0;
uint64_t fusion_state_evicted_capacity_total_ = 0;
uint64_t fusion_persistence_key_evicted_total_ = 0;
uint64_t fusion_cleanup_last_scan_count_ = 0;
uint64_t fusion_cleanup_last_evicted_count_ = 0;
int64_t fusion_cleanup_watermark_bucket_id_ = 0;
```

首版不引入 LRU list、TTL heap 或额外 ordered index。原因：

- `fusion_state_max_sources` 默认 4096，固定扫描预算足够控制成本。
- LRU / TTL 双索引会复制 source key，并要求 erase / rebuild / load 时维护多套一致性，增加实现风险。
- Sprint 21 已明确同 task 非并发，最重要的是状态有界和可观测，不需要精确 LRU。

---

## 6. 清理算法

### 6.1 Evidence 级清理

在 `UpdateRelationFusion()` 内执行。

当前 evidence key 包含：

```text
source_series_key
feature_base
metric
summary
direction
basis_version（basis-scoped summary 才包含）
```

basis version 进入 key 是正确的，因为 basis 切换后不应继承旧 persistence。但如果旧 key 永远不删，会造成单 source 内状态增长。

建议调整顺序：

```text
1. 构建当前 expected_by_key。
2. 删除 persistence_by_evidence_dir 中不在 expected_by_key 的 key。
3. 复制 previous = persistence_by_evidence_dir，供 dominant single 排序使用。
4. 按当前 bucket 的 routed input 更新 persistence。
```

这样旧 basis version、旧 stable head 和旧 expected summary 会在下一次 fusion update 时被清掉。

如果清理后 `persistence_by_evidence_dir.size()` 仍超过 `fusion_persistence_max_keys_per_source`：

- 不删除当前 expected universe 内的 key，避免破坏 fusion 语义。
- 记录 `relation_fusion_persistence_key_cap_reached` diagnostics。
- 增加 snapshot 计数，提示需要调大 `fusion_persistence_max_keys_per_source` 或检查 expected universe 是否异常膨胀。

### 6.2 Source 级清理触发

清理只在 `BaselineRelationTask::SubmitObservation()` 中触发。

触发条件：

```text
relation fusion disabled
  -> 不更新 fusion state，也不触发 fusion cleanup

当前 submit 没有创建或更新 fusion state
  -> 不递增 fusion_update_seq_
  -> 可选择不触发 cleanup

当前 submit 成功推进 fusion state
  -> fusion_update_seq_++
  -> 更新该 source 的 last_touched_bucket_id / last_touched_update_seq
  -> 如果 update_seq % cleanup_interval == 0，执行常规 cleanup
  -> 如果 fusion_states_.size() > fusion_state_max_sources，执行容量 cleanup
```

如果新 source 即将创建且 `fusion_states_.size() >= fusion_state_max_sources`，必须先执行容量 cleanup；cleanup 后仍满，则淘汰扫描窗口内最旧的非当前 source，再创建新 state。这样 source 数量不会长期越过上限。

### 6.3 扫描游标

使用 `unordered_map` bucket 级游标做增量扫描：

```text
fusion_cleanup_bucket_cursor_ in [0, fusion_states_.bucket_count())

每次 cleanup:
  scan_count = 0
  while scan_count < cleanup_scan_limit:
    扫描当前 bucket 内 entry
    cursor 前进到下一个 bucket
    到末尾后回绕
```

实现注意：

- `unordered_map` rehash 后 bucket 编号会变化，因此 cursor 只保存 bucket index，不保存 iterator。
- erase 时不要保留被删 entry 的 iterator。
- 如果 bucket_count 为 0，直接返回。
- 每次 cleanup 记录实际扫描 entry 数，而不是扫描 bucket 数。

### 6.4 TTL 淘汰

TTL 判断：

```text
expired =
  state.has_last_bucket
  && watermark_bucket_id > state.last_touched_bucket_id
  && watermark_bucket_id - state.last_touched_bucket_id > fusion_state_ttl_buckets
```

`watermark_bucket_id` 使用当前 submit 的 `obs.bucket_id` 推进：

```text
fusion_cleanup_watermark_bucket_id_ =
  max(fusion_cleanup_watermark_bucket_id_, obs.bucket_id)
```

如果某个 source 提交了旧 bucket，且 `UpdateRelationFusion()` 返回 stale bucket 语义，不更新 `last_touched_bucket_id`，避免乱序旧数据把过期 source 续命。

### 6.5 容量淘汰

容量 cleanup 目标是保持：

```text
fusion_states_.size() <= fusion_state_max_sources
```

在扫描窗口内选择淘汰候选：

1. 优先删除 TTL 过期 state。
2. 如果仍超限，删除扫描窗口内 `last_touched_update_seq` 最小的 state。
3. 不删除当前正在提交的 source。

这是近似 LRU，不保证全局最旧，但满足固定成本和有界状态。

正常新增 source 路径必须做到创建前先腾位，因此不会因为新 source 持续进入而长期超过 `fusion_state_max_sources`。如果配置热加载后把 `fusion_state_max_sources` 调小，或者从旧版本运行态迁移时已经超过上限，则允许通过后续 submit 多次触发 cleanup，逐步回落到上限内。

---

## 7. Snapshot 与可观测性

`QueryTaskSnapshot()` 的 `relation_fusion` 建议扩展。

注意：下面是 snapshot schema 示例，`cleanup_watermark_bucket_id` 不是配置默认值，也不得在代码中写死。实现必须根据已进入该 Relation task 的 `obs.bucket_id` 运行时计算：

```cpp
fusion_cleanup_watermark_bucket_id_ =
    std::max(fusion_cleanup_watermark_bucket_id_, obs.bucket_id);
```

```text
{
  "relation_fusion": {
    "enabled": true,
    "source_state_count": 1024,
    "source_state_max": 4096,
    "persistence_state_count": 8096,
    "persistence_key_max_per_source": 512,
    "state_evicted_total": 128,
    "state_evicted_ttl_total": 96,
    "state_evicted_capacity_total": 32,
    "persistence_key_evicted_total": 4096,
    "cleanup_last_scan_count": 256,
    "cleanup_last_evicted_count": 12,
    "cleanup_watermark_bucket_id": <current_max_observed_bucket_id>,
    "cleanup_ttl_buckets": 20160,
    "cleanup_scan_limit": 256
  }
}
```

字段语义：

- `source_state_count`：当前 `fusion_states_` 中 source 数。
- `persistence_state_count`：所有 source 的 `persistence_by_evidence_dir` key 总数。
- `state_evicted_*`：source 级淘汰累计计数。
- `persistence_key_evicted_total`：evidence key 级清理累计计数。
- `cleanup_last_*`：最近一次 cleanup 的扫描和淘汰情况，用于判断 scan limit 是否过小。
- `cleanup_watermark_bucket_id`：当前 task 已观测到的最大 bucket 水位，用于解释 TTL 清理判断，不是配置项。

`QuerySeriesSnapshot(source)` 保持现有 source 视图：如果该 source 的 fusion state 已被 TTL 或容量淘汰，则不输出旧 `relation_fusion`，并按现有 snapshot 缺失语义处理。

---

## 8. 性能评估

### 8.1 正常 update 成本

单次 fusion update 的新增成本：

| 动作 | 复杂度 | 说明 |
| --- | --- | --- |
| 更新 `last_touched_*` | `O(1)` | 只写当前 source state。 |
| evidence key prune | `O(P)` | `P` 为该 source 的 persistence key 数。当前代码本来会复制整个 persistence map；先 prune 再 copy 通常会降低长期成本。 |
| 常规 cleanup 触发判断 | `O(1)` | 只做计数取模。 |
| cleanup 扫描 | 摊销 `O(scan_limit / interval)` | 默认 `256 / 512 = 0.5` 个 entry / update。 |

容量超限时会增加 cleanup 成本，但每次仍受 `fusion_state_cleanup_scan_limit` 限制。

### 8.2 内存成本

新增字段是 task 级计数和每个 source 两个整数，成本很小。

本方案没有 LRU list、heap、multimap 或 source key 的第二份索引，因此额外内存主要来自：

- `RelationFusionRuntimeState::last_touched_bucket_id`
- `RelationFusionRuntimeState::last_touched_update_seq`
- task 级统计字段

相比 `last_result`、vector、string 和 `persistence_by_evidence_dir`，新增内存可以忽略。

### 8.3 与 LRU / TTL 索引方案对比

| 方案 | 优点 | 缺点 | 本文选择 |
| --- | --- | --- | --- |
| 精确 LRU + TTL index | 淘汰更精确，容量超限可快速删除全局最旧 source | 多套索引复制 source key，erase / rebuild 一致性复杂，内存更高 | 不选首版 |
| 固定扫描预算 + 近似最旧淘汰 | 实现简单，额外内存低，满足有界和可观测 | 不是精确 LRU，配置下调或旧状态已超限时可能需要多次 submit 回落 | 选择 |
| 每次 submit 全表扫描 | 逻辑简单，TTL 精确 | 高基数下热路径不可接受 | 禁止 |

---

## 9. 线程竞态评估

### 9.1 当前契约下不会新增竞态

B6 目标契约是同一个 Baseline task 的 public API 不并发进入。调用方必须保证：

```text
同一个 task:
  SubmitObservation / Predict / QuerySnapshot / Bootstrap / Load / Close
  任意时刻最多一个调用执行
```

在这个契约下，新增字段都只在同 task 串行调用序列内读写，不会引入新的数据竞争。

### 9.2 不引入后台线程

cleanup 不使用后台线程、timer 或异步任务，因此不存在后台 cleanup 与 submit / snapshot 并发访问 `fusion_states_` 的问题。

### 9.3 Snapshot 只读

`QueryTaskSnapshot()` 和 `QuerySeriesSnapshot()` 不触发 cleanup，只读取当前状态和计数。这样不会出现 const snapshot 方法偷偷修改 map 的问题，也不会扩大线程安全承诺。

### 9.4 如果未来支持同 task 并发

如果未来放宽同 task 非并发契约，本设计本身不足以保证线程安全。原因是当前 Relation task 的这些状态也需要统一同步：

- `fusion_states_`
- `basis_states_`
- `routed_shards_`
- `artifacts_by_series_`
- `seeds_by_series_`
- task lifecycle closed flag

届时应整体设计 task 内锁或 actor / strand，而不是只给 cleanup 增加局部锁。否则容易形成“局部看似安全、整体仍有竞态”的伪线程安全。

---

## 10. 测试矩阵

| 测试 | 验收点 |
| --- | --- |
| source 数量超过 `fusion_state_max_sources` | 新 source 进入前触发容量 cleanup，最终 `source_state_count <= max_sources`。 |
| source 超过 TTL | watermark 推进超过 TTL 后，旧 source 被清理，`state_evicted_ttl_total` 增加。 |
| 容量超限但无 TTL 过期 source | 扫描窗口内近似最旧 source 被淘汰，`state_evicted_capacity_total` 增加。 |
| basis version 切换 | `persistence_by_evidence_dir` 中旧 basis version key 被删除，不继续累积。 |
| expected universe 缩小 | 不再 expected 的 summary key 被删除。 |
| `fusion_persistence_max_keys_per_source` 触顶 | 当前 expected universe 不被破坏，snapshot / diagnostics 能看到 cap reached。 |
| `point_count` 等 unrelated baseline API | 清理改动不影响 value / ratio / routed rolling predict 行为。 |
| snapshot schema | `relation_fusion` 输出新增计数，且不会触发状态清理。 |

---

## 11. 实施任务建议

| 任务 | 文件 | 验收 |
| --- | --- | --- |
| `B7-T01` | `rolling/rolling_config.h/.cpp` | 新增配置字段、默认值、校验规则。 |
| `B7-T02` | `config/runtime_config.cpp`、`baseline-config-template.yaml` | YAML 解析、strict schema 白名单和模板同步。 |
| `B7-T03` | `relation/relation_fusion.h/.cpp` | evidence persistence 按当前 expected universe 清理，累计清理计数可回传。 |
| `B7-T04` | `task/relation_task.h/.cpp` | source state TTL / capacity cleanup、扫描游标、统计字段。 |
| `B7-T05` | `task/relation_task.cpp` | task snapshot 输出 cleanup 指标。 |
| `B7-T06` | `src/tests/test_baseline/*` | 覆盖 TTL、容量、basis version 切换、snapshot 指标和 strict config。 |

---

## 12. 风险与处理

| 风险 | 影响 | 处理 |
| --- | --- | --- |
| 默认 `max_sources` 过小 | 活跃 source 被容量淘汰，短期 persistence 丢失 | snapshot 暴露容量淘汰计数；生产按 source 基数调大。 |
| 默认 `max_sources` 过大 | 单 task 内存偏高 | 默认从 20000 收敛到 4096；小规格部署可调到 1024。 |
| 扫描预算过小 | 过期 source 回收延迟 | snapshot 暴露 last scan / evicted；容量超限时每次 submit 都触发 cleanup。 |
| cleanup 删除当前 source | 当前 submit 结果丢失 | 容量 cleanup 明确跳过当前 source。 |
| out-of-order bucket 续命旧 state | 旧 source 无法过期 | stale bucket 不更新 `last_touched_bucket_id`。 |
| 为 cleanup 加局部锁 | 造成虚假的局部线程安全或死锁风险 | 当前不加锁，遵守同 task 非并发契约。 |
