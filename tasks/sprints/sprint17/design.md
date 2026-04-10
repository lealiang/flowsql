# Sprint 17 设计文档：Hybrid DAG（batch + stream 混合编排）

## 1. 背景与目标

Story 14.14 的核心不是“再支持几条固定路径”，而是提供单任务内 `batch/stream` 的可组合编排能力。Sprint 17 的目标是把能力边界落到可编码实现的工程方案。

本设计遵循以下原则：

1. 不新增 SQL 语法；Story 14.14 主链路不新增 stream 执行 URI；不引入 V1/V2 双轨。
2. 7 条路径全部纳入 MVP 实现，分层只针对策略增强，不针对路径支持范围。
3. 组合闭包是能力承诺（可构建、可校验、可调度），不是“任意拓扑都成功产出结果”的正确性承诺。
4. 违反约束必须显式失败，错误码与错误信息必须用户可见。

---

## 1.1 Gate-0（前置重构）：batch 异步 worker 下沉到 Scheduler

### 1.1.1 现状线程模型（需先收敛）

1. `TaskPlugin` 维护 batch 异步队列与 worker 线程，负责实际执行循环。
2. `TaskPlugin` worker 在每条 SQL 上调用 `scheduler_client.ExecuteBatch`。
3. `Scheduler /scheduler/batch/execute` 当前是同步执行路径，运行在调用线程上下文（非 Scheduler 专属 batch worker）。

结论：当前 batch 异步执行能力主要在 `TaskPlugin`，而不是 `Scheduler`。

### 1.1.2 前置重构目标

1. 将 batch 异步执行线程池迁移到 `Scheduler`，统一调度面执行引擎。
2. `TaskPlugin` 不再持有 batch 执行队列/worker，只保留任务编排、状态落库与 API 透传。
3. 保持外部 API 兼容（`/tasks/batch/execute` 入参与语义不变）。
4. 为 Story 14.14 的 `kBatch` 节点运行时提供可复用执行底座，避免“双执行引擎”。

### 1.1.3 重构后职责边界

1. `Scheduler`：
   1. 维护 batch runtime 与 batch worker 池。
   2. 提供 batch 作业提交/状态查询/停止能力。
   3. 负责 batch 作业生命周期与 runtime retention。
2. `TaskPlugin`：
   1. 创建任务记录并提交 Scheduler batch 作业。
   2. 在 list/detail/result 等读路径同步 Scheduler runtime 状态到任务表。
   3. 取消任务时转发 stop 请求到 Scheduler。

### 1.1.4 执行契约（Gate-0）

1. 保持 `/tasks/batch/execute` 对外契约不变。
2. `Scheduler` batch 控制面新增（内部管理面）：
   1. `POST /scheduler/batch/submit`
   2. `POST /scheduler/batch/status`
   3. `POST /scheduler/batch/stop`
3. `runtime_task_id` 与 `task_id` 默认一一对应（TaskPlugin 生成并传入），减少映射复杂度。
4. 停止语义与现有一致：不强杀正在执行 SQL，仅在语句边界收敛。

### 1.1.5 Gate-0 与 Story 14.14 的关系

1. Gate-0 是 Story 14.14 的前置门槛（必须先完成）。
2. Story 14.14 中 `BatchNodeRuntime` 直接复用 Scheduler batch worker/runtime，不再新建并行线程模型。

---

## 2. 本轮边界（最终确认）

### 2.1 MVP 必须覆盖的 7 条路径

