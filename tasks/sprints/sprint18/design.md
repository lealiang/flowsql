# Sprint 18 设计文档：执行实例单画布 DAG 可视化

## 1. 背景与目标

当前 Web 端的任务可视化仍偏向「按 SQL 逐行展示」，不能体现真实编排关系。典型问题：

```sql
select * from dataframe.VNAT into ring.spsc_stream;
select * from ring.spsc_stream USING builtin.passthrough_stream into dataframe.VNAT_COPY;
```

上述两条 SQL 在运行时是串接关系，但现有页面难以直观看到上下游依赖与触发语义。

Sprint 18 的目标是把可视化对象从「SQL 文本」切换为「执行实例（Runtime Instance）」，在一张画布上展示完整 DAG，并让依赖关系、触发语义（`on_start/on_data/on_finish`）与运行状态可观测。

### 1.1 问题复盘：为何会出现“任务表有 runtime_task_id，但可视化查询不到”

现象：

1. `tasks` 表中能查到 `runtime_task_id=stream_task_xxx`；
2. 前端调用 `/api/tasks/runtime/graph/query` 却收到 `runtime task not found`。

前因后果：

1. `runtime_task_id` 是任务元数据中的“引用标识”，会持久化在 `TaskStore`；
2. Scheduler 的运行时对象（`stream_tasks_ / stream_task_groups_`）是内存态，不会随进程重启保留；
3. 运行时对象还会被 retention 清理（终态后按 `stream_runtime_retention_s/max_count` 回收）；
4. 因此“任务记录存在”不等价于“live runtime 仍可查询”；
5. 若查询链路把该场景直接视为错误，会导致历史任务可视化不可用。

设计结论：

1. Runtime Graph 查询采用“双源策略”：
   - 优先 `live runtime`（Scheduler 内存态）；
   - `live runtime` 缺失时，降级到 `TaskStore` 可重建图（reconstructed snapshot）。
2. 对用户侧接口，`runtime not found` 不再直接暴露为失败，而是返回降级图并携带降级标记。

---

## 2. 迭代边界

### 2.1 In Scope（阶段一）

1. Runtime Graph 后端契约与查询接口。
2. 单画布 DAG 渲染（`Channel`/`Operator`）。
3. 触发语义边可视化（`on_start/on_data/on_finish`）。
4. 运行时状态与关键指标展示。
5. 轮询刷新与终态收敛。
6. 所有任务（batch/stream/mixed）均可进入可视化页面。
7. 任务原始 SQL 独立持久化，并在任务历史列表显示任务类型列。

### 2.2 Out of Scope（阶段二）

1. 可视化编辑器（拖拽编排、保存草稿、发布）。
2. 未提交 SQL 的预览态可视化。
3. 新增 SQL 语法或新的执行入口 URI。
4. 跨任务实例合并绘图。

---

## 3. 决策冻结

1. 可视化对象固定为「执行实例」，不再以 SQL 文本为主视图。
2. 前端不再机械拆 SQL 猜图，图结构由后端统一输出。
3. SQL 工作台的「可视化」按钮不按 task kind 限制。
4. 同名通道节点必须去重，保证多 SQL 自动串接成一张图。
5. 触发语义以边类型表达：
   - `on_start`：启动触发
   - `on_data`：数据触发
   - `on_finish`：完成触发
6. 任务 SQL 文本不再使用 `[group] N SQL nodes` 摘要替代，必须可查询原始 SQL 文本。

---

## 4. Runtime Graph 统一数据模型

## 4.1 核心结构

```json
{
  "task_id": "tsk_20260410153000_1",
  "runtime_task_id": "rt_20260410153000_1",
  "task_kind": "stream",
  "runtime_kind": "group",
  "status": "running",
  "snapshot_time_ms": 1760000000000,
  "nodes": [],
  "edges": [],
  "events": [],
  "next_cursor": 120
}
```

字段说明：

1. `snapshot_time_ms`：快照时间戳（毫秒）；用于展示快照采样时刻。
2. `events/next_cursor`：增量事件流游标。
3. `status`：实例总状态（非节点状态）。

### 4.2 Node 定义

