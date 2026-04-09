# Sprint 16 设计文档：跨任务共享 Source（late join）与共享分发统一内核

## 背景

Sprint 14 已完成单任务 Group DAG（14.13），并通过 `BroadcastHub` 支撑组内 share set 的同源单读多分支广播；但当前流式执行仍存在关键限制：

1. source 采用任务独占租约，跨任务同源并发消费会被 `STREAM_SOURCE_IN_USE` 拒绝。
2. `BroadcastHub` 仅服务单任务固定成员广播，不支持运行中动态 join/leave。
3. 共享分发能力存在概念分裂风险：若直接新增第二套并行模型，后续维护成本会明显上升。

Sprint 16 目标是在同一迭代内完成两件事：

1. 落地 Story 14.15：跨任务共享 source + late join。
2. 完成共享分发统一：将 Group share set 广播迁移到新内核，移除 `BroadcastHub`。

---

## 设计目标

1. 支持多个 stream 任务并发消费同一 source（跨任务）。
2. 支持 late join：新任务加入后从加入时刻开始消费，不补历史。
3. 保证任务隔离：任一任务 stop/cancel/fail 不影响其他消费者。
4. 慢消费者默认不阻塞全局 source reader。
5. 不改 SQL 语法，不改执行 URI，不引入 V1/V2 双轨。
6. 同迭代内完成 `BroadcastHub -> SharedSourceHub` 融合，收敛为单一共享分发概念。
7. 保持对 Sprint 14 已上线能力的行为兼容（尤其是 Group share set 的一致性语义）。

---

## 非目标

1. 历史回放与持久化重放。
2. 跨主机分布式共享 source。
3. batch + stream 混合 DAG（Story 14.14）。
4. 新增 SQL 语法扩展。

---

## 设计原则

1. 不引入长期并行概念：Sprint 16 结束后仅保留 `SharedSourceHub` 一套共享分发模型。
2. 不做语义兜底分支：所有共享消费统一走 hub 装配，不保留临时 bypass。
3. 不破坏既有契约：URI、SQL 语法、核心状态机语义保持稳定。
4. 高风险路径优先可观测：并发冲突、背压丢弃、回收失败必须有指标与错误码。

---

## 现状与问题定位

### 1) source 独占租约限制跨任务并发

当前 `TryAcquireStreamTaskLeases` 会对 source key 施加 owner 互斥，同 key 不同 owner 会返回 `EBUSY`，导致跨任务共享 source 不可用。

### 2) Group share set 绑定 `BroadcastHub`

Group 构建路径会为 share set 构造 `BroadcastHub`，并生成内部中转 ring 通道给各 member node，属于“固定成员 + 预启动屏障”的专用实现。

### 3) 管理面状态依赖 `BroadcastHubSnapshot`

`status/list` 当前通过 share set snapshot 输出广播指标，若新增另一套共享运行时而不收敛，JSON 契约将长期分裂。

---

## 统一方案总览

### 核心结论

引入单一共享分发内核 `SharedSourceHub`，以模式化方式覆盖两类业务语义：

1. `dynamic` 模式：跨任务共享 source（late join）。
2. `fixed` 模式：Group share set（固定成员，保留 ready 屏障 + coordinated drop）。

Sprint 16 不保留长期并行架构：

1. 阶段1：先接入 `dynamic`，实现 14.15。
2. 阶段2（同迭代）：Group share set 切换到 `fixed`，删除 `BroadcastHub`。

---

## 对现有机制影响评估（全盘）

### 1) Scheduler 主执行链路

1. 受影响：
   1. stream source 装配（`BuildStreamExecutionPlan`）从“直连 source”改为“订阅 hub 输入”。
   2. source lease 判定从任务独占改为 hub + subscriber 引用模型。
   3. group share set 构建与 stop 流程迁移到 `SharedSourceHub(fixed)`。
2. 不受影响：
   1. SQL 解析器语法与 selector 规则。
   2. sink 解析与 sink 并发能力校验。
   3. `StreamRuntime` 线程池与 `StreamTask` shard 调度模型。

### 2) TaskPlugin 与任务存储

1. 受影响：
   1. `status/list` 透传新增共享拓扑字段。
   2. 错误码透传扩展到 shared hub 场景。
2. 不受影响：
   1. `task_id/runtime_task_id` 生成规则。
   2. 任务表核心状态迁移逻辑。

### 3) 管理面与前端

1. 受影响：
   1. 任务详情增加共享消费可观测字段展示。
   2. stream 状态页展示 hub/subscriber 指标摘要。
