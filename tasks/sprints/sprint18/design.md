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
