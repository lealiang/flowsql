# B6 Baseline 同 task 串行调用与锁优化方案

## 1. 目标与当前结论

本文基于 Sprint 21 BaselineB 当前实现，重新梳理 Baseline 插件的线程模型、锁边界、线程竞争模型和后续锁优化任务。它是后续落实同 task 非并发调用契约、收敛 Relation 热路径锁和补齐生命周期并发契约的设计输入，不改变当前功能范围。

当前结论：

1. Baseline 自身没有看到独立 worker 线程或后台调度线程；并发主要来自外部调用方通过 `IBaselineService` / `IBaseline*Task` 同时调用 public API。
2. Value / Ratio task 仍是 task 级粗锁模型，同一个 task 下所有 series 的 submit、predict、bootstrap、snapshot 和 export 串行。
3. Relation task 已按 B4/B5 拆出 source ordered lock、basis lock、routed shard lock 和 fusion lock，热路径粒度比 Value / Ratio 更细，但还有两个关键风险：生命周期门闩不足，以及 Bootstrap / Load 重建 runtime 时可与已经开始的 Relation submit 交错。
4. `TaskRegistry::mutex_` 和 runtime config 的 atomic immutable snapshot 边界基本合理。
5. `BaselinePlugin` 没有 plugin 级生命周期锁；若框架不能保证 `Load()` / `Unload()` / `Stop()` 与 service API 串行，`task_registry_` 的 `unique_ptr` 读写存在竞态。
6. 目标设计不继续在 BaselineTask 内部扩大锁模型，而是由上游调度保证同一个 task 不会被并发调用。BaselineTask 按外部串行化状态机实现，不要求同一 task 固定在同一个物理线程，也不额外引入线程身份检查或运行期进入门闩。

## 2. 线程模型

### 2.1 调用方驱动并发

Baseline 插件通过 interface 暴露能力：

```text
IBaselineService
  -> CreateValueTask / CreateRatioTask / CreateRelationTask
  -> QueryServiceSnapshot

IBaselineTask
  -> Close / ExportConfig / QueryTaskSnapshot / QuerySeriesSnapshot

IBaselineValueTask / IBaselineRatioTask / IBaselineRelationTask
  -> SubmitObservation / Predict / Bootstrap / Load / Export / Snapshot
```

代码中没有发现 Baseline 为 submit / bootstrap 创建内部 worker 线程。只要调用方持有 task 的 `shared_ptr`，就可能从多个线程并发调用同一个 task。

### 2.1.1 目标调用契约：同 task 非并发

目标模型将并发边界前移到上游调度层：

```text
同一个 BaselineTask
  -> 任意时刻最多一个 public API 调用处于执行中
  -> 调用之间按上游调度序列串行化
  -> 相邻调用之间由上游建立 happens-before，保证跨线程可见性
  -> 不要求连续调用在同一个物理线程执行
  -> task 内部状态不做并发访问同步

不同 BaselineTask
  -> 可以并行执行
```

这里的“所有 public API”不只包括 `SubmitObservation()`，也包括 `Predict*()`、`Query*Snapshot()`、`Bootstrap()`、`LoadBootstrapArtifact()`、`Export*()` 和 `Close()`。只保证 submit 不并发是不够的；查询、训练、导入和关闭如果与 submit 或其他状态迁移重叠进入同一个 task，同样会破坏 task 内部状态机。

如果上游存在 HTTP 查询线程、生命周期线程或控制面线程，它们不能绕过同 task 串行边界直接与数据面并发进入同一个 task。上游可以用 mailbox / queue、actor、strand、per-task executor 或其他方式实现串行化；具体方案不进入 Baseline 模型，但必须在 C++ 内存模型上提供等价的同步关系，使前一次 task 调用的写入对后一次 task 调用可见。BaselineTask 自身不实现 mailbox / queue，这属于上游调度实现细节。

### 2.2 插件生命周期线程模型

`BaselinePlugin::Unload()` 会调用 `Stop()`，随后替换 `task_registry_` 并重置 runtime config。`Create*Task()` 和 `QueryServiceSnapshot()` 会直接读取 `task_registry_`。

因此线程模型依赖框架契约：