| 路径 | 触发语义 | 价值/意义 | 典型场景 | SQL 样例 |
|---|---|---|---|---|
| `batch -> stream` | `batch` 写入完成并 `CloseStream()`，`stream` 从该通道持续消费到 EOF | 历史装填 + 流式处理 | 基线装填后做实时匹配 | `SELECT * FROM clickhouse.prod.tbl_rules INTO stream.rules_seed;`<br/>`SELECT * FROM stream.rules_seed USING builtin.rule_match_stream INTO dataframe.rule_hits;` |
| `stream -> batch` | `stream` 终态后触发（默认 `on_success`） | 流式筛选 + 批处理归档 | 流任务结束后生成统计表 | `SELECT * FROM tcp_session_mock.tcp_src USING builtin.tcp_service_merge_stream INTO dataframe.serviceaccess;`<br/>`SELECT app, COUNT(*) AS c FROM dataframe.serviceaccess GROUP BY app INTO mysql.prod.tbl_service_stat;` |
| `batch -> stream -> batch` | 第一段 `batch` 完成后触发 `stream`，`stream` 结束后触发第二段 `batch` | 准备-处理-归档闭环 | 基线装填后实时处理，再汇总落库 | `SELECT * FROM clickhouse.prod.tbl_baseline INTO stream.baseline_seed;`<br/>`SELECT * FROM stream.baseline_seed USING builtin.baseline_enrich_stream INTO dataframe.enriched;`<br/>`SELECT user_id, COUNT(*) AS cnt FROM dataframe.enriched GROUP BY user_id INTO mysql.prod.tbl_user_cnt;` |
| `stream ∥ stream -> batch` | 多路 `stream` 并发执行，汇聚后触发 `batch`（默认 `all_success`） | 多路并发采集后统一处理 | 多网卡并发采集后统一分析 | `SELECT * FROM tcp_session_mock.eth0_src USING builtin.tcp_service_merge_stream INTO dataframe.eth0_access;`<br/>`SELECT * FROM tcp_session_mock.eth1_src USING builtin.tcp_service_merge_stream INTO dataframe.eth1_access;`<br/>`SELECT * FROM dataframe.eth0_access, dataframe.eth1_access INTO clickhouse.prod.tbl_access_all;` |
| `batch -> stream ∥ stream` | `batch` 输出到下游通道，多个 `stream` 分支并发消费 | 一次装填，多路并行分析 | 同一输入驱动多条实时链路 | `SELECT * FROM mysql.prod.tbl_rules INTO stream.rules_hub;`<br/>`SELECT * FROM stream.rules_hub[0] USING builtin.rule_hit_stream INTO dataframe.rule_hit_a;`<br/>`SELECT * FROM stream.rules_hub[1] USING builtin.rule_hit_stream INTO dataframe.rule_hit_b;` |
| `batch ∥ batch -> stream` | 多个 `batch` 并发准备输入，汇聚后触发 `stream` | 多源并行准备后统一进入流处理 | 多张历史表并行装填后增量处理 | `SELECT * FROM clickhouse.prod.tbl_seed_a INTO stream.seed_a;`<br/>`SELECT * FROM clickhouse.prod.tbl_seed_b INTO stream.seed_b;`<br/>`SELECT * FROM stream.seed_a,stream.seed_b USING builtin.join_merge_stream INTO dataframe.joined_out;` |
| `stream -> stream`（边界路径） | 上游 `stream` 持续输出，下游 `stream` 持续消费 | 纯流 DAG，作为混合编排边界能力 | 流式清洗后继续流式聚合 | `SELECT * FROM tcp_session_mock.tcp_src USING builtin.tcp_service_merge_stream INTO stream.stage1;`<br/>`SELECT * FROM stream.stage1 USING builtin.session_agg_stream INTO dataframe.stage2_out;` |

说明：

1. 同一任务内多 SQL 必须使用分号 `;` 切分，换行仅用于排版。
2. 样例涉及 `stream.<hub>[i]` 时，前提是对应 `stream_hub(split)` 通道已定义。
3. `dataframe` 仍定位为 batch 结果物化层，不作为流式主干；`dataframe` 流式主干化冻结，不进入后续路线。

### 2.2 策略分层

| 层级 | 策略项 | 默认值 | Sprint 17 | 说明 |
|---|---|---|---|---|
| L1 | `stream -> batch` 触发策略 | `on_success` | 实施 | 仅 `eof/stopped` 触发下游 batch |
| L1 | 汇聚触发策略 | `all_success` | 实施 | `X ∥ Y -> Z` 默认全成功触发 |
| L1 | 失败策略 | `fail_fast` | 实施 | 任一关键节点失败触发组级失败 |
| L1 | `batch -> stream` 生命周期 | producer/consumer/引用三条件回收 | 实施 | 防止慢消费者场景过早释放 |
| L1 | 错误可见性 | 结构化错误强制透出 | 实施 | `execute` 与 `status/list` 都可见 |
| L2 | `on_terminal` / `on_partial_success` | 关闭 | 不实施 | 后续策略增强 |
| L2 | `partial_finalize` / `continue_on_failure` | 关闭 | 不实施 | 后续策略增强 |
| L2 | 跨任务 mixed DAG | N/A | 不实施 | 非本轮范围 |

---

## 3. 现状机制与关键差距

### 3.1 当前可复用能力

1. 路由与入口：`/tasks/stream/execute` -> `/scheduler/stream/execute`，已支持 `execution_kind=single|group`。
2. 多 SQL 切分：`SplitSqlText` 已稳定，支持 `sql_index` 错误定位。
3. DAG 运行时：`StreamTaskGroup` 已具备节点调度、依赖、超时、stop/fail-fast 主流程。
4. 共享分发：`SharedSourceHub` 已覆盖 dynamic/fixed 模式。
5. 错误契约：`BuildExecutionErrorJson`、`ErrorCodeId/ErrorStageId` 已统一。

### 3.2 当前不满足 Hybrid 的点

