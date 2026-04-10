# Sprint 17 规划

## Sprint 信息

- **Sprint 周期**：Sprint 17
- **开始日期**：2026-04-10
- **预计工作量**：18.3 天（评估区间 17-19 天）
- **Sprint 目标**：在 Story 14.14 完成基础上，补齐 `HandleStreamExecuteGroup` 分层重构，降低调度核心复杂度并保持契约零回退。
- **设计文档**：`tasks/sprints/sprint17/design.md`
- **状态**：已完成（原基线任务 + 增补任务 R1 均已完成）

---

## 迭代边界（冻结）

### 本迭代范围（In Scope）

1. Gate-0：batch 异步 worker 线程池迁移到 Scheduler（前置门槛）
2. 复用现有执行入口：`/tasks/stream/execute`、`/scheduler/stream/execute`
3. `execution_kind=group` + `group_mode=dag` 支持 mixed 节点（batch/stream）
4. 7 条路径全部纳入 MVP（不做白名单裁剪）
5. Scheduler 新增 batch 节点运行时并接入 group 调度
6. `status/list` 输出 node 级扩展字段与结构化错误
7. TaskPlugin mixed 提交流程打通
8. 前端混合任务提交与 DAG 结果展示打通
9. 自动化测试覆盖：单元 + 集成 + E2E + 并发稳定性
10. `HandleStreamExecuteGroup` 分层重构（请求/规划/运行时/回滚分离）

### 非本迭代范围（Out of Scope）

1. 新增 SQL 语法
2. 新增执行 URI
3. 跨任务 mixed DAG 编排
4. L2 策略：`on_terminal`、`partial_finalize`、`continue_on_failure`
5. `dataframe` 作为流式主干通道

### 关键约束

1. 多 SQL 仅支持分号 `;` 切分，换行只作排版。
2. 违反约束必须显式失败，并返回 `error_code/error_message/sql_index`。
3. 非 stream sink 并发写保持本轮限制：同一 sink_key 单 writer。
4. 设计与实现保持 0-based `sql_index` 统一口径。

---

## Story 列表

### Story 14.14：Hybrid DAG（batch + stream 混合编排）

**优先级**：P0  
**工作量估算**：11.0 天（不含 Gate-0）  
**依赖**：Sprint 14/16 的 stream group 与共享分发能力已完成 + Gate-0 完成

**验收标准**：

- [x] 7 条路径在同一版本全部可执行。
- [x] 执行契约与入口约束生效：`group_mode=dag`、`statement_count>=2`、single 仅允许单条 stream SQL。
- [x] mixed group 支持 DAG 自动构建、校验、调度与 stop/timeout 收敛。
- [x] `batch -> stream` 生命周期满足三条件回收（producer_done + consumer_done + ref_count==0），无过早释放。
- [x] 组级租约并集申请与节点复用租约生效（`skip_lease_acquire=true`），异常路径无租约残留。
- [x] share set 采用 fixed + `coordinated_drop`，`share_set_ready_timeout_s` 超时可观测且可收敛。
- [x] 构建期/运行期错误可定位到 `node_id + sql_index + phase`。
- [x] 新增错误码落地并可见：`STREAM_GROUP_NON_STREAM_SINK_MULTI_WRITER`、`STREAM_GROUP_NODE_KIND_INVALID`、`STREAM_GROUP_NODE_EXECUTION_FAILED`。
- [x] `status/list` 返回 `node_kind/sql_index/phase/error_*`，前端可直接展示。
- [x] 现有 stream-only group 能力无回退。

---

## Gate-0 前置重构任务（必须先完成）

> 目标：将 batch 异步执行线程池从 TaskPlugin 迁移到 Scheduler，避免 Story 14.14 落地时出现双执行引擎。

**验收标准**：

