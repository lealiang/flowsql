# Sprint 16 规划

## Sprint 信息

- **Sprint 周期**：Sprint 16
- **开始日期**：2026-04-09
- **预计工作量**：8 天（评估区间 7-10 天）
- **Sprint 目标**：落地 Story 14.15「跨任务共享 source（late join）」，让多个 stream 任务可并发消费同一 source，并保证任务隔离、背压可控、可观测可运维。
- **设计文档**：`tasks/sprints/sprint16/design.md`（本计划评审后补充）
- **状态**：✅ 已完成（Story 14.15 全部验收项达成）

---

## 迭代边界（冻结）

### 本迭代范围（In Scope）

1. **Story 14.15**：跨任务共享 source（late join）核心能力
2. 共享消费运行时：`SharedSourceHub`（同源单读、多任务分发）
3. source 租约模型改造：从“任务独占”升级为“Hub 持有 + 订阅引用计数”
4. 生命周期与隔离：单任务 stop/cancel/fail 不影响其他订阅者
5. 管理面可观测：共享拓扑、消费者数量、每消费者统计
6. `BroadcastHub` 融合进 `SharedSourceHub`，统一共享分发运行时概念
7. 自动化测试与回归文档

### 非本迭代范围（Out of Scope）

1. Story 14.16（历史数据批处理补算）
2. 广播回放/持久化重放（历史回放能力）
3. 跨主机分布式共享 source
4. batch + stream 混合 DAG（Story 14.14）

### 关键约束

1. 保持 SQL 语法不变，不新增 `FROM/INTO` 语法分支。
2. 保持执行入口不变：继续复用 `/tasks/stream/execute` 与 `/scheduler/stream/execute`。
3. 不引入兼容双轨（V1/V2）；按新模型统一实现。
4. 不破坏 Sprint 14 的单任务 Group DAG 语义；14.13 与 14.15 能并存。

---

## Sprint 目标与成功标准

### 主要目标

1. 支持多个 stream 任务并发消费同一 source，不再因 source 独占租约冲突失败。
2. 支持 late join：新任务加入后从加入时刻开始消费，不补历史。
3. 保证消费隔离：任何单任务终止不影响同源其他任务。
4. 在慢消费者场景下保证全局可用性：默认不阻塞共享 source reader。

### 成功标准

- [x] 同一 source 上两个及以上 stream 任务可同时 `submitted/running`。
- [x] 第二个任务在第一个任务运行中启动，能稳定从 join 时刻开始收到新批次数据。
- [x] 停止任一任务后，其余任务持续运行且数据链路不中断。
- [x] 共享链路背压策略可配置且可观测（至少含 `dropped/lag/subscriber_count`）。
- [x] `stream status/list` 能返回共享消费拓扑信息（hub、消费者、状态与关键统计）。
- [x] `modify/remove` 对共享 source 的 `in_use` 判定准确，无 TOCTOU 漏检。
- [x] 单任务 Group DAG 的 share set 路径改为复用 `SharedSourceHub`（固定成员模式），不再依赖 `BroadcastHub`。
- [x] `BroadcastHub` 代码下线后，14.13 功能回归无退化。
- [x] 回归测试通过：`test_scheduler_e2e`、`test_scheduler_mutation_guard`、`test_task`。

---

## Story 列表

### Story 14.15：跨任务共享 source（late join）

**优先级**：P0  
**工作量估算**：8 天（核心改造 + 回归）  
**依赖**：Story 14.13（单任务 Group DAG 已完成）

**验收标准**：

- [x] 支持多个 stream 任务并行消费同一 source，不再要求 source 独占租约。
- [x] 支持 `late join`：新任务在既有任务运行中加入后，从加入时刻开始消费（不补历史）。
- [x] 消费隔离：单任务 `stop/cancel/fail` 不影响其他共享消费者。
- [x] 背压策略可配置且可观测：慢消费者不阻塞全局，关键指标（丢弃/滞后/吞吐）可查询。
- [x] 管理面与状态接口返回共享消费拓扑信息（共享组、消费者数、每消费者状态）。

**任务分解**：

- [x] **T1：冻结运行时模型与并发约束**
  - 明确 `SharedSourceHub` 数据结构：`hub_key/source_keys/subscribers/metrics/status`。
  - 明确锁顺序与生命周期：`shared_hubs_mu_ -> stream_channel_refs_mu_ -> stream_tasks_mu_`。
  - 明确首版背压策略：默认 `drop`，并预留 `detach_on_slow` 扩展点。
  - 冻结 `WHERE` 约束：共享 hub 仅允许同一 `where_signature` 订阅，签名不一致必须显式报错。

- [x] **T2：实现 SharedSourceHub 核心组件**
  - 新增 `src/services/scheduler/shared_source_hub.h/.cpp`。
  - 支持 `AddSubscriber/RemoveSubscriber/Start/RequestStop/Join/Snapshot`。
  - 支持 late join（运行中动态添加订阅者）。
  - 支持 `where_signature` 一致性校验与上游 `SetFilter` 一次性绑定（仅首订阅生效）。

- [x] **T3：Scheduler source 解析接入共享消费装配**
  - 在 `ResolveSourceBindings` 与 `BuildStreamExecutionPlan` 路径接入共享装配。
  - 对同 canonical source keys 的任务复用同一 hub；为每任务注入独立 subscriber 入口通道。
  - 与 `stream_hub(split/merge)` 现有 selector 语义保持一致。