1. `HandleStreamExecuteGroup` 强制“所有节点必须是 stream task kind”，直接阻断 `batch + stream` 混合。
2. DAG 构建目前以 stream 节点为中心，依赖解析只覆盖流节点路径。
3. `StreamTaskGroup` 回调模型只面向 stream node runtime，缺 batch node runtime 抽象。
4. 组内节点快照缺少 `node_kind/sql_index/phase` 等混合编排必要可观测字段。
5. 前端 `Tasks.vue` 对 `task_kind=mixed` 仍直接拒绝提交。

---

## 4. 对现有机制的全盘影响评估

### 4.1 Scheduler（核心影响面）

1. `scheduler_stream_group.cpp`：从“stream-only DAG”升级为“hybrid DAG plan + 执行编排”。
2. `stream_task_group.h/.cpp`：节点元数据与查询回调扩展（支持 batch node runtime）。
3. `scheduler_json_codec.cpp`：状态输出新增 `node_kind/sql_index/phase` 与结构化错误对象。
4. `scheduler_runtime_retention.cpp`：新增 batch node runtime 资源回收路径。
5. Gate-0：新增 batch worker/runtime 管理与 batch submit/status/stop 控制入口。

### 4.2 TaskPlugin（任务面）

1. `/tasks/sql/analyze`：保留 `task_kind=mixed`（现有已具备）。
2. `/tasks/stream/execute`：group 模式允许 mixed SQL 提交（不增加新 URI）。
3. 任务存储维持 `task_kind=stream`（group/hybrid 统一归入 stream 任务管理面）。
4. Gate-0：移除 batch worker/queue 执行职责，改为 Scheduler runtime 提交与状态同步。

### 4.3 前端（展示与交互）

1. `Tasks.vue`：`task_kind=mixed` 时走 group 提交，不再报“不支持混合执行”。
2. group 结果展示新增 `node_kind/sql_index/error_code/error_message` 字段。
3. 错误展示严格使用后端结构化信息，不吞错。

### 4.4 不受影响项

1. SQL 语法与 Parser 规则。
2. 单 SQL batch 与单 SQL stream 的现有执行语义。
3. 流通道管理 URI 与通道定义能力。

---

## 5. 统一执行契约（Story 14.14 主链路不新增 URI）

### 5.1 提交契约

`POST /scheduler/stream/execute`

请求（group）：

```json
{
  "execution_kind": "group",
  "group_mode": "dag",
  "sql_text": "...;...;",
  "timeout_s": 0,
  "share_set_ready_timeout_s": 30
}
```

规则：

1. `group_mode` 本轮仅支持 `dag`。
2. 多 SQL 必须分号切分，`statement_count >= 2`。
3. group 模式允许节点混合 `batch/stream`；single 模式仍要求单条 stream SQL。

### 5.2 返回契约

提交成功：

```json
{
  "status": "submitted",
  "task_id": "...",
  "runtime_task_id": "...",
  "runtime_kind": "group",
  "group_mode": "dag",
  "node_count": 4,
  "share_set_count": 1
}
```

提交失败（构建期）：

```json
{
  "error": "...",
  "error_code": "...",
  "error_stage": "request|parse|dag_validate|...",
  "sql_index": 2
}
```

### 5.3 运行态错误可见性（强约束）

`status/list` 对执行中失败必须返回：

1. `phase`：失败阶段（如 `submit/query/execute/capability_check`）。
2. `node_id`：失败节点。
3. `sql_index`：0-based 语句索引。
4. `error_code`：标准错误码。
5. `error_message`：可读错误信息。
6. `ts_ms`：错误时间。

---

## 6. Hybrid DAG 规划与校验设计

### 6.1 规划数据结构

新增/扩展结构：

```cpp
enum class GroupNodeKind {
  kStream,
  kBatch,
};

struct GroupNodePlan {
  std::string id;            // n1/n2/...
  size_t sql_index = 0;      // 0-based
  GroupNodeKind kind;
  std::string sql;
  std::vector<std::string> depends_on;
  GroupStartCondition start_condition; // 本轮保持节点级条件
};
```

设计说明：

1. `sql_index` 作为错误可见性的主键之一，全链路统一 0-based。
2. 本轮 `start_condition` 保持节点级（`on_running/on_finished`），采用“保守提升”策略：若任一依赖要求 `on_finished`，该节点使用 `on_finished`。
3. 下一阶段如需更细粒度并行，可平滑演进为边级条件，不影响本轮能力闭包。

### 6.2 节点类型判定

对每条 SQL：

1. 使用 `SqlParser` 解析。
2. 使用 `ResolveSourceBindings` 识别 source 类型。
3. 判定规则：
   1. 全部 source 为 stream -> `kStream`。
   2. 全部 source 为非 stream -> `kBatch`。
   3. 同一语句混用 stream 与非 stream source -> 提交拒绝（结构化错误）。