- 如果框架保证插件生命周期回调与 service API 串行，当前实现可接受。
- 如果不保证，`Unload()` 替换 `task_registry_` 时可能与 `Create*Task()` / `QueryServiceSnapshot()` 并发读写同一个 `unique_ptr`，需要 plugin 级生命周期锁或 atomic shared ownership。

### 2.3 Task 对象所有权模型

Task 由 `std::shared_ptr` 管理：

```text
TaskRegistry
  task_id -> shared_ptr<BaselineTaskBase>

Create*Task()
  -> make_shared<Task>
  -> registry.Register(task)
  -> 返回 shared_ptr<IBaseline*Task>
```

`Close()` 会在 task 内设置 `closed_`，再从 registry unregister。已经被调用方持有的 `shared_ptr` 不会因为 unregister 立即失效，所以 `closed_` 是后续 API 拒绝调用的主要生命周期标记。

### 2.4 Value / Ratio 线程模型

Value / Ratio task 的所有 public 方法都持有 `BaselineTaskBase::mutex_` 执行完整逻辑：

```text
SubmitObservation
PredictRolling
Bootstrap
PredictBootstrap
ExportBootstrapArtifact / Seed
LoadBootstrapArtifact
QueryTaskSnapshot / QuerySeriesSnapshot
```

结果是同一个 task 内完全串行。这个模型容易理解，`Close()` 也能等待已进入方法的调用释放 task mutex，但在高基数 series 场景下会把天然独立的 series 全部串成单线程。

### 2.5 Relation 线程模型

Relation task 的 submit 已不再用 `mutex_` 包住完整 fan-out：

```text
SubmitObservation(source)
  -> task mutex 短锁：检查 open
  -> source ordered lock：保持同 source bucket 顺序
     -> basis lock：读取 active basis 快照
     -> routed shard lock：提交 routed summary 到 rolling core
     -> basis lock：更新 stream basis accumulator / refresh
     -> fusion lock：更新 source fusion state
```

这个模型允许不同 source 在不同 source lock stripe 上并发，也允许不同 routed summary shard 并发。但 source ordered lock 是 striped lock，不是无限 per-source mutex；不同 source 哈希到同一 stripe 时仍会串行。

### 2.6 Runtime config 线程模型

Runtime config 使用 `std::shared_ptr<const RuntimeConfigState>` immutable snapshot，并通过 `atomic_load` / `atomic_store` 整体替换。读路径不需要 mutex。

注意当前语义不完全一致：

- Rolling core 每次 `Run*RollingSubmit()` / predict / snapshot resolve `BaselineRollingConfig`，因此可能读到最新全局配置。
- Relation task 构造时缓存 `relation_rolling_config_` 和 `runtime_shard_count_`；后续全局 runtime config reload 不会改变已创建 Relation task 的 routed shard 数、basis/fusion 开关和 relation rolling 子配置。

如果未来支持运行中重载，需要明确“已创建 task 是否感知新配置”的契约。

## 3. 锁清单

| 同步点 | 位置 | 保护对象 | 竞争模型 | 当前判断 |
| --- | --- | --- | --- | --- |
| atomic runtime config snapshot | `config/runtime_config.cpp` | 全局 runtime config | 读无锁，写整体替换 | 合理。需要补齐 reload 对已创建 task 的语义契约 |
| `TaskRegistry::mutex_` | `task/task_registry.*` | `tasks_`、`next_seq_` | 任务表短临界区 | 合理。`Snapshot()` 锁内复制 shared_ptr，锁外遍历 |
| `BaselineTaskBase::mutex_` | `task/baseline_task_base.*` | `closed_`、config JSON；Value / Ratio 派生状态；Relation artifact / seed store | Value / Ratio 热路径粗锁；Relation 主要变成生命周期和 store 慢路径锁 | Value / Ratio 过粗；目标契约下不继续加运行期进入门闩，改由上游同 task 非并发调用契约收敛 |
| `source_ordered_locks_` | `task/relation_task.*` | 同一 source 的 submit 顺序 | `Hash(source_series_key) % shard_count` striped 串行 | 必要。保证同 source bucket 顺序，但不是严格 per-source 独占 |
| `basis_states_mutex_` | `task/relation_task.*` | `basis_states_`、`RelationBasisRuntimeState`、stream basis accumulator | Relation task 全局 basis 锁 | 正确但可能成为跨 source / metric 热点 |
| routed shard mutex | `RelationRoutedRuntimeShard::mutex` | `routed_seeds_by_series`、`routed_specs_by_series`、`routed_rolling_states` | 按 routed series hash 分片 | 正确。锁内执行完整 rolling submit，仍可能是 shard 热点 |
| `fusion_states_mutex_` | `task/relation_task.*` | `fusion_states_`、`RelationFusionRuntimeState` | Relation task 全局 fusion 锁 | 状态较小，当前可接受；必须保持 leaf lock |