```json
{
  "id": "channel:dataframe.VNAT",
  "kind": "channel|operator|control",
  "name": "dataframe.VNAT",
  "sql_index": 0,
  "status": "running",
  "phase": "on_data",
  "processed_rows": 1200,
  "output_rows": 1200,
  "error_code": "",
  "error_message": "",
  "start_at_ms": 1760000000100,
  "end_at_ms": 0
}
```

说明：

1. `kind=channel` 表示通道节点；`kind=operator` 表示算子节点。
2. `sql_index` 统一使用 0-based。
3. `phase` 用于展示节点当前执行阶段。

### 4.3 Edge 定义

```json
{
  "id": "edge_1",
  "from": "channel:dataframe.VNAT",
  "to": "operator:sql0",
  "edge_kind": "data|control",
  "trigger": "on_data|on_start|on_finish",
  "status": "idle|active|done|blocked",
  "rows": 1200,
  "fire_count": 32,
  "last_fire_at_ms": 1760000002000
}
```

说明：

1. `edge_kind=data` 通常用于 source/sink 数据链路。
2. `edge_kind=control` 通常用于依赖关系与启动/完成触发。

### 4.4 Event 定义

```json
{
  "seq": 101,
  "ts_ms": 1760000003000,
  "type": "node_phase_changed|edge_fired|node_failed|node_finished",
  "node_id": "operator:sql0",
  "edge_id": "",
  "detail": "phase running -> stopped"
}
```

---

## 5. 接口设计（URI 冻结）

### 5.1 URI 设计冻结表

| 层级 | Method | URI | 责任方 | 说明 |
|---|---|---|---|---|
| Web 对外 | `POST` | `/api/tasks/runtime/graph/query` | WebServer | 前端统一入口，按 `task_id` 查询执行实例 DAG |
| 管理面内部 | `POST` | `/tasks/runtime/graph/query` | TaskPlugin | 校验任务归属并完成 `task_id -> runtime_task_id` 映射 |
| 调度内部 | `POST` | `/scheduler/runtime/graph/query` | Scheduler | 构建并返回 Runtime Graph 快照 |

冻结规则：

1. 三层 URI 一一对应，语义保持一致。
2. 不复用现有 `/tasks/stream/status` 或 `/tasks/result`。
3. 不提供兼容别名 URI，不做隐式兜底转发。

### 5.2 请求契约

请求体（统一）：

```json
{
  "task_id": "tsk_20260410153000_1",
  "cursor": 0,
  "include_events": true
}
```

字段约束：

1. `task_id`：必填，字符串，不能为空。
2. `cursor`：可选，默认 `0`，`>=0`；用于事件增量拉取。
3. `include_events`：可选，默认 `true`；`false` 时仅返回快照主结构（`events=[]`）。

### 5.3 响应契约

成功响应：

```json
{
  "task_id": "tsk_20260410153000_1",
  "runtime_task_id": "rt_20260410153000_1",
  "task_kind": "stream",
  "runtime_kind": "group",
  "graph_source": "live",
  "degraded": false,
  "degrade_reason": "",
  "sql_text": "SELECT ... ; SELECT ... ;",
  "sqls": ["SELECT ...", "SELECT ..."],
  "status": "running",
  "snapshot_time_ms": 1760000000000,
  "nodes": [],
  "edges": [],
  "events": [],
  "next_cursor": 120
}
```

约束：

1. `nodes/edges` 始终返回数组，不返回 `null`。
2. `include_events=false` 时，`events=[]` 且 `next_cursor` 仍返回当前游标。
3. 若实例已终态但仍在 retention 窗口内，返回最后快照。
4. 若 `live runtime` 已被清理或进程重启丢失，返回 `200` 且降级为可重建图：
   - `graph_source="reconstructed"`
   - `degraded=true`
   - `degrade_reason="runtime_not_found"`
5. `sql_text/sqls` 始终由 `task_sql_payloads` 注入，供前端 SQL 对照显示。

### 5.4 错误码与 HTTP 映射

| error_code | HTTP | 场景 |
|---|---:|---|
| `RUNTIME_GRAPH_TASK_NOT_FOUND` | 404 | `task_id` 不存在 |
| `RUNTIME_GRAPH_CURSOR_INVALID` | 400 | `cursor < 0` 或类型非法 |
| `RUNTIME_GRAPH_SQL_PAYLOAD_INVALID` | 500 | SQL payload 缺失或损坏，无法构建/重建图 |
| `RUNTIME_GRAPH_BUILD_FAILED` | 500 | Scheduler 构图异常 |