### 6.3 依赖构建

1. 归一化每个节点 sink 引用（`ParseChannelRef().base`）。
2. 建立 `sink_base -> producer_indices` 映射。
3. 遍历节点 source：若 source_base 匹配前序 sink_base，则加依赖边。
4. 若一个 source 对应多个 producer：
   1. 不再按“歧义”直接拒绝。
   2. 全部 producer 加边，后续由 sink 能力校验决定是否可执行。
5. Kahn 拓扑校验 DAG 无环。

### 6.4 启动条件推导（节点级）

对节点的每条依赖边推导“期望条件”：

1. 上游为 `batch`：期望 `on_finished`。
2. 下游为 `batch`：期望 `on_finished`。
3. 其余（stream -> stream）：期望 `on_running`。

节点最终 `start_condition`：

1. 若任一依赖期望 `on_finished` -> 节点 `on_finished`。
2. 否则 `on_running`。

### 6.5 并发写约束

1. `stream` sink：沿用现有并发能力校验（`put_mode/max_producers`）。
2. 非 stream sink（`dataframe/database`）：本轮限制“同一 sink_key 仅允许一个 writer 节点”，违反即构建期拒绝。
3. 该限制属于本轮明确边界，不做隐式串行化兜底。

### 6.6 share set 自动构建（fixed）

目的：保障“单读多分支数据集合一致”语义。

规则：

1. 对消费同一 canonical stream source key 集合且成员数 >= 2 的节点，自动建 share set。
2. 不限制必须 root 节点；非 root 节点同样可建（覆盖串并组合分支）。
3. share set 统一走 `SharedSourceHub(kFixed)` + `coordinated_drop=true`。
4. `share_set_ready_timeout_s` 超时按组级失败处理。

实现同步（2026-04-10）：

1. 代码已按“全部 stream 节点（含 root/non-root）”进行 share set 自动分组，不再限制 root-only。
2. 已新增 E2E 回归 `T54.1`：`n1 -> stream.mid` 后，`n2/n3` 同读 `stream.mid` 自动建 share set 并验证双分支完整性。

---

## 7. 执行运行时设计

### 7.1 运行时抽象

在 Scheduler 内新增 batch 节点运行时（建议新增文件）：

1. `scheduler_batch_node_runtime.h`
2. `scheduler_batch_node_runtime.cpp`

核心对象：

```cpp
struct BatchNodeRuntimeSnapshot {
  std::string runtime_task_id;
  std::string node_id;
  GroupNodeStatus status;
  std::string error_code;
  int error_no;
  std::string error_message;
  int64_t started_ms;
  int64_t last_active_ms;
  int64_t finished_ms;
  uint64_t output_rows;
};
```

### 7.2 提交回调

`StreamTaskGroup::SubmitNodeFn` 扩展使用节点元数据：

1. `kStream` 节点：复用 `ExecuteStreamTask(..., lease_owner_id=group_id, skip_lease_acquire=true)`。
2. `kBatch` 节点：创建 `BatchNodeRuntime`，后台线程执行 batch SQL（复用 Scheduler batch 执行核心逻辑），立即返回 `runtime_task_id`。

### 7.3 查询回调

1. stream runtime：复用 `QueryStreamTaskSnapshotByRuntimeId`。
2. batch runtime：查询 `BatchNodeRuntimeSnapshot` 并映射到 group node 快照字段。

### 7.4 stop 回调

1. stream runtime：复用 `RequestStopStreamTaskByRuntimeId`。
2. batch runtime：标记 `stop_requested`（不支持强制中断正在执行的 SQL）。
3. 组 stop/fail 时：
   1. 未提交节点 -> `skipped`。
   2. 运行中 stream 节点 -> 发 stop。
   3. 运行中 batch 节点 -> 等待自然返回后收敛。

### 7.5 fail-fast 收敛

1. 任一节点失败 -> 组状态 `failed`，触发 stop 流程。
2. 组最终错误从首个失败节点继承，并附带 `phase/node_id/sql_index`。

---

## 8. 生命周期与资源回收

### 8.1 `batch -> stream` 生命周期

保持三条件释放：

1. producer_done（batch 完成 + `CloseStream()`）。
2. consumer_done（stream 消费到 EOF）。
3. `ref_count == 0`。

### 8.2 租约与引用

1. group 级 source/sink stream lease 仍在提交阶段统一申请。
2. group 内节点 `ExecuteStreamTask(skip_lease_acquire=true)`，避免重复租约。
3. batch node runtime 不参与 stream lease，但其 stream sink（若有）在 group 级已覆盖。

### 8.3 retention 清理

`SweepRuntimeRetainedObjects` 需新增：

