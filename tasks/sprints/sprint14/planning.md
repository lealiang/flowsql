# Sprint 14 规划

## Sprint 信息

- **Sprint 周期**：Sprint 14
- **开始日期**：2026-04-05
- **预计工作量**：8 天
- **Sprint 目标**：围绕流式架构的可扩展性与可运维性，完成内置能力统一加载、具名 Stream Sink 产品化，以及流式 Group DAG 编排能力（同源并发 + 链式串行 + 串并组合）。

---

## 迭代边界（冻结）

### 本迭代范围（In Scope）

1. **Story 14.11**：内置通道/算子统一加载（去硬编码）
2. **Story 14.12**：Stream 通道具名创建与 Sink 通道产品化
3. **Story 14.13**：流式 Group DAG 编排（同源并发 + 链式串行 + 串并组合）
4. **测试与文档**：补齐并发正确性、稳定性、管理面联调与回归文档

### 非本迭代范围（Out of Scope）

1. 跨任务共享 source 的动态订阅（late join）
2. 广播链路的数据回放与持久化重放
3. 路径 B 数据面能力（DPDK 与 block_stream 实现）
4. 多主机分布式流任务编排

### 关键约束

1. 项目仍处于构建阶段，不引入 V1/V2 双轨兼容策略。
2. 保持语义清晰：显式创建、显式绑定、显式报错，不做隐式兜底。
3. Sprint 14 的 Group DAG 仅覆盖“单任务内编排”，跨任务共享消费另列后续专题。

---

## Sprint 目标与成功标准

### 主要目标

1. 去除内置通道与内置算子的核心硬编码装配逻辑，建立统一注册与加载机制。
2. 将具名 Stream 通道从“可用”提升为“可管理、可观测、可约束”的产品化能力。
3. 在单任务多 SQL 场景下支持 Group DAG 编排，覆盖串行、并发与串并组合场景。

### 成功标准

- [x] `StreamPlugin` 通道构建通过统一注册表分发，不再内置 `if(type==...)` 选择分支。
- [x] `CatalogPlugin` 内置算子注册改为统一注册表驱动，新增内置项无需修改核心注册流程。
- [ ] Stream 通道管理接口返回字段可支撑 sink 运维（模式、容量、占用、状态）。
- [ ] 支持单任务多 SQL 的 Group DAG 执行，覆盖串行、并发与组合拓扑。
- [ ] 慢分支与异常分支行为可观测、可停止、可定位。
- [ ] 自动化测试覆盖并通过：`test_stream`、`test_scheduler_e2e`、Web 前端构建与关键联调回归。

---

## Story 列表

### Story 14.11：内置通道/算子统一加载（去硬编码）

**优先级**：P0  
**工作量估算**：3 天  
**依赖**：无

**验收标准**：
- [x] 建立统一 `BuiltinRegistry`（或等价机制）承载「通道类型构建器 + 算子工厂」注册。
- [x] `StreamPlugin` 从注册中心解析并构建流式通道，支持类型扩展与标准化错误返回。
- [x] `CatalogPlugin` 通过注册中心批量注册内置算子（含 `builtin.*` 别名策略）。
- [x] `StreamChannelTypeDescriptor` 支持参数 schema、角色能力与校验/规范化函数。
- [x] 补齐重复注册、缺失注册、非法配置的失败路径测试。

**任务分解**：
- [x] T1：定义通用内置注册接口与注册数据结构（通道/算子双域）。
- [x] T2：改造 `StreamPlugin::BuildOneChannelLocked` 为注册中心分发。
- [x] T3：改造 `CatalogPlugin::Load` 内置算子注册流程为注册中心驱动。
- [x] T4：补齐 `StreamChannelTypeDescriptor`（option schema/role/校验器）并落地 `ring/tcp_session_mock/stream_hub` 类型注册。
- [x] T5：补齐内置项注册冲突与构建失败的日志、错误码与回归测试。

---