`RollingStateMap`、`RelationBasisRuntimeState`、`RelationFusionRuntimeState` 本身没有内部锁，必须由上表对应外部锁保护。

## 4. 关键路径锁序

### 4.1 Service 创建与列表

```text
Create*Task()
  -> task_registry_->Register()
     -> TaskRegistry::mutex_

QueryServiceSnapshot()
  -> task_registry_->Size()
     -> TaskRegistry::mutex_
  -> task_registry_->Snapshot()
     -> TaskRegistry::mutex_
  -> 锁外写 JSON
```

这里 registry 锁粒度较小。主要未闭合点在 plugin 生命周期：`task_registry_` 指针本身没有独立并发保护。

### 4.2 Value / Ratio submit

```text
BaselineTaskBase::mutex_
  -> EnsureOpenLocked()
  -> ResolveBaselineRollingConfig()
  -> RunValueRollingSubmit / RunRatioRollingSubmit()
     -> 读 seed store
     -> 查找 / 插入 / 更新 rolling_states_
     -> band / drift / calibration / maturity / diagnostics
```

同一个 task 下不同 `series_key` 全部串行。`BuildBandDiagnostics()` 等字符串构造仍在锁内，放大了热路径临界区。

### 4.3 Value / Ratio bootstrap / snapshot / export

```text
BaselineTaskBase::mutex_
  -> Bootstrap train / Load artifact
  -> StoreBootstrapArtifact / WarmupRollingStatesFromBootstrapSeeds
```

训练、导入、导出和 JSON snapshot 都会阻塞 submit。正确性成立，但慢路径会直接影响数据面。

### 4.4 Relation submit

当前实现的锁持有不是完整嵌套，而是以 source ordered lock 为外层，分段获取 basis / shard / fusion：

```text
BaselineTaskBase::mutex_
  -> EnsureOpenLocked()
  -> release

source_ordered_locks_[Hash(source)]
  -> basis_states_mutex_        # 读取 active basis 快照
  -> release basis
  -> routed shard mutex         # 每个 routed summary 提交 rolling
  -> release shard
  -> basis_states_mutex_        # Observe / MaybeRefresh
  -> release basis
  -> fusion_states_mutex_       # 检查或更新 fusion state
  -> release fusion
```

这和 B5 设计里“`mutex_ -> source -> basis -> routed -> fusion`”的锁序目标方向一致，但实际没有在持有 `mutex_` 时进入 source lock，也没有同时持有 basis 与 shard。这样降低了死锁面，但也意味着 `mutex_` 不再是 Relation submit 的全程生命周期门闩。

### 4.5 Relation bootstrap / load / rebuild

```text
BaselineTaskBase::mutex_
  -> TrainRelation 或 LoadRelationBootstrapArtifactStore
  -> RebuildRuntimeFromRelationSeedsLocked()
     -> basis_states_mutex_：clear / insert
     -> routed shard mutex：clear / insert routed seed/spec
     -> fusion_states_mutex_：clear
```

这里不会获取 source ordered lock。由于 Relation submit 在 open 检查后会释放 `mutex_`，Bootstrap / Load 的 rebuild 可与已经开始的 submit 交错。

### 4.6 Relation snapshot / routed query

```text
QueryTaskSnapshot()
  -> BaselineTaskBase::mutex_
  -> basis_states_mutex_
  -> each routed shard mutex
  -> fusion_states_mutex_
  -> 锁外写 JSON

QuerySeriesSnapshot(source)
  -> BaselineTaskBase::mutex_ 短锁 open check
  -> basis_states_mutex_
  -> each routed shard mutex
  -> fusion_states_mutex_
  -> 锁外写 JSON

PredictRoutedSummary / QueryRoutedSummarySnapshot
  -> BaselineTaskBase::mutex_ 短锁 open check
  -> basis_states_mutex_（仅 basis-scoped 且未指定 basis_version）
  -> routed shard mutex
```