1. 清理 batch node runtime map。
2. 清理 group -> batch runtime 反向索引。
3. 与现有 `stream_group_node_owners_` 保持幂等删除顺序。

---

## 9. 错误模型与可观测性

### 9.1 统一错误结构

构建期错误（`execute`）：

1. `error`
2. `error_code`
3. `error_stage`
4. `sql_index`（若可定位）

运行期错误（`status/list`）：

```json
{
  "last_error": {
    "phase": "execute",
    "node_id": "n3",
    "sql_index": 2,
    "error_code": "OP_EXEC_FAIL",
    "error_message": "...",
    "ts_ms": 1712200000123
  }
}
```

### 9.2 Node 级状态扩展

`nodes[]` 增加字段：

1. `node_kind`: `stream|batch`
2. `sql_index`: 0-based
3. `phase`
4. `error_code/error_message`

---

## 10. 文件级改造清单（可直接指导编码）

### 10.0 Gate-0（先实施）

1. `src/services/scheduler/scheduler_plugin.h/.cpp`
   1. 新增 batch worker 数量配置、batch runtime 索引、启动/停止逻辑。
2. 新增：`src/services/scheduler/scheduler_batch_runtime.h/.cpp`
   1. 实现 batch 作业执行器（submit/status/stop/snapshot）。
3. `src/services/scheduler/scheduler_routes.cpp`
   1. 新增 `POST /scheduler/batch/submit|status|stop`。
4. `src/framework/interfaces/ischeduler_control_service.h`
   1. 扩展 batch submit/status/stop 接口。
5. `src/framework/core/scheduler_control_client.h/.cpp`
   1. 增加对应调用方法并供 TaskPlugin 复用。
6. `src/services/task/task_plugin.h/.cpp`
   1. 移除 batch worker/queue 执行路径，改为提交 Scheduler batch runtime。
   2. 补齐 batch runtime 状态回填与取消透传。
7. `src/tests/test_task/test_task.cpp`
   1. 补齐 batch async 提交、状态推进、取消语义回归。

### 10.1 Scheduler

1. `src/services/scheduler/scheduler_stream_group.cpp`
   1. 节点 kind 判定、混合 DAG 依赖构建、share set 扩展、并发写校验。
2. `src/services/scheduler/stream_task_group.h`
   1. `GroupNodePlan/GroupNodeSnapshot` 增加 `node_kind/sql_index`。
3. `src/services/scheduler/stream_task_group.cpp`
   1. 查询/失败收敛链路透传 `sql_index/node_kind`。
4. `src/services/scheduler/scheduler_json_codec.cpp`
   1. `WriteGroupSnapshotJson` 输出新字段与结构化错误。
5. `src/services/scheduler/scheduler_plugin.h`
   1. 增加 batch node runtime 索引与查询接口声明。
6. `src/services/scheduler/scheduler_runtime_retention.cpp`
   1. 增加 batch node runtime 清理。
7. 新增：`src/services/scheduler/scheduler_batch_node_runtime.h/.cpp`
   1. 实现 batch 节点异步运行时与快照查询。

### 10.2 TaskPlugin

1. `src/services/task/task_plugin.cpp`
   1. group 提交流程保持 `/tasks/stream/execute`，允许 mixed SQL。
   2. 错误透传增强（保持 `error_code/sql_index`）。
2. `src/services/task/task_sql_utils.*`
   1. 无语法变更，必要时补充错误提取辅助函数。

### 10.3 Frontend

1. `src/frontend/src/views/Tasks.vue`
   1. `task_kind=mixed` 走 group 提交。
   2. group 详情表格增加 `node_kind/sql_index/phase`。
   3. 错误优先展示后端结构化字段。
2. `src/frontend/src/api/index.js`
   1. 保持现有 `executeStreamTask` 契约，无需新增 API。

### 10.4 错误码扩展（如需）

文件：`src/framework/core/error_contract.h/.cpp`

建议新增：

1. `STREAM_GROUP_NON_STREAM_SINK_MULTI_WRITER`
2. `STREAM_GROUP_NODE_KIND_INVALID`
3. `STREAM_GROUP_NODE_EXECUTION_FAILED`（batch/stream 通用节点执行失败兜底）

---

## 11. 实施顺序（工程落地）

1. 第 0 步（Gate-0）：batch 异步 worker 下沉到 Scheduler（submit/status/stop + runtime + TaskPlugin 去 worker 化）。
2. 第 1 步：DAG 规划器改造（节点 kind、依赖、校验、share set 扩展）。
3. 第 2 步：batch node runtime 与 group 回调接入（复用 Gate-0 batch runtime）。
4. 第 3 步：状态/错误可见性字段补齐。
5. 第 4 步：TaskPlugin 与前端 mixed 提交流程接入。
6. 第 5 步：全链路回归与稳定性验证。