错误响应格式（统一）：

```json
{
  "error": "runtime graph task not found",
  "error_code": "RUNTIME_GRAPH_TASK_NOT_FOUND"
}
```

### 5.5 路由职责边界

1. WebServer：仅转发，不拼装业务字段。
2. TaskPlugin：
   - 校验 `task_id`；
   - 获取任务记录并解析 `runtime_task_id/task_kind/runtime_kind`；
   - 转发到 Scheduler 并补齐任务元信息；
   - 当 Scheduler 返回 `runtime not found` 时，切换到 `TaskStore` 本地重建图并返回降级标记。
3. Scheduler：
   - 基于 `runtime_task_id` 读取运行态；
   - 输出图快照与事件增量。

### 5.6 Stream 状态通道重置接口（补充）

为解决“有状态 stream 通道残留取消态（如 `ECANCELED`）导致后续任务写入失败”的运维问题，新增显式重置接口：

1. 调度层：`POST /channels/stream/reset`
2. Web 层：`POST /api/channels/stream/reset`

请求体：

```json
{
  "type": "ring",
  "name": "testHub"
}
```

语义与约束：

1. `reset` 只重置运行态，不修改配置；实现方式为“读取当前 option 后同配置重建通道”。
2. 必须复用 `TryBeginStreamChannelMutation` 并执行冲突检查：
   - `in_use` / `source_in_use` / `mutating` 返回 `409`。
3. 通道不存在返回 `404`。
4. 接口不提供兼容别名，不做隐式兜底恢复。
5. 前端通道管理页提供“重置”按钮；`in_use=true` 时禁用操作。

---

## 6. 后端实现设计

### 6.1 图构建规则

每条 SQL 抽象为：

1. `source_channel -> operator`（`trigger=on_data`）
2. `operator -> sink_channel`（`trigger=on_data`）

依赖关系抽象为：

1. `upstream_operator -> downstream_operator`（`trigger=on_finish`，`edge_kind=control`）

启动关系抽象为：

1. `task_start -> root_operator`（`trigger=on_start`，可用 `control` 节点）

### 6.2 节点去重策略

1. `channel` 节点按 canonical key 去重（如 `dataframe.VNAT`）。
2. `operator` 节点按 `sql_index` 唯一（`operator:sql{index}`）。

### 6.3 多任务类型处理

1. `stream single/group`：直接由 Scheduler runtime 输出节点与边。
2. `batch`：构造最小图（单 SQL 也可展示 source->operator->sink），状态来自 batch runtime。
3. `mixed`：按 DAG 节点类型聚合，统一输出到同一图。

### 6.4 并发与一致性

1. Graph 快照构建在 runtime 读锁内完成，避免半更新状态外泄。
2. `snapshot_time_ms` 表示本次快照采样时间（毫秒）。
3. `cursor` 仅用于事件流，快照本体始终返回全量结构。

### 6.5 任务 SQL 载荷独立持久化

#### 6.5.1 设计目标

1. 原始 SQL 文本独立存储，避免被 `request_sql` 摘要覆盖。
2. 执行编排（single/group）与 SQL 展示解耦。
3. 不保留退化读取逻辑（历史任务由人工清理）。

#### 6.5.2 表结构设计

新增表：

```sql
CREATE TABLE IF NOT EXISTS task_sql_payloads (
  task_id TEXT PRIMARY KEY,
  raw_sql_text TEXT NOT NULL,
  sqls_json TEXT NOT NULL,
  sql_count INTEGER NOT NULL,
  created_at DATETIME NOT NULL DEFAULT CURRENT_TIMESTAMP,
  FOREIGN KEY(task_id) REFERENCES tasks(task_id) ON DELETE CASCADE
);
CREATE INDEX IF NOT EXISTS idx_task_sql_payloads_created_at
  ON task_sql_payloads(created_at);
```

说明：