这些 snapshot 是分段弱一致视图：不会读写裸数据造成数据竞争，但不能保证 basis、routed state、fusion state 来自同一个全局时间点。

## 5. 线程竞争热点

### 5.1 Value / Ratio task 粗锁

热点来自 `BaselineTaskBase::mutex_`：

1. 不同 `series_key` 不能并行 submit。
2. `PredictRolling()` 与 submit 串行。
3. `Bootstrap()` 训练慢路径阻塞 submit。
4. export 和 snapshot 的 JSON 序列化阻塞 submit。

高基数 series 场景下，这是当前最直接的吞吐瓶颈。

### 5.2 Relation source ordered lock

`source_ordered_locks_` 保证同一 source 的 bucket 顺序，符合 B4 要求：rolling core 对 routed state 有时间顺序假设，不能让同 source 的 bucket 交错提交。

竞争点：

1. 一个 source block 的 projection、routed fan-out、basis update 和 fusion update 都在 source lock 内。
2. source lock 是 hash stripe；不同 source 哈希碰撞时串行。
3. 如果单个 source 产生大量 routed summaries，会长时间占用该 stripe。

### 5.3 Relation routed shard lock

每个 routed summary 会按 `routed_series_key` 定位 shard。锁内执行完整 `RunValueRollingSubmit()` 或 `RunRatioRollingSubmit()`，包括 config resolve、state update、band、score trust、maturity 和 diagnostics。

竞争点：

1. 同 shard 的不同 routed series 串行。
2. shard 数由 `relation_rolling_config_.routed_state_shard_count` 决定，已创建 task 不会随 runtime config reload 改变。
3. routed summary 分布不均时，热点 shard 会成为 Relation submit 的主要等待点。

### 5.4 Relation basis 全局锁

`basis_states_mutex_` 是每个 Relation task 一把全局锁，保护所有 `(source_series_key, metric)` basis runtime state。

竞争点：

1. 所有 source / metric 的 active basis 读取串行。
2. 所有 stream basis accumulator update 和 `MaybeRefresh()` 串行。
3. Bootstrap / Load rebuild 会 clear / insert basis state，与 submit 互相等待。

当前 basis state 有界，锁内工作量通常可控，但在多 source、多 metric、高频 submit 下会成为全局热点。

### 5.5 Relation fusion 全局锁

`fusion_states_mutex_` 保护 source 级 fusion state。B5 设计要求它是 leaf lock，不得在持有它时再反向获取 task / basis / routed shard 锁。

竞争点：

1. 所有 source 的 fusion update 串行。
2. snapshot 读取 `last_result` 与 submit update 串行。
3. 状态较小，当前优先级低于 source / basis / routed shard，但后续如果 fusion 逻辑变重，应先复制输入、缩短锁内 update。

### 5.6 Bootstrap / Load 与 Relation submit 交错

这是当前最需要明确的正确性风险。

Relation `Bootstrap()` / `LoadBootstrapArtifact()` 持有 `mutex_` 做训练或导入，并调用 `RebuildRuntimeFromRelationSeedsLocked()` 清理和重建 basis、routed seed/spec/state、fusion state。但 Relation `SubmitObservation()` 只在入口短暂持有 `mutex_` 做 open check，随后释放。

可能交错：

```text
Thread A: SubmitObservation(source)
  -> open check 通过
  -> 读取 old active basis 快照

Thread B: Bootstrap / Load
  -> 持有 mutex_
  -> clear / rebuild basis, routed shards, fusion

Thread A:
  -> 基于 old basis 投影 routed summary
  -> 写入 rebuild 后的 routed shard / basis / fusion
```

外部 mutex 基本能避免 C++ 数据竞争，但语义上一次 submit 可能混用旧 basis 快照和重建后的 routed/fusion runtime。另一个交错是 submit 在 rebuild 的 clear 与 insert 中间观察到空 basis，导致 source 状态进入 stream-only 路径。

### 5.7 Close 与 Relation in-flight submit

Value / Ratio 的 `Close()` 会等待正在执行的 public 方法释放 task mutex。Relation submit 释放 `mutex_` 后继续执行，因此：