---

## 12. 测试设计（充分覆盖）

### 12.1 单元测试

建议新增/补充：

1. `test_framework`：
   1. SQL 切分与 `sql_index` 定位（含 `;;`、注释、字符串分号）。
2. `test_scheduler`（或现有 e2e 中的 plan builder 子测试）：
   1. 节点 kind 判定（batch/stream/mixed-invalid）。
   2. 依赖推导与无环校验。
   3. 启动条件推导矩阵（`batch->stream`、`stream->batch`、`stream->stream`）。
   4. 非 stream sink 多 writer 拒绝。
   5. share set 构建（root 与 non-root）。

### 12.2 调度层集成测试（Scheduler E2E）

文件建议：`src/tests/test_scheduler_e2e/test_scheduler_e2e.cpp` 扩展新用例。

覆盖项：

1. 7 条基础路径全部回归通过。
2. 组合拓扑：
   1. `(batch -> stream) -> (stream ∥ stream) -> batch`
   2. `(batch ∥ batch) -> stream -> batch`
3. 错误可见性：
   1. 构建期错误返回 `error_code/error_stage/sql_index`。
   2. 执行期节点失败在 `status/list` 可见 `phase/node_id/sql_index`。
4. stop/timeout：
   1. stop 后 pending 节点为 `skipped`。
   2. timeout 返回 `STREAM_GROUP_TIMEOUT`，并包含未完成节点信息。
5. share set 一致性：
   1. `input_batches = delivered_batches + dropped_batches_shared`。
6. non-root 自动 share set：
   1. `n1 -> stream.mid`，`n2/n3 <- stream.mid` 场景自动建 share set（`share_set_count=1`）。
   2. 双分支接收行数一致且完整。
7. 并发写约束（non-stream）：
   1. 同一 `dataframe/database` sink 出现多 writer 时，构建期失败并返回 `STREAM_GROUP_NON_STREAM_SINK_MULTI_WRITER`。

测试备忘（T60/T61/T62，late join + stop 隔离）：

1. `source.Put(batch)` 成功仅表示写入 source 成功，不代表 batch 已被 shared hub 分发到所有订阅分支。
2. 若在 `Put(5002)` 后立即对其中一个订阅任务执行 `stop`，会出现“目标分支尚未收到该 batch” 的时序竞态，导致断言偶发失败。
3. E2E 用例应在 `Put` 后增加“分发可见性同步点”：轮询并确认各目标分支已观测到关键 batch，再执行 stop 断言。
4. 该备忘是测试同步策略，不改变当前运行时语义；运行时如需“stop 前已写入必须送达”强语义，需后续引入 ack/barrier 机制。

### 12.3 TaskPlugin 集成测试

文件建议：`src/tests/test_task/test_task.cpp` 扩展。

覆盖项：

1. `/tasks/sql/analyze` 返回 `task_kind=mixed`。
2. mixed 多 SQL 可通过 `/tasks/stream/execute` 提交成功。
3. group 执行失败时，任务状态与错误码同步更新。
4. `status/list` 透传新增 node 字段。
5. Gate-0：batch async 提交后可查询运行态，取消可收敛，TaskPlugin 无本地 worker 依赖。

### 12.4 前端联调验证

1. mixed 多 SQL 提交路径可用。
2. “仅异步”提示与执行模式联动正确。
3. group 详情展示 `node_kind/sql_index/error_code/error_message`。
4. 错误弹窗优先显示结构化错误。

### 12.5 稳定性与并发回归

1. `execute` 与 `modify/remove` 并发无 TOCTOU 回归。
2. 100+ 组任务重复提交/stop/timeout 无资源泄漏。
3. retention 清理后无悬挂 runtime 索引。
4. ASAN/TSAN（可选）回归无新增崩溃与数据竞争。
5. Gate-0：batch runtime 在高并发 submit/status/stop 下无死锁、无状态丢失。

---

## 13. 验收标准（实现完成判定）

1. 7 条路径在同一版本全部可执行。
2. 组合 DAG 能力可用，不依赖路径白名单分流。
3. 违反约束稳定失败，错误码可定位到节点与 SQL 序号。
4. `status/list` 可观测字段完整，前端可直接消费展示。
5. 现有 stream-only group 能力无行为回退。
6. `dataframe` 不作为流式主干的定位保持不变。

---

## 14. 风险与缓解

1. 风险：`scheduler_stream_group.cpp` 持续膨胀，维护成本上升。
   1. 缓解：按本设计拆分 `BatchNodeRuntime` 独立文件，后续可继续拆 plan builder。
2. 风险：batch 节点 stop 不可抢占导致收敛变慢。
   1. 缓解：明确状态语义（stopping -> terminal），并在超时与错误中可观测。