- [x] TaskPlugin 不再持有 batch 执行队列与 worker 线程。
- [x] Scheduler 提供 batch submit/status/stop 内部控制能力。
- [x] `/tasks/batch/execute` 对外契约保持不变（sync/async 行为不回退）。
- [x] batch async 任务状态可从 Scheduler runtime 同步回 TaskPlugin 存储。
- [x] 取消语义不回退（语句边界收敛，不强杀执行中 SQL）。

| 任务 | 状态 | 内容 | 主要文件 | 估算（PD） | 依赖 | 输出/验收点 |
|---|---|---|---|---:|---|---|
| G1 | 已完成 | Scheduler batch runtime 模型与 worker 池落地 | `scheduler_batch_runtime.h/.cpp`（新增）、`scheduler_plugin.h/.cpp` | 1.6 | - | batch runtime 可 submit/status/stop，具备基础快照 |
| G2 | 已完成 | Scheduler 路由与控制接口扩展 | `scheduler_routes.cpp`、`ischeduler_control_service.h`、`scheduler_control_client.h/.cpp` | 0.8 | G1 | 内部控制面可调用 batch submit/status/stop |
| G3 | 已完成 | TaskPlugin 去 worker 化与提交改造 | `task_plugin.h/.cpp` | 1.4 | G2 | 移除 batch queue/worker，async 改为提交 Scheduler runtime |
| G4 | 已完成 | 状态回填与取消链路打通 | `task_plugin.cpp`、相关存储读写路径 | 0.9 | G3 | list/detail/result/cancel 与 Scheduler runtime 一致 |
| G5 | 已完成 | Gate-0 回归测试 | `test_task.cpp`、`test_scheduler_e2e.cpp` | 0.8 | G1-G4 | batch async 生命周期、取消、错误码回归通过 |

**Gate-0 合计**：5.5 PD

---

## Story 14.14 任务分解与时间估计

> 说明：以下估算按“单人日（PD）”计算，包含编码 + 自测，不含外部阻塞等待。

| 任务 | 状态 | 内容 | 主要文件 | 估算（PD） | 依赖 | 输出/验收点 |
|---|---|---|---|---:|---|---|
| T1 | 已完成 | 混合节点识别与 DAG 规划改造（kind 判定、依赖构建、拓扑校验、执行契约护栏） | `scheduler_stream_group.cpp` | 1.7 | - | mixed SQL 可构建 `GroupNodePlan`；`group_mode=dag`、`statement_count>=2`、single 限制校验生效 |
| T2 | 已完成 | 启动条件推导与约束校验（node start_condition、non-stream 单 writer、share set 扩展） | `scheduler_stream_group.cpp` | 1.1 | T1 | 校验失败可返回结构化错误（含 `sql_index`）；share set fixed + `coordinated_drop` 生效 |
| T3 | 已完成 | batch 节点运行时实现（异步执行、快照查询、stop 语义） | `scheduler_batch_node_runtime.h/.cpp`（新增） | 1.5 | T1 | `kBatch` 节点可被 group 调度并产出运行态快照 |
| T4 | 已完成 | StreamTaskGroup 回调链路扩展（submit/query/stop/fail-fast 融合 batch） | `stream_task_group.h/.cpp` | 1.0 | T3 | mixed 节点统一收敛到组级状态机；组级租约复用与 `skip_lease_acquire=true` 语义落地 |
| T5 | 已完成 | JSON 编解码与错误可见性增强（node 字段 + last_error 结构） | `scheduler_json_codec.cpp` | 0.8 | T4 | `status/list` 包含 `node_kind/sql_index/phase/error_*` |
| T6 | 已完成 | 运行态 retention 与资源回收（batch runtime 索引清理 + batch->stream 生命周期） | `scheduler_runtime_retention.cpp`、`scheduler_plugin.h` | 0.8 | T3 | 无悬挂 runtime 索引；三条件回收语义成立 |
| T7 | 已完成 | TaskPlugin mixed 提交流程打通与错误透传 | `task_plugin.cpp` | 0.8 | T1/T5 | `/tasks/stream/execute` 可提交 mixed group |
| T8 | 已完成 | 前端 mixed 提交与节点信息展示 | `Tasks.vue`、`api/index.js` | 1.0 | T7 | UI 可提交 mixed 多 SQL 并展示节点级状态 |
| T9 | 已完成 | 测试补齐（单元/集成/E2E） | `test_framework`、`test_task.cpp`、`test_scheduler_e2e.cpp` 等 | 1.7 | T1-T8 | 7 路径 + 组合拓扑 + 错误可见性 + SQL 切分/sql_index 用例通过 |
| T10 | 已完成 | 错误码与契约回归收口 | `error_contract.h/.cpp`、相关测试 | 0.6 | T5/T9 | 新增错误码生效并在 `execute/status/list` 可见 |
| T11 | 已完成 | 并发与稳定性回归（TOCTOU、stop/timeout、retention） | 相关测试与脚本 | 0.8 | T9/T10 | 高并发场景无回归、无明显资源泄漏 |