### Story 14.12：Stream 通道具名创建与 Sink 通道产品化

**优先级**：P1  
**工作量估算**：2 天  
**依赖**：Story 14.11（建议）

**验收标准**：
- [ ] 具名 Stream 通道可显式创建并作为 `INTO stream.<name>` sink 使用。
- [ ] `query` 返回关键运行态字段：容量、模式、状态、占用标记与必要统计。
- [ ] 明确并落地通道角色校验（source/sink/both）与错误信息。
- [ ] 新增 `POST /channels/stream/types/query`，前端按后端类型 schema 动态渲染参数表单。
- [ ] `stream_hub(split)` 支持 source selector：`stream.<hub>`、`stream.<hub>[*]`、`stream.<hub>[i]`。
- [ ] `stream_hub(merge)` 仅支持 root source：`stream.<hub>`；`[*]/[i]` 明确报错。
- [ ] 当 source 为 split hub 且未指定 selector 时，后端自动等价 `[*]` 展开，并在状态中返回 `resolved_sources`。
- [ ] `INTO stream.<hub>` 在 `split/merge` 下都可写 root；`INTO stream.<hub>[*|i]` 一律报错。
- [ ] `modify/remove` 与运行态并发冲突处理可复现、可观测。

**任务分解**：
- [ ] T6：梳理并固化 Stream 通道管理契约（结构化 options、role、状态与观测字段）。
- [ ] T7：新增 `/channels/stream/types/query` 与 `/api/channels/stream/types/query` 代理。
- [ ] T8：扩展 `/channels/stream/query` 响应字段并补齐角色校验、错误码映射。
- [ ] T9：前端 Stream 通道新增/修改改为动态 schema 表单（含 `spsc/spmc/mpsc/mpmc`、`stream_hub` 参数）。
- [ ] T10：实现 `split/merge` 的 source selector 语义与自动 `[*]` 展开（仅 split 生效），并补齐 `INTO` selector 禁止规则与端到端回归测试。
- [ ] T10.1：`ClassifySqlTaskKind` 与 `ExecuteStreamTask` 复用统一 source 解析函数，保证分类/执行一致性。
- [ ] T10.2：SQL parser 采用最小侵入方案支持 source/dest selector，并补齐历史 SQL 回归与非法 selector 用例。

---

### Story 14.13：流式 Group DAG 编排（同源并发 + 链式串行 + 串并组合）

**优先级**：P0  
**工作量估算**：3 天  
**依赖**：无

**验收标准**：
- [ ] 复用 `POST /tasks/stream/execute` 与 `POST /scheduler/stream/execute`，通过 `execution_kind/group_mode` 显式区分 single 与 group。
- [ ] `group_mode=dag` 支持三类拓扑：串行链式、同源广播并发、串并组合。
- [ ] 同源广播分支满足集合一致性（source 读一次；允许全分支一致 drop，禁止分支间不一致可见）。
- [ ] DAG 校验完整（无环、依赖合法、share set 合法），失败路径错误码清晰。
- [ ] 慢分支/故障分支策略明确：背压、fail-fast 收敛、错误透传。
- [ ] `timeout_s` 与 `share_set_ready_timeout_s` 语义明确，超时路径可收敛且错误码可观测。
- [ ] `execute` 与 `modify/remove` 并发下无 TOCTOU 误判（版本校验 + 引用登记原子）。
- [ ] 组级可观测性可用（节点状态、失败节点、share set 摘要）。
- [ ] 前端工作台支持流式 group DAG 提交与状态展示。