3. 风险：non-root share set 自动构建引入语义回归。
   1. 缓解：增加专门回归用例验证分支一致性与指标守恒。
4. 风险：前后端字段不一致造成展示缺失。
   1. 缓解：在 TaskPlugin 与前端联调中以 `status/list` JSON schema 作为验收基线。

---

## 15. 增补任务 R1：`HandleStreamExecuteGroup` 重构（函数签名级实施清单）

### 15.1 目标与不变项

目标：

1. 将 [`SchedulerPlugin::HandleStreamExecuteGroup`](/mnt/d/working/flowSQL/src/services/scheduler/scheduler_stream_group.cpp:257) 从「超大函数」重构为分层编排入口，降低复杂度与回归风险。
2. 保持现有行为与契约完全一致，不引入策略变化。

强约束（必须保持）：

1. 成功响应字段不变：`status/task_id/runtime_task_id/runtime_kind/group_mode/node_count/share_set_count`。
2. 错误返回不变：`error_code/error_stage/sql_index` 语义与取值保持一致。
3. 运行时语义不变：lease 申请、share set ready/start 时序、stop/fail-fast 收敛保持一致。
4. 不新增执行 URI，不新增 SQL 语法，不新增兼容分支。

### 15.2 文件拆分方案

1. 保留 `src/services/scheduler/scheduler_stream_group.cpp`
   1. 仅保留入口编排与少量 glue 逻辑。
2. 新增 `src/services/scheduler/scheduler_stream_group_request.cpp`
   1. 请求校验与 `sql_text` 切分、参数归一化。
3. 新增 `src/services/scheduler/scheduler_stream_group_planner.cpp`
   1. 节点构建、依赖/DAG 校验、sink 能力校验、share set 规划。
4. 新增 `src/services/scheduler/scheduler_stream_group_runtime.cpp`
   1. lease/本地资源构建、回调装配、注册启动、ready 等待与失败回滚。
5. 新增 `src/services/scheduler/scheduler_stream_group_internal.h`
   1. R1 专用内部结构体与函数声明（仅 Scheduler 内部使用）。
6. 更新 `src/services/scheduler/CMakeLists.txt`
   1. 增加上述编译单元。

### 15.3 内部结构体签名（`scheduler_stream_group_internal.h`）

```cpp
namespace flowsql {
namespace scheduler {

struct StreamGroupExecuteRequest final {
    int timeout_s = 0;
    int share_set_ready_timeout_s = 30;
    std::vector<std::string> sqls;
};

struct StreamGroupNodeResolvedMeta final {
    std::vector<std::string> source_keys;
    std::vector<std::string> resolved_sources;
    std::string expand_rule = "explicit";
    std::vector<std::shared_ptr<IStreamChannel>> stream_channels;
    bool has_stream_source = false;
    bool has_non_stream_source = false;
};

struct StreamGroupShareSetPlan final {
    std::string id;
    std::string source_ref;
    std::vector<std::string> members;
    std::vector<std::string> canonical_source_keys;
    std::vector<std::shared_ptr<IStreamChannel>> source_channels;
};

struct StreamGroupBuildArtifacts final {
    std::vector<GroupNodePlan> plans;
    std::unordered_map<std::string, size_t> node_index;
    std::unordered_map<std::string, StreamGroupNodeResolvedMeta> node_resolved;
    std::vector<std::string> group_source_keys;
    std::vector<std::string> group_sink_keys;
    std::vector<StreamGroupShareSetPlan> share_set_plans;
};

struct StreamGroupShareSetRuntimeItem final {
    std::string id;
    std::string source_ref;
    std::vector<std::string> members;
    std::vector<std::string> internal_channel_refs;
    std::shared_ptr<SharedSourceHub> hub;
};

struct StreamGroupRuntimeArtifacts final {
    std::unordered_map<std::string, std::string> node_source_overrides;
    std::vector<StreamGroupShareSetRuntimeItem> share_set_runtimes;
    std::vector<std::string> created_channel_refs;
};

struct StreamGroupCallbackContext final {
    std::string group_runtime_task_id;
    int timeout_s = 0;
    std::unordered_map<std::string, GroupNodeKind> node_kind_by_id;
    std::unordered_map<std::string, std::string> node_source_overrides;
    std::unordered_map<std::string, GroupNodeKind> runtime_kind_by_node_task_id;
    std::mutex runtime_kind_mu;
};

}  // namespace scheduler
}  // namespace flowsql
```

### 15.4 `SchedulerPlugin` 私有函数签名（精确到可编码）

在 `scheduler_plugin.h` 私有区新增（或替换）以下声明：