**合计**：11.0 PD

---

## 增补任务（独立任务）

> 说明：以下任务为 Sprint 17 新增重构项，独立于 Story 14.14 原始功能基线，用于收敛 `scheduler_stream_group.cpp` 的复杂度与回归风险。

| 任务 | 状态 | 内容 | 主要文件 | 估算（PD） | 依赖 | 输出/验收点 |
|---|---|---|---|---:|---|---|
| R1 | 已完成 | `SchedulerPlugin::HandleStreamExecuteGroup` 重构拆分（请求校验/规划/资源装配/运行编排分层） | `scheduler_stream_group.cpp`、`scheduler_stream_group_request.cpp`（新增）、`scheduler_stream_group_planner.cpp`（新增）、`scheduler_stream_group_runtime.cpp`（新增）、`scheduler_plugin.h` | 1.8 | T11 | 单函数行数降至 < 200；错误码与 JSON 契约零回退；现有 stream group 回归用例全通过 |

**Sprint 滚动总计**：18.3 PD（Gate-0 5.5 + Story 14.14 11.0 + 增补任务 R1 1.8）

---

## 时间排期（建议）

```text
Day 1-3:   G1 + G2（Scheduler batch runtime 与控制接口）
Day 4-5:   G3 + G4（TaskPlugin 去 worker 化与状态回填）
Day 6:     G5（Gate-0 回归）
Day 7-8:   T1 + T2（DAG 规划、契约护栏与校验）
Day 9-10:  T3 + T4（batch/runtime 与 group 状态机接入）
Day 11:    T5 + T6（可观测与回收）
Day 12:    T7（TaskPlugin mixed 提交流程）
Day 13:    T8（前端联动）
Day 14:    T9（测试补齐）
Day 15:    T10 + T11（错误码收口 + 并发稳定性）
Day 16:    风险缓冲（仅在出现阻塞时启用）
Day 17:    R1（`HandleStreamExecuteGroup` 重构拆分与回归）
```

---

## 里程碑与检查点

1. **M0（Day 6）**：Gate-0 完成，batch 异步执行引擎统一到 Scheduler。
2. **M1（Day 8）**：后端可生成 mixed DAG 计划并完成校验失败快返。
3. **M2（Day 10）**：mixed DAG 可执行，batch/stream 节点统一纳入 group 状态机。
4. **M3（Day 12）**：TaskPlugin mixed 提交流程打通，API 可端到端执行。
5. **M4（Day 13）**：前端提交与状态展示打通。
6. **M5（Day 15）**：自动化测试与并发回归通过，进入验收。
7. **M6（Day 17）**：`HandleStreamExecuteGroup` 重构完成，行为与契约零回退。

---

## 测试与验证计划

**核心验证命令（计划）**：