2. 不受影响：
   1. 提交流程与 SQL 编辑体验。
   2. 通道创建/修改 API 契约。

### 4) 回归风险点

1. Group share set 迁移后的一致性指标（原 T49/T49.1 语义）。
2. `modify/remove` 与 execute 并发时 in-use 判定。
3. runtime retention 与 stop 并发清理幂等性。

---

## 架构设计

### 1) 新增组件

新增文件：

1. `src/services/scheduler/shared_source_hub.h`
2. `src/services/scheduler/shared_source_hub.cpp`

核心对象：

1. `SharedSourceHubManager`
2. `SharedSourceHub`
3. `SharedSubscriberHandle`（RAII）
4. `SharedHubSnapshot`
5. `SharedSubscriberSnapshot`

### 2) 关键职责

#### `SharedSourceHubManager`

1. 维护 `hub_key -> hub instance` 全局映射。
2. 负责“查找或创建 hub”的原子路径。
3. 维护 `runtime_task_id -> subscriber handles` 反向索引，供 stop/retention 回收。
4. 提供管理面快照查询接口。

#### `SharedSourceHub`

1. 单 reader 从上游 source `PollNext`。
2. fanout 到多个 subscriber 输入队列（内部 IStreamChannel）。
3. 维护 hub 级统计与每 subscriber 统计。
4. 支持运行中 Add/RemoveSubscriber（dynamic）。
5. 支持固定成员 gate（fixed）。

#### `SharedSubscriberHandle`

1. 绑定 `hub + subscriber_id + runtime_task_id`。
2. 析构自动退订，确保异常路径无泄漏。
3. 对调用方暴露 subscriber 输入通道（`std::shared_ptr<IStreamChannel>`）。

---

## 统一语义模型

### 1) hub 模式

#### `SharedHubMode::kDynamic`

用于跨任务共享 source：

1. 允许运行中新增订阅者（late join）。
2. 新订阅者从“加入成功后的下一批次”开始消费。
3. 不要求所有订阅者同步就绪。

#### `SharedHubMode::kFixed`

用于 Group share set：

1. 成员在构建期固定。
2. 启动前需要通过 ready 屏障。
3. 分发策略采用 `coordinated_drop`，保持组内集合一致性。

### 2) 背压策略

首版策略：

1. 默认 `drop`（dynamic/fixed 均可用）。
2. fixed + coordinated_drop：任一成员不可写时全员一致 drop。
3. dynamic：按 subscriber 粒度 drop，不阻塞全局 reader。

预留扩展：

1. `detach_on_slow`（后续可选）。
2. `block` 不作为本迭代默认策略（避免全局阻塞风险）。

### 3) `WHERE` 下推一致性约束（必须）

为避免共享场景中 `SetFilter` 被内部 ring 通道静默吞掉而导致语义漂移，本迭代明确以下规则：

1. hub 维护 `where_signature`（`trim(where_clause)`；空串表示无过滤）。
2. 首个订阅者创建 hub 时，若 `where_signature` 非空，必须在 hub 启动前对上游 source 执行一次 `SetFilter`。
3. 后续订阅者仅允许附着到同一 `where_signature` 的 hub；签名不一致直接报错（不做隐式 fallback）。
4. `where_signature` 为空的 hub，不允许后续以非空签名附着；反之亦然。
5. Group fixed share set 路径同样遵守该约束，确保组内共享 source 语义一致。

---

## 数据结构设计

### 1) Hub Key（统一身份）

`hub_key = CanonicalSourceKeySet(resolved_source_keys)`

规则：

1. source key 去重。
2. 字典序排序。
3. 使用不可见分隔符拼接（与现有 `CanonicalKeysHash` 一致语义）。

说明：

1. 相同 canonical key 集合复用同一个 hub。
2. 支持 `stream_hub(split)` 自动展开后的分区 key 集合。

### 2) SharedSourceHub 内部字段（示意）

```cpp
struct SharedSubscriberState {
    std::string subscriber_id;
    std::string runtime_task_id;
    std::string logical_node_id; // group node id or empty
    std::shared_ptr<IStreamChannel> input;
    bool active = true;
    bool ready = false;          // fixed mode gate
    uint64_t delivered_batches = 0;
    uint64_t delivered_rows = 0;
    uint64_t dropped_batches = 0;
    uint64_t dropped_rows = 0;
    uint64_t last_delivered_seq = 0;
    uint64_t last_dropped_seq = 0;
};

class SharedSourceHub {
  SharedHubMode mode_;
  std::string hub_id_;
  std::vector<std::string> source_keys_;
  std::string where_signature_;
  std::string source_ref_;
  std::shared_ptr<IStreamChannel> source_;
  std::unordered_map<std::string, SharedSubscriberState> subscribers_;
  std::atomic<uint64_t> broadcast_seq_{0};
  ...
};
```