```text
Thread A: Relation SubmitObservation()
  -> open check 通过
  -> release mutex_
  -> source lock / basis / routed / fusion

Thread B: Close()
  -> mutex_ 设置 closed_ = true
  -> unregister
  -> 返回

Thread A:
  -> 继续完成 submit
```

如果产品契约允许“Close 后已开始的 submit 可以自然完成”，这是可接受的弱语义；如果 Close 需要成为停止点，则当前实现不满足。当前文档和接口没有把这个差异写清楚。

### 5.8 Snapshot 弱一致视图

Relation snapshot 分段读取 basis、routed shard 和 fusion state，不持有 source ordered lock，也不持有一个全局 runtime 读锁。因此它能避免长时间阻塞 submit，但只能提供弱一致观测：

1. `QuerySeriesSnapshot(source)` 中的 basis 可能早于 routed entries。
2. routed entries 可能早于 fusion `last_result`。
3. task 级计数可能不是同一瞬间的全局计数。

如果 snapshot 只用于观测，这是合理折中；如果后续用于可恢复 checkpoint 或审计证据，需要引入版本化 snapshot 或全局 runtime 读锁。

## 6. 正确性风险清单

| 风险 | 严重度 | 说明 | 建议 |
| --- | --- | --- | --- |
| Relation Bootstrap / Load rebuild 与 in-flight submit 语义交错 | 高 | 无数据竞争不等于语义一致；一次 submit 可混用旧 basis 和新 runtime | 目标契约下由同 task 非并发调用消除；实现前不得删除现有保护 |
| Relation Close 不等待已过 open check 的 submit | 中高 | Close 语义弱于 Value / Ratio，且接口未说明 | 目标契约下 `Close()` 是 task 调用序列中的状态迁移点；接口必须写明同 task 非并发调用契约 |
| Plugin 生命周期与 service API 竞态 | 中高 | `task_registry_` 是无锁 `unique_ptr` | 依赖框架串行契约则写入设计；否则加 plugin lifecycle lock |
| Relation snapshot 弱一致 | 中 | 观测接口可接受，恢复 / 审计不可接受 | 目标契约下 snapshot 成为 task 调用序列中的序列点；实现前仍按弱一致理解 |
| Value / Ratio 粗锁吞吐瓶颈 | 中 | 高基数 series 下 submit 无法并行 | 目标契约接受同 task 串行；吞吐通过多 task 并行扩展 |
| Relation basis 全局锁热点 | 中 | 多 source / metric submit 串行读写 basis | 目标契约接受同 task 串行；锁可在后续实现中删除 |
| Runtime config reload 语义不一致 | 中 | rolling core 每次 resolve，Relation 子配置构造时缓存 | 明确已创建 task 的配置冻结 / 动态读取边界 |

## 7. 目标设计方案：上游保证同 task 非并发

### 7.1 设计选择

Baseline task 简化为由上游外部串行化的状态机：

```text
同 task 调用序列
  -> SubmitObservation
  -> Predict / QuerySnapshot
  -> Bootstrap / Load / Export
  -> Close
```

同一个 task 内不再追求并发执行。原因是 baseline 的核心状态天然依赖顺序：

1. Value / Ratio rolling state 要按 bucket 顺序更新。
2. Relation source block fan-out 后的 routed rolling、basis refresh 和 fusion result 都是同一 source 状态机的一部分。
3. Bootstrap / Load 会整体替换或重建 runtime state，本质上是 task 状态迁移点。
4. Snapshot / export 应观测 task 序列中的某个确定位置，而不是跨多个锁拼出来的弱一致视图。

并行粒度上移到 task：

```text
task A -> 调用序列 A
task B -> 调用序列 B
task C -> 调用序列 C
```

不同 task 可以并行；同一个 task 串行。这里的“串行”只表示同一 task 的调用不重叠，不表示同一 task 必须固定在同一个物理线程。

### 7.2 契约

目标契约：

```text
BaselineTask threading contract:

1. `IBaselineTask` 及其派生接口返回的 task 对象不是线程安全对象，不为同一 task 的并发访问提供内部同步。
2. 调用方 / 上游调度必须保证同一个 task 的 public API 调用不会重叠执行。
3. 如果同一个 task 的连续调用发生在不同物理线程，上游调度必须在前一次调用结束与后一次调用开始之间建立 happens-before，保证 task 内部非 atomic 状态的可见性。
4. 同一个 task 的连续调用可以由不同物理线程执行；Baseline 不要求也不检查固定线程。
5. `Id()`、`Name()`、`Kind()` 是 immutable identity getter，在 task 对象生命周期内必须保持跨线程读取能力。
6. 不同 task 可以在不同线程并行调用。
7. `IBaselineService`、`TaskRegistry`、plugin lifecycle 和 runtime config 是独立并发边界，不由 task 非并发调用契约自动覆盖。
```