1. `raw_sql_text` 保存用户提交原文（多 SQL 使用分号分隔的完整文本）。
2. `sqls_json/sql_count` 作为执行编排与回放的结构化载荷。
3. `tasks.request_sql` 调整为摘要用途（可重命名为 `sql_summary`，本 Sprint 最低要求是语义切换并在代码注释中明确）。

#### 6.5.3 写入时机与事务

1. `POST /tasks/batch/execute`、`POST /tasks/stream/execute` 创建任务时，统一执行单事务写入：
   - `tasks`：任务元数据与摘要；
   - `task_sql_payloads`：原始 SQL 及拆分结果。
2. 任一写入失败即整体回滚，禁止“仅任务元数据成功”。

#### 6.5.4 查询规则（无退化）

1. 任务列表 `sql_text`、任务详情 `sql_text`、Runtime Graph SQL 输入均从 `task_sql_payloads` 读取。
2. 删除 `request_sql -> sql_text` 的退化路径。
3. 若 payload 缺失/损坏，返回结构化错误，不隐式兜底。

#### 6.5.5 错误码补充

| error_code | HTTP | 场景 |
|---|---:|---|
| `TASK_SQL_PAYLOAD_INVALID` | 500 | 任务详情/列表查询时 payload 缺失或损坏 |
| `RUNTIME_GRAPH_SQL_PAYLOAD_INVALID` | 500 | Runtime Graph 查询时 payload 缺失或损坏 |

---

## 7. 前端实现设计

### 7.1 页面与路由

1. 路由继续使用：`/tasks/runtime/:taskId`。
2. SQL 工作台「可视化」按钮对所有任务开放。
3. 任务历史表增加“任务类型”列，显示 `task_kind`（`batch`/`stream`）。

### 7.2 渲染模型

1. 画布按后端 `nodes/edges` 直接渲染。
2. 节点卡片显示：名称、类型、`sql_index`、`status`、`phase`、`processed/output`、错误信息。
3. 边样式映射：
   - `on_data`：主实线（可带流动动画）
   - `on_start`：虚线
   - `on_finish`：点线

### 7.3 刷新策略

1. 默认 2 s 轮询 `graph/query`。
2. 请求带 `cursor` 拉取增量 `events`。
3. 实例终态后自动停止轮询。

---

## 8. 示例（目标效果）

输入 SQL：

```sql
select * from dataframe.VNAT into ring.spsc_stream;
select * from ring.spsc_stream USING builtin.passthrough_stream into dataframe.VNAT_COPY;
```

期望图：

```text
dataframe.VNAT --(on_data)--> transfer(sql0) --(on_data)--> ring.spsc_stream
ring.spsc_stream --(on_data)--> builtin.passthrough_stream(sql1) --(on_data)--> dataframe.VNAT_COPY
```

其中 `ring.spsc_stream` 节点唯一且复用，形成串接链路。

---

## 9. 测试设计

### 9.1 后端单元测试

1. 节点去重测试：同名 channel 只出现一次。
2. 边语义测试：`on_start/on_data/on_finish` 映射正确。
3. 时间戳与游标测试：`snapshot_time_ms` 字段存在且为毫秒时间戳，`cursor` 增量正确。

### 9.2 后端集成测试

1. stream chain 场景返回可串接图。
2. group DAG 场景返回依赖边与节点状态。
3. batch 任务返回最小可视化图。
4. task 不存在/运行时不存在返回结构化错误。

### 9.3 前端验证

1. 所有任务类型可点击进入可视化页。
2. 画布中节点与边数量与后端快照一致。
3. 终态自动停止轮询。
4. `npm --prefix src/frontend run build` 通过。

### 9.4 SQL 持久化与任务列表验证

1. 提交 group 任务后，任务列表 `sql_text` 显示原始 SQL 文本（不再显示 `[group] N SQL nodes`）。
2. 任务详情返回原始 SQL 文本。
3. 任务历史显示“任务类型”列，值与 `task_kind` 一致。
4. 人工构造 payload 缺失场景时，接口返回 `TASK_SQL_PAYLOAD_INVALID` 或 `RUNTIME_GRAPH_SQL_PAYLOAD_INVALID`。

---

## 10. 文件级实施清单

### 10.1 后端