### 3) Scheduler 侧新增索引

在 `SchedulerPlugin` 增加：

1. `shared_hubs_mu_` + `shared_hubs_`（`hub_key -> hub`）。
2. `runtime_subscriptions_mu_` + `runtime_subscriptions_`（`runtime_task_id -> vector<SharedSubscriberHandle>`）。
3. `group_share_set_bindings_`（group runtime -> share set -> hub/subscriber mapping）用于 group 状态展示与收敛。

---

## 执行链路设计

### 1) Single Stream Task 执行

`BuildStreamExecutionPlan` 阶段：

1. 完成 `ResolveSourceBindings` 后得到 canonical source keys。
2. 计算 `where_signature = trim(stmt.where_clause)`。
3. 通过 `SharedSourceHubManager::AcquireOrAttach(dynamic, where_signature)` 获取/创建 hub。
4. 在 hub 首次创建时执行上游 source `SetFilter` 绑定；失败则执行失败并返回明确错误码。
5. 创建订阅者并取得 subscriber input channel。
6. 用 subscriber input 替换原 plan.source。
7. 保存 `SharedSubscriberHandle` 到 runtime 索引（供 stop/retention 自动退订）。

### 2) Group Task 执行（share set）

`HandleStreamExecuteGroup` 阶段：

1. 自动识别 share set 后，不再构造 `BroadcastHub`。
2. 对每个 share set 建立 `SharedSourceHub(fixed)`。
3. 为每个 member node 创建固定订阅者，绑定 node source override。
4. 保留 share set ready 屏障逻辑；ready 后再启动 hub reader。
5. 组 stop/fail 时通过 hub 统一收敛。

### 3) 非 share set 节点

保持现有路径：

1. 独立 source 走 shared hub dynamic（若同源复用自动发生）。
2. 语义不变。

---

## 租约模型改造

### 1) 原则

1. source 由“任务独占”改为“hub 占用 + 订阅计数”。
2. sink 仍保持原语义（含并发能力校验）。
3. `modify/remove` 仍必须拦截 in-use。

### 2) 新模型

#### Source Lease

1. `stream_source_leases_[source_key]` 记录 `hub_owner_id` 与 ref_count。
2. 同 source key：
   - 若 hub 已存在，任务仅增加 subscriber 引用。
   - 若 hub 不存在，首任务创建 hub 并占用 source lease。
3. hub 销毁时释放 source lease。

#### Task Lease

1. `stream_task_leases_` 继续记录 task 关联 key，便于 retention 清理。
2. 对 source key 的释放改为“退订 -> hub 决定是否释放 source lease”。
3. sink key 释放保持现有逻辑。

### 3) TOCTOU

保持并强化：

1. 解析后版本快照。
2. 在统一临界区做版本复核 + 引用登记。
3. 失败路径回滚。

---

## 生命周期与状态机

### 1) Hub 生命周期

1. `created` -> `running` -> `stopping` -> `stopped|failed|cancelled`。
2. dynamic 模式下允许 `running` 期间 Add/RemoveSubscriber。
3. fixed 模式下 AddSubscriber 仅允许 `created/preparing`。

### 2) 任务生命周期联动

1. `execute` 成功后：runtime 与 subscriber handle 绑定。
2. `stop/cancel/fail`：先退订，再按引用计数决定 hub 停止。
3. group stop 顺序保持：先停 share hub，再停 node，再 join。

### 3) Retention 清理

1. terminal runtime 清理时，必须先清理 runtime_subscriptions。
2. 最后一个 subscriber 退出后自动触发 hub 回收。
3. 清理异常不影响主流程，但需记录错误日志与指标。

---

## 并发模型与锁顺序

### 1) 全局锁顺序（强约束）

1. `shared_hubs_mu_`
2. `stream_channel_refs_mu_`
3. `stream_tasks_mu_ / stream_task_groups_mu_ / stream_group_*`

禁止反向获取。

### 2) 禁止事项

1. 禁止在全局锁内执行 `PollNext/Put/Open/Close/Join`。
2. 禁止在 hub 内部锁持有期间回调 Scheduler 全局对象。
3. 禁止锁内调用外部插件接口。

### 3) 原子性边界

1. “查找/创建 hub + source lease 登记”必须是原子路径。
2. “runtime 绑定 subscriber handle”必须在任务可查询前完成。
3. stop 和 retention 对同 runtime 的清理需幂等。