这个契约必须在后续代码实现时写入 `src/framework/interfaces/ibaseline_service.h` 和 `src/plugins/baseline/README.md`。建议放在 `IBaselineTask` 注释前，并明确所有派生 task 接口继承该同 task 非并发调用约束。接口形态仍返回 `std::shared_ptr<IBaseline*Task>`；B6 不引入 task wrapper、mailbox 或 queue 类型到 Baseline public ABI。

接口注释建议口径：

```cpp
// Threading contract:
// Baseline task instances are not internally synchronized for concurrent
// access. The caller or upstream scheduler must ensure that calls for the
// same task instance do not run concurrently.
// If calls for the same task instance are executed by different physical
// threads, the upstream scheduler must establish a happens-before relation
// between the end of one call and the beginning of the next call.
// Id(), Name() and Kind() are immutable identity getters and may be read
// across threads during the task instance lifetime.
// Calls for different task instances may run concurrently. The same task
// instance does not require a stable physical thread between calls.
// Overlapping calls on the same task instance are outside this interface
// contract unless a future implementation explicitly documents stronger
// guarantees.
```

`Id()`、`Name()`、`Kind()` 的跨线程读取能力不是可选优化，而是 `IBaselineService::QueryServiceSnapshot()` 等 service 边界的必要能力。实现时应保证这些字段在构造后不可变，getter 不依赖 task runtime mutex。除这 3 个 immutable identity getter 外，默认仍按“同 task 所有方法不可重叠执行”理解。

### 7.3 对当前锁模型的影响

在该契约成立后，task 内部状态可以按无 mutex 模型收敛：

| 当前锁 | 目标处理 | 原因 |
| --- | --- | --- |
| `BaselineTaskBase::mutex_` | 不再作为 task runtime 锁 | `closed_`、artifact / seed、rolling state 都只在同 task 调用序列内访问 |
| `source_ordered_locks_` | 可删除 | 同一 Relation task 的 source submit 已由同 task 调用序列全局串行 |
| `basis_states_mutex_` | 可删除 | basis runtime state 只在同 task 调用序列内访问 |
| routed shard mutex | 可删除 | routed seed/spec/state 只在同 task 调用序列内访问，shard 只保留容量和索引用途时才需要 |
| `fusion_states_mutex_` | 可删除 | fusion state 只在同 task 调用序列内访问 |

这不是 C++ 意义上的 lock-free 数据结构，而是外部串行化状态机模型。它通过上游调度契约消除 task 内部竞争，不在 task 内增加额外保护性限制。

### 7.4 风险收敛

该目标模型下，当前高风险项的处理方式变为：

1. `Bootstrap()` / `LoadBootstrapArtifact()` 与 `SubmitObservation()` 不再并发进入同一 task，Relation runtime rebuild 与 submit 语义交错问题消失。
2. `Close()` 是 task 调用序列中的一个状态迁移点。排在 `Close()` 之前的调用先完成，排在之后的调用看到 closed 状态并拒绝。
3. `QuerySeriesSnapshot()` / `QueryTaskSnapshot()` 观测的是 task 调用序列中的确定位置，不再需要跨 basis / routed / fusion 分段弱一致解释。
4. Value / Ratio 不再需要在同 task 下按 series 拆 shard 锁；同 task 内并发不是目标。

仍需保留或单独处理的同步边界：

1. `TaskRegistry::mutex_` 仍保护 task 表和 task id 分配。
2. `BaselinePlugin` lifecycle 若可能与 service API 并发，仍需要 plugin 级保护或框架串行契约。
3. Runtime config 的 atomic immutable snapshot 仍可保留。
4. 上游调度层必须能证明同一 task 不会并发进入，并在跨线程串行 handoff 时建立 happens-before；Baseline 后续实现不增加线程身份检查或运行期进入门闩作为必要保护，避免把调度层可合法选择的跨线程串行调用变成额外限制。