```bash
cmake --build build -j4 --target flowsql test_task test_scheduler_e2e test_scheduler_mutation_guard
ctest --test-dir build -R test_task --output-on-failure
ctest --test-dir build -R test_scheduler_e2e --output-on-failure
ctest --test-dir build -R test_scheduler_mutation_guard --output-on-failure
npm --prefix src/frontend run build
```

**新增测试覆盖点**：

- [x] 7 条路径逐条回归（含提交、执行、停止、状态查询）。
- [x] 组合 DAG 回归（串并混合 2 组以上拓扑）。
- [x] `test_framework`：多 SQL 分号切分、注释/字符串分号保护、`sql_index` 定位回归。
- [x] 构建期错误定位（`error_stage/error_code/sql_index`）。
- [x] 运行期节点失败定位（`phase/node_id/sql_index`）。
- [x] share set：`coordinated_drop` 一致性与 `share_set_ready_timeout_s` 超时收敛。
- [x] non-root stream 节点自动 share set（`n1 -> mid`, `n2/n3 <- mid`）回归通过。
- [x] 非 stream sink 多 writer 约束回归通过（返回 `STREAM_GROUP_NON_STREAM_SINK_MULTI_WRITER`）。
- [x] 执行契约护栏：`group_mode=dag`、`statement_count>=2`、single stream-only。
- [x] 组级租约并集 + 节点复用租约（`skip_lease_acquire=true`）语义回归。
- [x] 并发竞争（`execute` 与 `modify/remove`）无 TOCTOU 回归。
- [x] retention 清理后无 runtime 残留。
- [x] Gate-0：batch async 提交/查询/取消链路端到端通过，TaskPlugin 无本地 batch worker 依赖。
- [x] R1：重构后 `HandleStreamExecuteGroup` 行数 < 200，且现有 group 相关测试全绿。

**本轮验证结果（2026-04-10）**：

- [x] `cmake --build build --target test_scheduler_e2e -j4`
- [x] `cmake --build build --target test_scheduler_mutation_guard -j4`
- [x] `cmake --build build --target test_task -j4`
- [x] `./build/output/test_scheduler_e2e`（含 `T50.1/T53/T54/T54.1/T60/T61/T62/T66/T67/T68`）
- [x] `./build/output/test_scheduler_mutation_guard`
- [x] `./build/output/test_task`
- [x] `npm --prefix src/frontend run build`
- [x] `cmake --build build --target test_scheduler_e2e test_scheduler_mutation_guard -j4`
- [x] `./build/output/test_scheduler_e2e --gtest_filter=*T49*:*T54*:*T60*:*T61*:*T62*:*T66*:*T67*:*T68*`

---

## 风险与缓解

| 风险 | 可能性 | 影响 | 缓解措施 |
|---|---|---|---|
| `scheduler_stream_group.cpp` 复杂度继续膨胀 | 高 | 可维护性下降、回归风险上升 | 本轮引入 `BatchNodeRuntime` 独立文件，后续再拆 plan builder |
| batch 节点不可抢占 stop 导致收敛慢 | 中 | stop 体验不稳定 | 明确 `stopping -> terminal` 语义并提供超时可观测 |
| mixed 校验与既有 stream-only 行为冲突 | 中 | 老能力回退 | stream-only 回归用例作为必跑门禁 |
| 前后端字段口径不一致 | 中 | UI 信息缺失或误导 | 冻结 `status/list` 字段契约并做联调断言 |
| 测试矩阵不充分导致线上遗漏 | 中 | 隐性缺陷进入主干 | 7 路径 + 组合拓扑 + 并发稳定性三层必测 |

---

## 交付物清单

1. Sprint 17 设计文档：`tasks/sprints/sprint17/design.md`
2. Sprint 17 计划文档：`tasks/sprints/sprint17/planning.md`
3. Scheduler/TaskPlugin/Frontend 的 mixed DAG 实现代码
4. 自动化测试与回归结果
5. README 能力矩阵同步（若能力边界发生变化）
6. Gate-0 重构交付：batch 异步执行引擎下沉 Scheduler（含回归测试）