1. `src/services/scheduler/scheduler_routes.cpp`
2. `src/services/scheduler/scheduler_plugin.h`
3. `src/services/scheduler/scheduler_plugin.cpp`
4. `src/services/task/task_plugin.cpp`
5. `src/services/web/web_server.cpp`
6. `src/framework/core/scheduler_control_client.h`
7. `src/framework/core/scheduler_control_client.cpp`

### 10.2 前端

1. `src/frontend/src/api/index.js`
2. `src/frontend/src/views/Tasks.vue`
3. `src/frontend/src/views/TaskRuntime.vue`
4. `src/frontend/src/router/index.js`

### 10.3 测试

1. `src/tests/test_scheduler_e2e.cpp`
2. `src/tests/test_task.cpp`

---

## 11. 验收门槛（DoD）

1. Story 17.1 的 8 条验收项全部满足。
2. `TaskRuntime` 页不再以 SQL 行机械渲染为主逻辑。
3. 多 SQL 串接示例在单画布上可正确展示依赖关系。
4. 触发语义（`on_start/on_data/on_finish`）可见且与后端一致。
5. 后端与前端回归测试通过，且文档与实现一致。
6. 编排任务历史中可见原始 SQL 文本，且任务类型列可见。

---

## 12. 增补设计（待评审）：`dataframe -> stream_hub(split)` 分派算子

### 12.1 背景与问题

当前 `source=dataframe` 且 `sink=stream_hub(split)` 时，执行路径本质是单次 `RecordBatch` 写入，`stream_hub` 分派按 batch 粒度执行，导致“单批数据全部进入同一分区（常见为 `[0]`）”。

这与“将 dataframe 数据分流到多个下游分区”的业务预期不一致。

### 12.2 目标

1. 提供统一、显式、可扩展的 dataframe 分派能力。
2. 默认支持行级轮询分派。
3. 支持按字段哈希取模分派（同值同分区）。
4. 对不满足约束的输入给出明确错误，不做静默兜底。

### 12.3 SQL 语法设计

新增内置算子：`builtin.dataframe_dispatch_stream`。

#### 12.3.1 规范写法

```sql
SELECT * FROM dataframe.VNAT
USING builtin.dataframe_dispatch_stream WITH strategy=round_robin
INTO stream_hub.testHub;
```

```sql
SELECT * FROM dataframe.VNAT
USING builtin.dataframe_dispatch_stream WITH strategy=hash,field_name=service_id
INTO stream_hub.testHub;
```

```sql
SELECT * FROM dataframe.VNAT
USING builtin.dataframe_dispatch_stream WITH strategy=range,range_rows=100
INTO stream_hub.testHub;
```

```sql
SELECT * FROM dataframe.VNAT
USING builtin.dataframe_dispatch_stream
INTO stream_hub.testHub;
```

#### 12.3.2 参数推导规则（冻结）

说明：沿用当前 SQL 解析器能力，`WITH` 采用 `key=value` 形式。

1. **完全不提供参数**（无 `WITH`）：
   - 等价于 `strategy=range,range_rows=auto`（按分区数平均切分连续行段）。
2. **仅提供 `field_name` 参数**（`WITH field_name=...`）：
   - 等价于 `strategy=hash,field_name=...`。
3. **其他情况必须显式提供 `strategy`**：
   - 例如只给 `range_rows` 或同时给多个业务参数时，若无 `strategy` 直接报错。
4. 当显式 `strategy=range` 时：
   - `range_rows` 可选；缺省为 `auto`。
5. 参数冲突直接报错：
   - `strategy=hash` 不允许带 `range_rows`；
   - `strategy=round_robin` 不允许带 `field_name/range_rows`；
   - `strategy=range` 不允许带 `field_name`。

### 12.4 执行语义

1. `strategy=round_robin`：按行轮询写入 `stream_hub.<name>[i]`。
2. `strategy=hash`：`partition = hash(field_value) % partition_count`。
3. `strategy=range`：按连续行范围分派。
   - `range_rows=<N>`：`partition=(row_index/N)%partition_count`，如 `N=100` 时 `0-99->[0]，100-199->[1]`；
   - `range_rows=auto`：按总行数与分区数做平均切分（连续区间）。