### 7.5 实现任务顺序

以下任务按 B4 / B5 设计文档的任务表格式编排。B5 已完成，本锁优化作为 B6 独立阶段推进，因此任务编号从 `B6-T01` 开始。任务仍引用 B4 / B5 已确认的 Relation 顺序和状态生命周期，不能借 B6 锁优化改变既有业务语义。

| 任务 | 名称 | 设计引用 | 主要文件 | 完成标准 |
| --- | --- | --- | --- | --- |
| `B6-T01` | 补齐 Baseline task 非并发 public ABI 契约 | 本文第 2.1.1、7.2 节 | `src/framework/interfaces/ibaseline_service.h`、`src/plugins/baseline/README.md` | `IBaselineTask` 前写明 task object 不为并发访问提供内部同步；同一 task 所有 public API 不得重叠执行；跨线程串行 handoff 必须由上游建立 happens-before；不要求稳定物理线程；`Id()` / `Name()` / `Kind()` 保持跨线程 immutable getter；派生接口继承该约束；README 同步记录能力、使用方式和接口契约 |
| `B6-T02` | 对齐 Baseline README 与外部框架同 task 串行调度契约 | 本文第 2.1.1、7.2、7.4 节 | `src/plugins/baseline/README.md`、必要的外部框架契约文档、必要时补充 sprint planning | Baseline README 中声明进入 Baseline task 前必须完成同 task 串行化和 happens-before 保证；外部框架文档引用或复述该契约；HTTP 查询、控制面和生命周期调用不得绕过串行边界与数据面并发进入；Baseline 接口仍返回 `std::shared_ptr<IBaseline*Task>`，不绑定固定线程，不引入 task wrapper / mailbox / queue 到 public ABI |
| `B6-T03` | 补齐同 task 串行 handoff 回归 | 本文第 7.2、7.4、8 节 | `src/tests/test_baseline/*`、必要的调用方测试 | 覆盖同一 task 的 submit、query、bootstrap、load、export 和 close 在测试构造的外部同步串行 handoff 下得到确定结果；同一 task 可由不同线程先后调用且不被拒绝；该测试只验证 Baseline 行为兼容跨线程串行调用，不声称证明生产上游不会并发；不增加线程身份检查或运行期进入门闩；此阶段不删除现有锁 |
| `B6-T04` | 收敛 Value / Ratio task 级锁 | 本文第 4.2、4.3、5.1、7.3 节 | `task/value_task.*`、`task/ratio_task.*`、`task/baseline_task_base.*`、`rolling/rolling_task_runner.*` | Value / Ratio 的 submit、predict、bootstrap、load、export、snapshot 改为依赖上游同 task 非并发契约，不再用 `BaselineTaskBase::mutex_` 保护 rolling / artifact / seed runtime；行为和 JSON schema 不变 |
| `B6-T05` | 收敛 Relation task base mutex 与 rebuild 序列 | 本文第 4.5、5.6、7.4 节；B4 第 8 节；B5 第 7.2、7.3 节 | `task/relation_task.*` | Relation `Bootstrap()` / `LoadBootstrapArtifact()` / `Close()` 成为 task 调用序列中的状态迁移点；runtime rebuild、fusion clear、seed/spec/basis 重建顺序保持 B4/B5 语义；不再依赖 `BaselineTaskBase::mutex_` 防并发交错 |
| `B6-T06` | 收敛 Relation source ordered lock | 本文第 5.2、7.3 节；B4 第 8.2 节；B5 第 7.2 节 | `task/relation_task.*` | 删除 `source_ordered_locks_` 的同步职责；同一 Relation task 的 source bucket 顺序由同 task 调用序列保证；保留 projection → routed fan-out → basis update → fusion update 的 B4/B5 顺序 |
| `B6-T07` | 收敛 Relation routed shard lock | 本文第 5.3、7.3 节；B4 第 8.1、8.2 节；B5 第 7.2 节 | `task/relation_task.*`、`rolling/rolling_task_runner.*` | 删除 `RelationRoutedRuntimeShard::mutex` 的同步职责；`routed_shards_` 可继续作为有界分片状态容器，但不再承担线程互斥；routed submit 仍复用 B2/B3 rolling core，routed key 和 snapshot 语义不变 |
| `B6-T08` | 收敛 Relation basis / fusion runtime 锁 | 本文第 5.4、5.5、7.3 节；B4 第 6、8 节；B5 第 7.1、7.2、7.3 节 | `task/relation_task.*`、`relation/relation_basis_state.*`、`relation/relation_fusion.*` | 删除 `basis_states_mutex_`、`fusion_states_mutex_` 的同步职责；basis accumulator / refresh、fusion update、fusion clear 仍按 B4/B5 状态生命周期执行；B5 中“fusion leaf lock”约束退化为“fusion update 不反向改写 routed / basis” |
| `B6-T09` | 私有化并删除 `BaselineTaskBase::mutex_` | 本文第 3、7.3 节 | `task/baseline_task_base.*`、所有派生 task | 派生类不再直接访问 `mutex_`；先将 `mutex_` 从 protected 移出防止新增依赖，确认无用途后删除；`closed_`、config 和 runtime 字段访问均符合同 task 非并发契约；identity 字段构造后不可变，`Id()` / `Name()` / `Kind()` 不依赖 task runtime mutex |
| `B6-T10` | 保留 task 外同步边界 | 本文第 2.2、3、7.4 节 | `task/task_registry.*`、`baseline_plugin.*`、`config/runtime_config.*` | `TaskRegistry::mutex_` 保留；runtime config atomic snapshot 保留；plugin lifecycle 若框架不保证串行，需要补 plugin 级保护或在框架契约中写明串行保证 |
| `B6-T11` | 自动化测试、文档与回归验证 | 本文第 8 节；B4 / B5 测试矩阵 | `src/tests/test_baseline/*`、`src/plugins/baseline/README.md` | 覆盖同 task 顺序、同 task 跨线程但非并发调用、不同 task 并行、Relation submit / bootstrap / load / close 序列、snapshot 序列点、registry / plugin 外边界；TSAN 或等价工具无 task 外数据竞争；README 的能力 / 使用 / 契约与 public ABI 和测试覆盖保持一致 |