- [x] **T4：source 租约模型改造（独占 -> 共享）**
  - 调整 `TryAcquireStreamTaskLeases/ReleaseStreamTaskLeases`：source 侧改为 hub-owner + subscriber 引用计数。
  - 保持 sink 租约语义不变。
  - `modify/remove` 仍基于 in-use 严格拦截共享 source。

- [x] **T5：任务终止与资源回收**
  - `single/group` 任务终止时移除订阅者并释放任务级资源。
  - 最后一个订阅者离开后关闭并回收 hub。
  - 对异常路径补齐兜底回收，避免 hub 泄漏或僵尸订阅。

- [x] **T6：管理面与状态面可观测增强**
  - 扩展 `stream status/list` 输出共享消费拓扑字段：
    - `shared_hub_id`
    - `shared_source_keys`
    - `subscriber_count`
    - `subscriber_stats`（dropped/lag/delivered）
  - 前后端错误码与错误阶段保持统一（`error/error_code/error_stage`）。
  - 完成结果：`shared_hub_id/shared_source_keys/subscriber_count/subscriber_stats(dropped+delivered+lag)` 已在 scheduler status/list 输出

- [x] **T7：TaskPlugin 与前端状态对齐**
  - `TaskPlugin` 透传共享拓扑关键字段至 `/tasks/stream/status|list`。
  - 前端任务详情页展示“共享 source 订阅信息”（只读观测）。

- [x] **T8：配置项与护栏**
  - 增加可配置护栏：`max_shared_hubs`、`max_subscribers_per_hub`、`shared_subscriber_queue_size`。
  - 增加共享装配失败快返与异常保护，避免静默降级与无界重试。

- [x] **T9：测试补齐（功能 + 并发 + 回归）**
  - E2E：同源双任务并发消费、late join、单任务停止隔离。
  - 并发：`execute` 与 `modify/remove` 并发竞争下的 in-use 与 TOCTOU。
  - 稳定性：慢消费者压测下不阻塞全局 reader。
  - 语义：`WHERE` 同签名可共享、异签名显式拒绝（无静默过滤失效）。

- [x] **T10：文档与验收收口**
  - 更新 `README.md` 的能力矩阵（跨任务共享 source 能力与限制）。
  - 同步 `product_backlog.md` / `sprint16/planning.md` 状态与验收结果。

- [x] **T11：阶段2融合（BroadcastHub -> SharedSourceHub）**
  - 将 Group DAG share set 广播实现切换到 `SharedSourceHub` 的固定成员模式（非 late join）。
  - 删除 `BroadcastHub` 的运行态装配与调用路径，统一共享分发入口。
  - 清理 `BroadcastHub` 相关冗余代码与测试夹具，补齐等价性回归用例。

---

## 测试与验证

**核心验证命令**：

```bash
cmake --build build -j4 --target flowsql test_task test_scheduler_e2e test_scheduler_mutation_guard
ctest --test-dir build -R test_task --output-on-failure
ctest --test-dir build -R test_scheduler_e2e --output-on-failure
ctest --test-dir build -R test_scheduler_mutation_guard --output-on-failure
npm --prefix src/frontend run build
```

**新增测试范围**：

- [x] 同源双任务并发消费成功（不再报 `STREAM_SOURCE_IN_USE`）。
- [x] late join 从加入时刻生效（不补历史）。
- [x] stop/cancel/fail 隔离：单任务终止不影响其他订阅者。
- [x] 慢消费者背压：全局 reader 不阻塞，慢任务可观测到丢弃/滞后。
- [x] `modify/remove` 与 execute 并发下无 TOCTOU 漏洞。
- [x] Group DAG（14.13）在迁移到 `SharedSourceHub` 后回归不退化。
- [x] Group share set 与原实现的结果等价性验证通过（关键计数与状态机一致）。

---

## 实施顺序

```text
Day 1: T1（模型冻结）
Day 2-3: T2（SharedSourceHub）
Day 4: T3 + T4（调度装配与租约改造）
Day 5: T5 + T6（生命周期与可观测）
Day 6: T7 + T8（TaskPlugin/前端与护栏）
Day 7: T11（BroadcastHub 融合与代码收敛）
Day 8: T9 + T10（测试回归与文档收口）
```

---

## 风险与缓解

| 风险 | 可能性 | 缓解措施 |
|------|--------|---------|
| 共享 hub 与任务回收并发导致资源泄漏 | 中 | 统一锁顺序 + RAII 订阅句柄 + 终态双重回收 |
| 慢消费者导致内存膨胀 | 高 | subscriber 队列上限 + 默认 drop + 指标告警 |
| 任务 stop 与 hub stop 顺序错误导致误停 | 中 | 固化停止序列：先退订，再判空停 hub |
| source 共享改造影响既有 14.13 语义 | 中 | 分层改造 + 14.13 全量回归用例 |
| 状态字段扩展引起前后端契约漂移 | 中 | 冻结 JSON 字段并加契约测试 |

---

## 交付物清单

1. Sprint 16 计划文档：`tasks/sprints/sprint16/planning.md`
2. Story 14.15 设计文档：`tasks/sprints/sprint16/design.md`
3. SharedSourceHub 实现与调度接入代码
4. 自动化测试与回归报告
5. README 能力矩阵同步

---

## 后续候选（不纳入 Sprint 16）

- [ ] Story 14.16：历史数据批处理补算
- [ ] 共享 source 持久化重放（若未来需要在线回放再独立立项）
- [ ] 跨主机共享 source（分布式调度）