4. `field_value=NULL`：按固定字节串参与哈希（保证可重复路由）。
5. 输出行数守恒：各分区行数总和必须等于输入行数。

### 12.5 约束与错误处理

1. sink 必须是 `stream_hub` 且 `mode=split`，否则拒绝执行。
2. `strategy=hash` 必须给出 `field_name`。
3. `field_name` 不存在于输入 schema 时直接报错。
4. `strategy=range` 且 `range_rows` 显式给定时必须 `>0`。
5. `partition_count<=0` 或无法解析分区时直接报错。
6. `strategy` 非法值（非 `round_robin/hash/range`）直接报错。
7. 未命中“无参数默认”与“仅 field_name 自动 hash”时，如缺少 `strategy`，直接报错。
8. 策略-参数冲突（如 `hash+range_rows`）直接报错。

建议错误码：

1. `DATAFRAME_DISPATCH_INVALID_SINK`
2. `DATAFRAME_DISPATCH_INVALID_STRATEGY`
3. `DATAFRAME_DISPATCH_FIELD_REQUIRED`
4. `DATAFRAME_DISPATCH_FIELD_NOT_FOUND`
5. `DATAFRAME_DISPATCH_PARTITION_INVALID`
6. `DATAFRAME_DISPATCH_STRATEGY_REQUIRED`
7. `DATAFRAME_DISPATCH_PARAM_CONFLICT`
8. `DATAFRAME_DISPATCH_RANGE_ROWS_INVALID`

### 12.6 架构与代码落点

1. 该能力实现为 **batch 算子路径**（`IOperator`），不改变 `stream task` 的分类规则。
2. 算子职责：
   - 读取输入 `IDataFrameChannel`；
   - 校验输出 `IStreamChannel` 是否为 `stream_hub(split)`；
   - 获取分区通道并按策略将行切分成多个 `RecordBatch` 写入；
   - 关闭输出流。
3. 调度器不引入隐式行为变更：显式 `USING builtin.dataframe_dispatch_stream` 才启用该语义（自动注入是否开启，另行决策）。

### 12.7 线程与性能考虑

1. 首版采用单线程分派，保证正确性优先。
2. 行级切分会增加 batch 数量，需限制最小批尺寸（避免每行一个超小 batch 导致开销过大）。
3. 首版建议参数：
   - `emit_batch_rows`（默认 `256`，可选）用于聚合后写入。
4. 后续可演进为：
   - 分区并行构建 batch；
   - 向量化哈希。

### 12.8 测试设计（新增）

1. `round_robin` 基线：
   - 输入 8 行，4 分区，预期每分区 2 行。
2. `hash` 一致性：
   - 相同 `field_name` 值必须路由到同一分区。
3. `range` 固定窗口：
   - `range_rows=100` 时，验证 `0-99->[0]，100-199->[1]` 的连续分段行为。
4. `range` 自动均分：
   - 无参数时自动按分区数平均切段，验证总量守恒与分段连续性。
5. `hash` 错误分支：
   - 缺少 `field_name`；
   - `field_name` 不存在；
   - `strategy` 非法。
6. 策略推导与冲突：
   - 仅 `field_name` 自动推导为 `hash`；
   - 仅 `range_rows` 且无 `strategy` 应报 `DATAFRAME_DISPATCH_STRATEGY_REQUIRED`；
   - `hash + range_rows`、`range + field_name` 应报 `DATAFRAME_DISPATCH_PARAM_CONFLICT`。
7. sink 约束：
   - `INTO ring.xxx` 必须报 `DATAFRAME_DISPATCH_INVALID_SINK`。
8. 总量守恒：
   - 所有分区 dataframe 汇总行数等于输入行数。

### 12.9 文档与前端影响

1. README 的 SQL 能力矩阵新增一行：
   - `dataframe -> stream_hub(split)` 支持 `builtin.dataframe_dispatch_stream`（RR/Hash/Range）。
2. SQL 编辑提示中补充示例（后续可在阶段二统一整理）。
3. 通道管理前端无需新增字段；策略由 SQL 的 `WITH` 参数表达。

### 12.10 代码改动清单（精确到文件）

#### 12.10.1 新增文件

1. `src/framework/builtin/dataframe/dataframe_dispatch_stream_operator.h`
2. `src/framework/builtin/dataframe/dataframe_dispatch_stream_operator.cpp`