执行约束：

1. `B6-T01` 到 `B6-T03` 是删除任何 task 内部锁之前的契约和验证前置条件。
2. `B6-T04` 到 `B6-T08` 必须保持 B4/B5 已确认的业务顺序，不允许借锁优化改变 Relation projection、routed fan-out、basis refresh、fusion update、runtime rebuild 或 snapshot schema。
3. `B6-T09` 必须晚于 Value / Ratio 和 Relation 对 `mutex_` 的依赖收敛；不得先删 base mutex 再补齐接口契约和上游串行边界。
4. `B6-T10` 不属于可删除项。同 task 非并发调用契约只覆盖单 task 内部状态，不覆盖 registry、plugin lifecycle 和 runtime config。
5. 任一任务发现同一 task 仍可能并发直接进入，或跨线程串行 handoff 缺少 happens-before 保证，必须先回到 `B6-T02` 修正外部框架契约；不得在 Baseline task 内追加线程身份检查或运行期进入门闩代替上游契约。

## 8. 验证建议

后续按同 task 非并发模型实现时，至少补以下验证：

1. 文档契约检查：`src/plugins/baseline/README.md` 和外部框架契约文档明确同一 task 的 submit、query、bootstrap、load、export 和 close 不会重叠执行，并在跨线程 handoff 时建立 happens-before。
2. 跨线程串行兼容测试：同一个 task 可由不同线程通过测试构造的同步 handoff 先后调用，只要调用不重叠且有 happens-before，结果应确定且不应被 thread id 检查拒绝。
3. 不同 task 并行测试：多个 task 可以并行推进且状态互不串扰。
4. Relation 顺序测试：同一 task 的 `SubmitObservation()`、`Bootstrap()`、`LoadBootstrapArtifact()`、`Close()` 按调用序列得到确定结果，不再出现 rebuild 与 submit 交错。
5. Snapshot 序列点测试：`QueryTaskSnapshot()` / `QuerySeriesSnapshot()` 返回 task 调用序列中的确定状态。
6. Registry / plugin lifecycle 测试：`TaskRegistry` 和 `BaselinePlugin` 生命周期边界仍有同步或框架串行保证。
7. TSAN 或等价线程 sanitizer 仍应覆盖 baseline 测试，但重点从 task 内部锁竞争转为验证没有 task 外边界的数据竞争。