---

## API 与响应契约

### 1) URI

不新增、不修改：

1. `/scheduler/stream/execute|status|stop|list`
2. `/tasks/stream/execute|status|stop|list`

### 2) status/list 扩展字段

在现有 JSON 上扩展：

1. `shared_hub_id`
2. `shared_source_keys`
3. `subscriber_count`
4. `subscriber_stats`（数组）
5. group 下 `share_sets` 继续保留，但来源改为 SharedSourceHub snapshot。

### 3) 兼容性策略

本项目构建期，不做双轨兼容。

1. 字段新增遵循“只增不删”本迭代策略。
2. `BroadcastHubSnapshot` 类型在阶段2完成后替换为通用 `SharedHubSnapshot`。

---

## 错误码设计

在现有 `ErrorCodeId` 基础上新增（建议）：

1. `kSharedSourceHubCreateFailed` -> `SHARED_SOURCE_HUB_CREATE_FAILED`
2. `kSharedSourceSubscribeFailed` -> `SHARED_SOURCE_SUBSCRIBE_FAILED`
3. `kSharedSourceModeMismatch` -> `SHARED_SOURCE_MODE_MISMATCH`
4. `kSharedSourceWhereMismatch` -> `SHARED_SOURCE_WHERE_MISMATCH`
5. `kSharedSourceFilterUnsupported` -> `SHARED_SOURCE_FILTER_UNSUPPORTED`
6. `kSharedSourceReadyTimeout` -> `SHARED_SOURCE_READY_TIMEOUT`
7. `kSharedSourceInternalError` -> `SHARED_SOURCE_INTERNAL_ERROR`

使用阶段：

1. hub 创建/复用失败：`lease`。
2. 订阅失败：`source_resolve` 或 `execute`。
3. `where_signature` 不一致：`source_resolve` 或 `lease`。
4. 上游 `SetFilter` 绑定失败或不支持：`execute`。
5. fixed ready 屏障超时：沿用 group 语义错误码或映射新码（本迭代保持 `STREAM_GROUP_SHARE_SET_READY_TIMEOUT` 对外一致）。

---

## 配置项设计

新增配置项（Scheduler Option）：

1. `max_shared_hubs`（默认 4096）
2. `max_subscribers_per_hub`（默认 128）
3. `shared_subscriber_queue_size`（默认 2048）
4. `shared_hub_poll_timeout_ms`（默认 50）
5. `shared_hub_drop_policy`（默认 `drop`）

约束：

1. 超限返回明确错误码。
2. 配置非法时启动失败（fail-fast）。

---

## 实施阶段（同一迭代）

### 阶段1：新增 `SharedSourceHub`（并存）

1. 完成 dynamic 模式。
2. single task 接入跨任务共享 source。
3. 保持 group 仍使用 BroadcastHub。
4. 完成跨任务 late join 相关测试。

### 阶段2：融合与收敛（同迭代）

1. group share set 切换到 fixed 模式 SharedSourceHub。
2. 删除 BroadcastHub 运行态装配与调用。
3. 清理 `broadcast_hub.*` 与相关冗余。
4. 完成等价性回归与压力回归。

约束：

1. 阶段1提交仅作为中间开发里程碑，不作为迭代最终形态。
2. 迭代收口时代码库中不再保留 `BroadcastHub` 运行态依赖。

---

## 测试设计（充分覆盖）

## A. 单元测试

新增：`src/tests/test_scheduler_shared_source_hub/test_scheduler_shared_source_hub.cpp`

用例组：

1. `dynamic_join_basic`：先有订阅者，再 late join，第二订阅者仅接收 join 后新批次。
2. `dynamic_isolation_stop_one`：移除一个订阅者不影响另一个。
3. `dynamic_backpressure_drop`：慢订阅者产生 drop，快订阅者持续消费。
4. `dynamic_where_signature_match`：同签名订阅可附着共享 hub。
5. `dynamic_where_signature_mismatch`：异签名订阅显式拒绝并返回固定错误码。
6. `fixed_ready_gate`：未 ready 前不得开始 source read。
7. `fixed_coordinated_drop`：任一成员满时全员一致 drop。
8. `source_error_propagation`：source error 后 subscriber 全部收到终止信号。
9. `subscriber_raii_cleanup`：异常路径句柄析构自动退订。

## B. Scheduler E2E

扩展：`test_scheduler_e2e.cpp`

新增场景建议编号：