#### 12.10.2 修改文件

1. `src/framework/CMakeLists.txt`
   - 将 `dataframe_dispatch_stream_operator.cpp` 加入 `flowsql_common` 源文件列表。
2. `src/services/builtin/builtin_registry.cpp`
   - 注册内置算子：
     - `category=builtin`
     - `name=dataframe_dispatch_stream`
     - `aliases={"dataframe_dispatch_stream","builtin.dataframe_dispatch_stream"}`。
3. `src/tests/test_scheduler_e2e/test_scheduler_e2e.cpp`
   - 新增 e2e 用例覆盖 `round_robin/hash/range(auto|fixed)` 与冲突报错。
4. `README.md`
   - 在 SQL 能力矩阵补充该算子能力与参数规则。

#### 12.10.3 说明（不改动）

1. `SqlParser` 无需改动（继续 `WITH key=value`）。
2. `SchedulerPlugin::HandleExecute` 无需新增分支（走既有 batch 算子链）。
3. `stream_hub` 通道实现无侵入改动（仍按 `partition_id` 或 RR 消费输入 batch）。

### 12.11 算子接口与数据结构设计

#### 12.11.1 类定义

```cpp
class DataframeDispatchStreamOperator : public IOperator {
 public:
  std::string Category() override { return "builtin"; }
  std::string Name() override { return "dataframe_dispatch_stream"; }
  std::string Description() override;
  OperatorPosition Position() override { return OperatorPosition::DATA; }

  int Configure(const char* key, const char* value) override;
  int Work(IChannel* in, IChannel* out) override;
  std::string LastError() override { return last_error_; }

 private:
  enum class Strategy { kRoundRobin, kHash, kRange };

  struct DispatchConfig {
    bool has_strategy = false;
    Strategy strategy = Strategy::kRange;   // 无参数默认 range(auto)
    bool has_field_name = false;
    std::string field_name;
    bool has_range_rows = false;            // false 表示 auto
    int64_t range_rows = 0;                 // >0 时生效
    int64_t emit_batch_rows = 256;          // 内部缓冲阈值
  };

  struct EffectiveConfig {
    Strategy strategy = Strategy::kRange;
    std::string field_name;                 // hash 时必填
    bool range_auto = true;                 // range 策略：auto 或 fixed
    int64_t range_rows = 0;                 // fixed 时 >0
    int64_t emit_batch_rows = 256;
  };

  int ResolveSinkPartitions(IChannel* out,
                            std::vector<std::shared_ptr<IStreamChannel>>* partitions);
  int ResolveEffectiveConfig(const DispatchConfig& in,
                             const arrow::Schema& schema,
                             size_t partition_count,
                             EffectiveConfig* out);
  int Dispatch(const arrow::RecordBatch& batch,
               const EffectiveConfig& cfg,
               const std::vector<std::shared_ptr<IStreamChannel>>& partitions);
  uint64_t HashFieldAt(const arrow::Array& col, int64_t row_index) const;
  int FlushPartitionBuffers(std::vector<std::shared_ptr<DataFrame>>& buffers,
                            const std::vector<std::shared_ptr<IStreamChannel>>& partitions,
                            int64_t ts_ms);

  DispatchConfig cfg_;
  std::string last_error_;
};
```

#### 12.11.2 参数解析状态机（`Configure` + `Work` 收敛）

1. `Configure` 阶段仅采集原始参数，不做最终语义推导。
2. `Work` 开始时调用 `ResolveEffectiveConfig` 一次性推导：
   - 无参数 -> `range(auto)`；
   - 仅 `field_name` -> `hash(field_name)`；
   - 其他必须 `has_strategy=true`，否则 `DATAFRAME_DISPATCH_STRATEGY_REQUIRED`。
3. 推导后做冲突校验（`hash+range_rows` 等），失败直接返回。

### 12.12 执行流程（函数级）

#### 12.12.1 `Work` 主流程

```text
1) 校验 in/out 类型：in 必须 IDataFrameChannel，out 必须 IStreamChannel
2) 解析 out 是否 stream_hub(split)，获取 partitions[]
3) 读入 DataFrame -> Arrow RecordBatch
4) ResolveEffectiveConfig(...)
5) Dispatch(batch, cfg, partitions)
6) 成功后 CloseStream（逐分区关闭）
7) 返回 0
```