**任务分解**：
- [ ] T11：定义 `StreamTaskGroup + DagNodeRuntime` 数据结构与状态机（group/node 两级）。
- [ ] T12：在 `/scheduler/stream/execute` 增加 `execution_kind` 分发与 `group_mode=dag` 校验。
- [ ] T13：实现 DAG 归一化与校验（ID 唯一、依赖合法、无环、share set 合法）。
- [ ] T13.1：补齐 DAG 护栏校验（`max_group_nodes/max_group_edges/max_group_share_sets/max_group_sql_bytes`）与错误码 `STREAM_GROUP_DAG_TOO_LARGE`。
- [ ] T14：实现 `source_share_sets` 与 `BroadcastHub`（同源一次读取、多分支广播）。
- [ ] T14.1：实现 `coordinated drop`（全分支一致丢弃）与 `broadcast_seq` 指标。
- [ ] T15：实现 DAG 调度推进（拓扑启动、`on_running/on_finished`、share set 全员 ready 启动屏障）。
- [ ] T15.1：实现 share set 成员 `start_condition=on_running` 限制与 `share_set_ready_timeout_s` 屏障超时。
- [ ] T16：实现组级 stop/status/list、租约申请/释放与 fail-fast 收敛。
- [ ] T16.1：实现 `timeout_s` 组级超时收敛与 `STREAM_GROUP_TIMEOUT` 错误透传。
- [ ] T16.2：实现 `execute` 与 `modify/remove` 的 TOCTOU 原子防护（版本复核 + 引用登记 + 失败回滚）。
- [ ] T17：补齐 `TaskPlugin` 与前端工作台请求契约（`execution_kind/group_mode/dag`）及可视化展示。
- [ ] T18：补齐 DAG 回归测试（串行、并行、组合、环路校验、异常收敛）。
- [ ] T18.1：补齐高压丢弃一致性测试（`delivered/dropped/seq` 跨分支一致）。

---

## 测试与验证

**核心验证命令**：

```bash
cmake --build build -j4 --target flowsql test_stream test_scheduler_e2e
ctest --test-dir build -R test_stream --output-on-failure
ctest --test-dir build -R test_scheduler_e2e --output-on-failure
npm --prefix src/frontend run build
```

**新增测试范围**：

- [ ] `test_stream`：DAG 节点调度、广播分发正确性、慢分支稳定性、Stop/Cancel/Timeout 收敛。
- [ ] `test_scheduler_e2e`：单任务多 SQL 的 DAG 串并组合全链路回归。
- [ ] 管理面回归：`/channels/stream/query|add|modify|remove` 与 sink 绑定校验（含 `stream_hub split/merge` 语义）。
- [ ] 前端冒烟：具名 Stream Sink 创建、查询、任务执行与状态展示。
- [ ] SQL parser/语义回归：历史语法 AST 稳定 + selector 新增/非法语法用例 + `merge` 语义拒绝用例。
- [ ] 并发回归：`execute` 与 `modify/remove` 并发压测，验证 TOCTOU 防护与 in-use 判定稳定。

---

## 实施顺序

```text
Day 1-3: Story 14.11（统一注册与加载机制）
Day 4-5: Story 14.12（具名 Stream Sink 产品化）
Day 6-8: Story 14.13（Group DAG 编排 + 测试回归）
```

---

## 风险与缓解

| 风险 | 可能性 | 缓解措施 |
|------|--------|---------|
| 去硬编码改造影响既有内置能力加载 | 中 | 先引入注册中心，再逐步切换调用点，保留回归用例 |
| 广播分发在慢分支下出现背压放大 | 高 | 明确背压策略与组级失败策略，增加滞后监控 |
| DAG 编排引入状态机复杂度，导致 stop/cancel 不一致 | 中 | 统一 group/node/task 状态机，并在 E2E 中覆盖异常路径 |
| 管理面字段扩展引起前后端契约不一致 | 中 | 先冻结 JSON 契约，再联动实现与回归 |

---

## 交付物清单

1. Sprint 14 计划文档：`tasks/sprints/sprint14/planning.md`
2. Story 14.11 设计与实现：内置通道/算子统一注册加载
3. Story 14.12 设计与实现：具名 Stream Sink 管理与观测
4. Story 14.13 设计与实现：流式 Group DAG 执行链路
5. 测试与回归报告：并发正确性、稳定性、管理面与前端联调