1. `T60`：同源双任务并发消费（原本冲突，现应成功）。
2. `T61`：late join 语义验证（批次序列断点验证）。
3. `T62`：单任务 stop 隔离性（另一任务吞吐不中断）。
4. `T63`：group share set 迁移后基础功能回归（替代原 T49）。
5. `T64`：group share set 高压 drop 一致性回归（替代原 T49.1）。
6. `T65`：group ready timeout 与错误码回归。
7. `T66`：同源同 `WHERE` 签名跨任务共享成功。
8. `T67`：同源异 `WHERE` 签名跨任务共享失败（固定错误码 + 无静默降级）。

## C. 并发与竞态

扩展：`test_scheduler_mutation_guard.cpp`

1. `execute` 与 `modify/remove` 并发，in_use 稳定。
2. 同源并发 execute 仅创建一个 hub（幂等）。
3. stop/status/list 并发读写无崩溃、无死锁。
4. retention 清理与 stop 并发幂等。

## D. TaskPlugin 侧

扩展：`test_task.cpp`

1. `/tasks/stream/status` 返回共享拓扑字段。
2. `/tasks/stream/list` 返回 subscriber 统计摘要。
3. runtime error/status 映射保持一致。

## E. 非回归清单

必须全量通过：

1. `test_task`
2. `test_scheduler_e2e`
3. `test_scheduler_mutation_guard`
4. `test_framework`（解析器与契约回归）
5. 前端构建 `npm --prefix src/frontend run build`

## F. 测试执行矩阵

构建与执行命令：

```bash
cmake --build build -j4 --target flowsql test_task test_scheduler_e2e test_scheduler_mutation_guard test_framework
ctest --test-dir build -R test_task --output-on-failure
ctest --test-dir build -R test_scheduler_e2e --output-on-failure
ctest --test-dir build -R test_scheduler_mutation_guard --output-on-failure
ctest --test-dir build -R test_framework --output-on-failure
npm --prefix src/frontend run build
```

通过判据：

1. 所有新增/回归测试通过，无 flaky 重试依赖。
2. `STREAM_SOURCE_IN_USE` 在同源跨任务并发场景不再误报。
3. Group share set 迁移后关键统计恒等式不变。
4. `WHERE` 签名不一致场景稳定返回 `SHARED_SOURCE_WHERE_MISMATCH`，不存在静默过滤失效。
5. ASAN/UBSAN（若启用）无新增崩溃与未定义行为告警。

---

## 验收准则映射

1. Story 14.15 的 5 条验收标准全部映射到 E2E 场景与状态字段断言。
2. 规划文档 T1-T11 每项至少有一条测试或契约检查对应。
3. `BroadcastHub` 删除后，原 share set 指标语义不变：
   1. `input_batches == delivered_batches + dropped_batches_shared`
   2. `input_rows == delivered_rows + dropped_rows_shared`
   3. group stop/timeout/cancel 状态语义不退化。

### 阶段收口 Gate（Go/No-Go）

1. **功能门槛**：同源双任务并发消费、late join、单任务 stop 隔离三项必须全部通过。
2. **语义门槛**：Group share set 迁移后，ready 屏障、coordinated drop、timeout 错误码语义全部保持一致。
3. **并发门槛**：`execute` 与 `modify/remove` 并发压测稳定，无 TOCTOU 误判与资源残留。
4. **性能门槛**：固定成员广播场景（原 BroadcastHub 等价场景）吞吐回退不得超过预设阈值（建议 5%）；口径固定为同机同配置下 `T64` 3 次运行中位值（rows/s 或 batches/s）。
5. **收口规则**：任一门槛未通过，不得进行阶段2最终收口（删除旧路径）；需继续优化，若短期无法达标则暂停并评审。

---

## 风险评估与缓解

1. **风险：共享引用泄漏导致 hub 常驻**
   1. 缓解：RAII 句柄 + retention 二次兜底 + 指标告警。
2. **风险：锁顺序错误导致死锁**
   1. 缓解：统一锁序写入代码注释与 review checklist，新增死锁压力测试。
3. **风险：阶段2迁移导致 group 语义回归**
   1. 缓解：保留等价性断言集（T49/T49.1 对应迁移测试）。
4. **风险：慢消费者拖垮系统**
   1. 缓解：默认 drop + 队列上限 + 可观测指标。

---

## 未来演进空间

1. 在 `SharedSourceHub` 上增加可选 cursor/offset 模型，支撑历史回放专题。
2. 增加租户级 QoS 与配额（hub 数、订阅者数、吞吐限制）。
3. 外置 hub 元数据到协调服务，支持跨节点共享（分布式版本）。