#### 12.12.2 `ResolveSinkPartitions`

1. `out` 必须满足：
   - `dynamic_cast<IStreamChannel*>(out) != nullptr`
   - `IsHubChannel()==true`
   - `HubModeHint()=="split"`
   - `HubPartitionCount()>0`
2. 逐个 `HubPartition(i)` 取分区通道，构建 `partitions[]`。
3. 任一失败返回 `DATAFRAME_DISPATCH_INVALID_SINK` 或 `...PARTITION_INVALID`。

#### 12.12.3 `Dispatch` 分派算法

1. `round_robin`
   - 行级轮询：`p = row_index % N`；
   - 行写入 `buffers[p]`，达到 `emit_batch_rows` 刷新一次。
2. `hash`
   - 先定位 `field_name` 列索引；
   - `p = HashFieldAt(col[row]) % N`；
   - 行写入 `buffers[p]`，达到阈值刷新。
3. `range(fixed)`
   - `p = (row_index / range_rows) % N`；
   - 该策略是连续区间分配，可按区间批量写入，优先用 `RecordBatch::Slice` 降低行拷贝。
4. `range(auto)`
   - `total_rows=M, partitions=N`
   - `base=M/N, rem=M%N`
   - `i` 分区分配 `base + (i<rem?1:0)` 行，连续区间切片写入。

### 12.13 哈希规范（冻结）

为避免同值跨版本分区漂移，hash 输入与算法固定：

1. 算法：`FNV-1a 64-bit`（项目内自实现，不依赖外部库）。
2. 输入编码：
   - 整数/浮点：小端字节序；
   - 字符串：UTF-8 原始字节；
   - 布尔：`0/1` 单字节；
   - NULL：常量字节串 `"__NULL__"`。
3. 输出分区：`hash % partition_count`。

### 12.14 错误输出与调度映射

#### 12.14.1 算子错误文案规范

`LastError()` 采用前缀码格式：

```text
[DATAFRAME_DISPATCH_FIELD_NOT_FOUND] field not found: service_id
```

#### 12.14.2 调度层行为

1. `SchedulerPlugin::HandleExecute` 保持现有错误打包逻辑。
2. 算子失败时返回 `kOpExecFail + error_stage=execute`，并保留上面的错误前缀，便于前端/日志检索。

### 12.15 测试实现清单（代码级）

#### 12.15.1 e2e 用例（`test_scheduler_e2e.cpp`）

1. `T54.3`：`range(auto)`（无 WITH 参数）
   - SQL：`USING builtin.dataframe_dispatch_stream INTO stream_hub.x`
   - 断言：总量守恒、分区数据连续切段。
2. `T54.4`：`hash(field_name)`
   - SQL：`WITH field_name=service_id`
   - 断言：同 `service_id` 行落同分区。
3. `T54.5`：`range(fixed)`
   - SQL：`WITH strategy=range,range_rows=100`
   - 断言：`0-99->[0]，100-199->[1]`。
4. `T54.6`：参数冲突/缺失
   - `WITH range_rows=100`（无 strategy） -> `DATAFRAME_DISPATCH_STRATEGY_REQUIRED`
   - `WITH strategy=hash,range_rows=100,field_name=id` -> `DATAFRAME_DISPATCH_PARAM_CONFLICT`
5. `T54.7`：非法 sink
   - `INTO ring.xxx` -> `DATAFRAME_DISPATCH_INVALID_SINK`。

#### 12.15.2 回归范围

1. 既有 `T54.2`（group 中省略 USING）保持通过。
2. `dataframe -> ring` 既有路径行为不变（不引入隐式分派）。
3. 全量回归：`test_scheduler_e2e`、`test_task`、`test_builtin`。

### 12.16 实施顺序（可直接执行）

1. 新增算子头/实现文件与 `flowsql_common` 构建项。
2. 在 `builtin_registry.cpp` 注册算子与别名。
3. 先补 `T54.3~T54.7` 失败测试，再实现算子逻辑。
4. 运行回归并修正文档（README 能力矩阵）。