```cpp
struct StreamGroupExecuteRequest;
struct StreamGroupBuildArtifacts;
struct StreamGroupRuntimeArtifacts;
struct StreamGroupCallbackContext;
```

```cpp
int32_t ParseStreamGroupExecuteRequest(const rapidjson::Document& doc,
                                       StreamGroupExecuteRequest* out,
                                       std::string* err_rsp);

int32_t BuildStreamGroupPlan(const StreamGroupExecuteRequest& req,
                             StreamGroupBuildArtifacts* out,
                             std::string* err_rsp);

int32_t ValidateStreamGroupPlan(const StreamGroupBuildArtifacts& build,
                                std::string* err_rsp);

int32_t AcquireStreamGroupLeases(const std::string& runtime_task_id,
                                 const StreamGroupBuildArtifacts& build,
                                 std::string* err_rsp);

int32_t PrepareStreamGroupRuntimeResources(const std::string& runtime_task_id,
                                           const StreamGroupBuildArtifacts& build,
                                           StreamGroupRuntimeArtifacts* out,
                                           std::function<void()>* cleanup_local_resources,
                                           std::string* err_rsp);

std::shared_ptr<StreamTaskGroup> BuildStreamGroupObject(
    const std::string& runtime_task_id,
    const StreamGroupExecuteRequest& req,
    const StreamGroupBuildArtifacts& build,
    const StreamGroupRuntimeArtifacts& runtime_build,
    std::shared_ptr<StreamGroupCallbackContext>* callback_ctx_out);

int32_t RegisterAndStartStreamGroup(const std::string& runtime_task_id,
                                    const StreamGroupBuildArtifacts& build,
                                    const StreamGroupRuntimeArtifacts& runtime_build,
                                    const std::shared_ptr<StreamTaskGroup>& group,
                                    int share_set_ready_timeout_s,
                                    std::function<void()> cleanup_local_resources,
                                    std::string* err_rsp);
```

说明：

1. `HandleStreamExecuteGroup` 仅负责串联上述阶段，不再承担阶段内部细节。
2. 每个阶段必须单点返回错误 JSON，禁止跨阶段拼接错误字符串。

### 15.5 节点回调拆分签名（替代超大 lambda）

```cpp
int SubmitStreamGroupNodeRuntime(StreamGroupCallbackContext* ctx,
                                 const std::string& node_id,
                                 const std::string& sql,
                                 std::string* node_runtime_task_id,
                                 std::string* error_msg);

int QueryStreamGroupNodeRuntime(StreamGroupCallbackContext* ctx,
                                const std::string& node_runtime_task_id,
                                TaskSnapshot* snapshot_out);

void StopStreamGroupNodeRuntime(StreamGroupCallbackContext* ctx,
                                const std::string& node_runtime_task_id);

void StopStreamGroupShareSetHubs(const std::string& group_runtime_task_id);
```

约束：

1. 回调内只允许访问 `StreamGroupCallbackContext` + `SchedulerPlugin` 明确方法。
2. 回调内禁止直接操作 `stream_group_*` map（统一经 `SchedulerPlugin` 封装方法处理）。

### 15.6 迁移步骤（R1-1 ~ R1-8）

1. `R1-1`：引入 `scheduler_stream_group_internal.h`，落地结构体与函数声明，不改行为。
2. `R1-2`：抽取 `ParseStreamGroupExecuteRequest`，入口函数先替换请求段。
3. `R1-3`：抽取 `BuildStreamGroupPlan + ValidateStreamGroupPlan`，入口只接收 artifacts。
4. `R1-4`：抽取 `AcquireStreamGroupLeases`，并保持失败自动回滚语义。
5. `R1-5`：抽取 `PrepareStreamGroupRuntimeResources`，统一内部通道与 share set 构建/清理。
6. `R1-6`：抽取 `Submit/Query/Stop` 回调函数，移除大 lambda。
7. `R1-7`：抽取 `RegisterAndStartStreamGroup`，统一启动失败与 ready 超时收敛路径。
8. `R1-8`：删除入口遗留重复逻辑，控制入口函数行数 `< 200`，补齐回归测试。

### 15.7 测试与验收（R1 专项）

1. 行为一致性：
   1. 重构前后同一输入，成功响应关键字段一致。
   2. 重构前后同一失败输入，`error_code/error_stage/sql_index` 一致。
2. 回归用例：
   1. `test_scheduler_e2e`：`T49/T54.1/T60/T61/T62/T66/T67/T68` 全通过。
   2. `test_scheduler_mutation_guard` 全通过。
3. 质量指标：
   1. `HandleStreamExecuteGroup` 行数 `< 200`。
   2. 入口函数 `return` 分支显著下降，阶段函数职责单一。
   3. 无新增死锁/资源泄漏/悬挂回调问题。
