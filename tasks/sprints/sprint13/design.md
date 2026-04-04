# Sprint 13 设计文档：流式控制面收敛与 Ring 并发补齐（Epic 14，Story 14.7~14.10）

## 背景与目标

Sprint 12 已完成路径 A（结构化列式流）基础能力，包含：

1. `IStreamChannel` / `IStreamOperator` / `StreamRuntime` 基础运行时。
2. `Scheduler` 侧流式任务执行与状态查询（`/tasks/stream/*`）。
3. Web 最小可用能力（Stream 通道只读展示）。

当前仍存在两类缺口：

1. **控制面缺口**：流式任务未纳入 `TaskPlugin` 统一入口，`Scheduler` 同时承担了任务管理语义。
2. **并发能力缺口**：`ring_mode=mpsc/mpmc` 仍是 `ENOTSUP` 占位。

Sprint 13 目标是围绕 Story 14.7/14.8/14.9/14.10，完成控制面收敛和并发能力补齐。

---

## 继承 Sprint 12 的设计约束

本 Sprint 直接继承 Sprint 12 已确认的约束，不引入双轨兼容：

1. 单轨语义：`INTO` 决定 sink 通道类型，Scheduler 绑定真实 sink 并透传。
2. URI 语义清晰优先：stream 走 stream 专属 URI，不做 dataframe 兜底。
3. `builtin.*` 对两段式数据库目标（`INTO <db_type>.<db_name>`）保持显式报错。
4. 不引入 V1/V2 或 shadow 路由。

---

## 当前实现与目标差距

### Story 14.8（跨进程流通道与 TaskPlugin 统一入口）

当前状态：

1. `Scheduler` 已直接暴露 `/tasks/stream/execute|stop|status|list`。
2. `TaskPlugin` 只管理批任务路由（`/tasks/batch/execute|sql/classify|list|detail|cancel|delete|diagnostics`）。
3. Web 端无 `/api/tasks/stream/*`。

差距：

1. 流式任务未统一归口到 `TaskPlugin`。
2. `Scheduler` 仍承担了对外任务管理职责。
3. 前后端对流式任务无统一管理链路。

### Story 14.10（Ring MPSC/MPMC）

当前状态：

1. `ParseRingMode` 可解析 `mpsc/mpmc`。
2. `AtomicRing` 仅 `SPSC/SPMC` 可用，`MPSC/MPMC` 返回 `ENOTSUP`。

差距：

1. `MPSC/MPMC` 无真实并发实现。
2. 缺少对应并发正确性和性能基线验证。

---

## 架构总览（Sprint 13）

### 职责分层

1. `TaskPlugin`：任务管理层（统一入口、任务状态持久化、错误与诊断聚合）。
2. `SchedulerPlugin`：执行引擎层（批/流执行、运行态快照、算子调度）。
3. `WebServer`：API 代理层（仅转发，不持有任务状态）。

### 路由分层

1. 任务管理路由：`/tasks/*`（由 `TaskPlugin` 统一提供）。
2. 执行路由：`/scheduler/*`（由 `SchedulerPlugin` 提供）。

说明：

1. 本分层仅用于职责边界与 URI 语义清晰，不引入“内部路由”新概念。
2. 管理面路由统一视为内部可见。
3. 通过前缀分离解决路由重复注册冲突，并避免 TaskPlugin 内部调度误命中自身 handler。

---

## URI 设计（确认版）

### 任务管理路由（TaskPlugin）

批任务：

1. `POST /tasks/batch/execute`
2. `POST /tasks/sql/classify`
3. `POST /tasks/list`
4. `POST /tasks/detail`
5. `POST /tasks/cancel`
6. `POST /tasks/delete`
7. `POST /tasks/diagnostics`

流任务：

1. `POST /tasks/stream/execute`
2. `POST /tasks/stream/stop`
3. `POST /tasks/stream/status`
4. `POST /tasks/stream/list`

### 执行路由（SchedulerPlugin）

1. `POST /scheduler/batch/execute`（替代现有 `instant` 命名）
2. `POST /scheduler/sql/classify`
3. `POST /scheduler/stream/execute`
4. `POST /scheduler/stream/stop`
5. `POST /scheduler/stream/status`
6. `POST /scheduler/stream/list`

### Stream 通道管理路由（SchedulerPlugin）

1. `POST /channels/stream/query`
2. `POST /channels/stream/add`
3. `POST /channels/stream/modify`
4. `POST /channels/stream/remove`

> 约束：Stream 通道查询统一为 `POST /channels/stream/query`，不再保留 `list` 路由。

补充约束（Sprint 13 增补）：

1. 项目处于构建阶段，不做双入口兼容；`/tasks/submit` 由 `/tasks/batch/execute` 直接替代。
2. SQL 工作台执行入口保持批流对称：batch 走 `/tasks/batch/execute`，stream 走 `/tasks/stream/execute`。
3. `POST /tasks/sql/classify` 仅用于前端交互提示，不作为后端执行正确性的唯一依据。

---

## Story 14.7 设计：路径 B 接口占位（`IBlockStream*`）

## 目标

仅补齐契约，不实现路径 B 数据面。

### 设计

1. 新增 `iblock_stream_channel.h`：
- 定义块状流读取协议（如 `PollBlock` / `ReleaseBlock` / `Cancel`）。
- `Type()` 对应 `block_stream`。

2. 新增 `iblock_stream_operator.h`：
- 定义路径 B 算子生命周期（`Init/ProcessBlock/Flush/LastError`）。

3. Scheduler 占位分支：
- 识别 `block_stream` 输入时返回 `BAD_REQUEST`。
- 错误响应包含固定业务码：`error_code=BLOCK_STREAM_NOT_IMPLEMENTED`。
- 错误消息统一为可定位文案，明确“路径 B 未实现”。

4. 测试要求：
- 最小编译通过。
- 调度入口命中 `block_stream` 时返回预期错误。

---

## Story 14.8 设计：跨进程流通道与 TaskPlugin 统一入口

## 目标

把流式任务管理语义从 Scheduler 收敛到 TaskPlugin，建立统一跨进程链路：

`Web -> TaskPlugin (/tasks/batch/*, /tasks/stream/*) -> Scheduler (/scheduler/*)`

### 14.8.1 TaskPlugin 设计

新增任务执行与分类 handler：

1. `HandleBatchExecute`
2. `HandleSqlClassify`

新增 4 个流任务 handler：

1. `HandleStreamExecute`
2. `HandleStreamStop`
3. `HandleStreamStatus`
4. `HandleStreamList`

行为约束：

1. `execute`：由 TaskPlugin 生成并管理 `task_id`，调用 Scheduler 执行。
- `task_id` 生成算法沿用现有实现（`MakeNowTaskId(seq)`），全链路统一。
2. `status/list`：优先返回 TaskPlugin 任务视图，可透传 Scheduler 运行态字段（`op_stats` 等）。
3. `stop`：更新任务控制状态并转发 Scheduler 停止请求。

### 14.8.2 Scheduler 设计

1. 原 `/tasks/stream/*` 改为 `/scheduler/stream/*`。
2. 新增 `POST /scheduler/sql/classify`，作为 TaskPlugin 的 SQL 类型判定单一来源。
3. 原 `/tasks/instant/execute` 改为 `/scheduler/batch/execute`。
4. `HandleStreamExecute` 不接收外部 `task_id`，仅返回 Scheduler 内部 `runtime_task_id`（用于 TaskPlugin 映射）。

### 14.8.3 WebServer 设计

新增对外 API：

1. `POST /api/tasks/batch/execute`
2. `POST /api/tasks/sql/classify`
3. `POST /api/tasks/stream/execute`
4. `POST /api/tasks/stream/stop`
5. `POST /api/tasks/stream/status`
6. `POST /api/tasks/stream/list`

实现方式：

1. WebServer 仅代理到 `TaskPlugin` 对外路由。
2. 状态码映射沿用现有 `ProxyPostJson` 规则（400/404/409/503/500）。

### 14.8.4 请求/响应契约

`POST /tasks/stream/execute`

请求：

```json
{
  "sql": "SELECT ... USING ... INTO ...",
  "timeout_s": 0
}
```

响应：

```json
{
  "task_id": "tsk_20260403_000001",
  "runtime_task_id": "stream_task_1712123456789_1",
  "status": "submitted"
}
```

说明：

1. `task_id` 为 TaskPlugin 业务任务 ID，沿用现有 `MakeNowTaskId(seq)` 算法。
2. `runtime_task_id` 为 Scheduler 运行态任务 ID，本 Sprint 统一响应字段名为 `runtime_task_id`（不保留旧字段名兼容分支）。
3. `execute` 响应中的 `status=submitted` 表示“请求已受理”的提交态，不作为持久化终态；任务视图状态由 `status` 接口返回。

`POST /tasks/stream/status`

请求：

```json
{
  "task_id": "tsk_20260403_000001"
}
```

响应（示例）：

```json
{
  "task_id": "tsk_20260403_000001",
  "runtime_task_id": "stream_task_1712123456789_1",
  "status": "running",
  "runtime_status": "running",
  "terminal_reason": "",
  "error_code": 0,
  "error_message": "",
  "processed_rows": 1234,
  "output_rows": 1220,
  "op_stats": {}
}
```

### 14.8.5 数据模型扩展（TaskPlugin 元数据）

建议在 `tasks` 表新增字段：

1. `task_kind TEXT NOT NULL DEFAULT 'batch'`（`batch|stream`）
2. `runtime_task_id TEXT NOT NULL DEFAULT ''`（与 Scheduler 运行态任务 ID 对应）

用途：

1. 统一批/流任务查询。
2. 支撑 `/tasks/list` 混合展示与过滤。
3. 保持任务管理唯一真相在 TaskPlugin。

补充约束：

1. TaskPlugin 是业务 `task_id` 的唯一分配方；Scheduler 不再承担业务 `task_id` 分配职责。
2. TaskPlugin 通过 `runtime_task_id` 关联 Scheduler 运行态，避免双主键来源。

### 14.8.6 Stream 状态映射与 `stopped/cancelled` 语义

TaskPlugin 任务视图与 Scheduler 运行态的状态映射需显式固定：

1. `submitted|pending|created -> pending`
2. `running|stopping -> running`
3. `stopped -> stopped`
4. `cancelled -> cancelled`
5. `failed -> failed`

语义约束：

1. `stopped`：用户发起 `stop` 后，任务完成有序收敛（允许 drain 完毕）进入终态。
2. `cancelled`：强制中断终态（例如超时、系统停机、运维强制取消），语义上不同于有序停止。
3. TaskPlugin 侧新增 `stopped` 终态，纳入终态集合与清理策略。
4. `status` 返回 TaskPlugin 视图状态，`runtime_status` 返回 Scheduler 原生状态，避免语义损失。

### 14.8.7 SQL 工作台执行入口对称化（Sprint 13 增补任务）

背景问题：

1. SQL 工作台若只依赖输入阶段的异步判定结果，存在“用户粘贴 SQL 后立即点击执行、判定结果尚未返回”的竞态窗口。
2. 流式 SQL 的执行语义是长运行任务，`sync` 交互在产品语义上不成立。
3. batch 与 stream 入口不对称时，执行链路与错误诊断不够直观。

目标：

1. 批流入口对称：batch 走 `/tasks/batch/execute`，stream 走 `/tasks/stream/execute`。
2. `classify` 用于 UI 提示，不承载最终正确性；点击执行时做强一致判定。
3. 流式 SQL 在 SQL 工作台仅允许异步执行（禁用/隐藏 `sync`）。

接口方案：

1. `POST /tasks/sql/classify`
- 请求：`{"sql":"..."}`。
- 响应：`{"task_kind":"batch|stream"}`。

2. `POST /tasks/batch/execute`
- 请求：`{"sql":"..."|"sqls":[...],"mode":"sync|async","timeout_s":0}`。
- 约束：仅接收 batch SQL；若判定为 stream SQL，返回 `BAD_REQUEST` + `STREAM_SQL_USE_STREAM_API`。

3. `POST /tasks/stream/execute`
- 请求：`{"sql":"...","timeout_s":0}`。
- 约束：仅接收 stream SQL；若判定为 batch SQL，返回 `BAD_REQUEST` + `BATCH_SQL_USE_BATCH_API`。

前端执行流程（SQL 工作台）：

1. 输入变化时使用 debounce 调用 `POST /api/tasks/sql/classify`，只用于更新 UI（模式提示、按钮状态）。
2. 点击执行时，如果 classify 状态为 `pending/unknown` 或与当前 SQL 版本不一致，先同步等待一次 classify 再执行。
3. 按点击时最新判定结果分流：batch -> `/api/tasks/batch/execute`；stream -> `/api/tasks/stream/execute`。
4. 当判定为 stream 时，前端强制 `executeMode=async`，并禁用/隐藏同步模式。

后端硬校验（防误路由）：

1. `batch/execute` 和 `stream/execute` 在执行前都做 SQL 类型校验，后端保留最终判定权。
2. 即使前端发生竞态，也不会出现“stream SQL 以 batch 语义执行”或“batch SQL 误入 stream 执行”的错误。
3. 错误码统一可诊断，便于前端提示与日志排查。

兼容性策略：

1. 本项目构建阶段不保留 `/tasks/submit` 兼容路径。
2. Web 端统一暴露 `/api/tasks/batch/execute` 与 `/api/tasks/stream/execute` 的对称入口。

测试补充：

1. “粘贴 SQL 后立即执行”场景下，执行入口选择正确。
2. stream SQL 无法走 `sync`。
3. `batch/execute` 调 stream SQL 与 `stream/execute` 调 batch SQL 均返回预期错误码。

---

## Story 14.9 设计：Web 流式管理页面完整化

## 目标

补齐 Stream 通道管理（增删改）和流任务可视化。

### 14.9.1 后端能力

新增 `IStreamManager` 接口（建议）：

1. `AddChannel(type, name, option)`
2. `ModifyChannel(type, name, option)`
3. `RemoveChannel(type, name)`
4. `QueryChannels()`

`StreamPlugin` 同时实现 `IStreamFactory + IStreamManager`：

1. 运行态通道实例管理。
2. 配置持久化与重启恢复。

### 14.9.2 持久化设计

建议新增表 `stream_channel_store`：

1. `type TEXT`
2. `name TEXT`
3. `option TEXT`
4. `status TEXT`
5. `updated_at DATETIME`
6. 唯一键 `(type, name)`

策略：

1. `stream_channel_store` 持久化到 `flowsql_meta.db`（与任务/算子元数据同库不同表）。
2. 数据库路径通过插件配置项指定，沿用现有配置风格（示例：`db_path=./meta/flowsql_meta.db`）。
3. 启动时优先加载 `stream_channel_store`。
4. YAML 仅用于首次导入（无库记录时）。
5. 若未配置可用 `db_path`，`add/modify/remove` 返回明确错误（拒绝无持久化后端的动态管理）。
6. SQLite 运行参数沿用现有元数据库策略（建议 WAL 模式），避免多插件共库写冲突放大。

### 14.9.3 Web 与前端

WebServer 新增代理：

1. `POST /api/channels/stream/add`
2. `POST /api/channels/stream/modify`
3. `POST /api/channels/stream/remove`
4. `POST /api/channels/stream/query`

前端新增能力：

1. `Channels.vue`：Stream 通道增删改弹窗与配置编辑。
2. 流任务视图默认并入 `Tasks.vue`（新增 Stream 分区/标签页）。
3. 若实现评估显示与批任务交互差异过大（表单与状态交互大面积分叉），再拆分为独立 `StreamTasks.vue`，路由与接口契约不变。

### 14.9.4 运行态一致性与操作矩阵

为避免运行态与持久态不一致，增删改需遵循统一约束：

1. `add`：
- 若 `(type,name)` 已存在，返回 `CONFLICT`。
- 先创建并 `Open` 运行态通道，成功后写入 `stream_channel_store`。
- 任一步失败即回滚，不保留半成功记录。

2. `modify`：
- 若通道被运行中任务引用，返回 `CONFLICT`（禁止热改）。
- 若未被引用，按“新建校验 -> 切换实例 -> 更新存储”执行。
- 切换失败时回滚到旧实例并保持旧配置。

3. `remove`：
- 若通道被运行中任务引用，返回 `CONFLICT`（禁止删除）。
- 若未被引用，先关闭运行态实例，再删除存储记录。

4. `query`：
- 返回 `status` 与 `in_use`（是否被任务引用）字段，便于前端判定按钮可用性。

### 14.9.5 TOCTOU 修复：`execute` 与 `modify/remove` 并发一致性

为消除 `in_use` 检查与实际占用之间的竞态，采用“短临界区 + 两阶段切换”：

1. 运行态控制结构：
- 每个通道维护 `ChannelCtl`：`in_use`、`mutating`、`remove_pending`、`lease_owner`（source 场景）。
- 读路径优先走无锁快照，写路径走细粒度互斥。

2. `execute`：
- 阶段 A（短临界区）：校验 `mutating/remove_pending`，原子增加 `in_use`，source 申请租约。
- 阶段 B（锁外）：任务装配与启动。
- 阶段 C（补偿）：若阶段 B 失败，回滚 `in_use` 与 source 租约。

3. `modify/remove`：
- 阶段 A（短临界区）：检查 `in_use==0` 且无租约，占位 `mutating/remove_pending=true`。
- 阶段 B（锁外）：创建新实例、`Open/Close`、持久化写入。
- 阶段 C（短临界区）：提交切换或回滚占位状态。

4. 死锁与性能约束：
- 固定锁顺序为“通道索引锁 -> ChannelCtl 锁”，禁止反向获取。
- 禁止在临界区内执行 `Open/Close`、HTTP、SQLite 等慢操作。
- 该策略仅影响管理面，不进入 `PollNext/Put` 热路径。

### 14.9.6 同一 source 的多任务并发消费策略（Sprint 13）

1. Sprint 13 采用 `exclusive lease`：同一 source 不允许被多个 stream 任务并行消费。
2. 若 source 已被占用，`execute` 返回 `CONFLICT`（建议错误码：`STREAM_SOURCE_IN_USE`）。
3. source 租约在任务终态统一释放，避免异常路径泄漏。
4. 未来支持多任务并行消费（shared/broadcast）单列后续专题，不在本 Sprint 落地。
5. 同一任务内“多 SQL 并行分支共享同一 source（广播消费）”需求同样不在本 Sprint 实施。

---

## Story 14.10 设计：Ring 并发模式补齐（`MPSC/MPMC`）

## 目标

将 `ring_mode=mpsc/mpmc` 从占位不可用升级为真实可用实现。

### 14.10.1 当前线程池调用基线（现状）

当前流式执行链路由三层并发组件组成：

1. `StreamRuntime` 线程池：`ready_queue + timer_queue + worker/timer threads`。
2. `ShardRunner::Step`：每次最多处理 `kBatchBudget=8`，`PollNext(0)` 无阻塞轮询。
3. 分发辅助线程：
- `STATELESS`：`SharedSpmcState` 单独分发线程（source -> SPMC queue）。
- `KEYED`：`FanOutStreamChannel` 单独分发线程（source -> N 个分区 ring）。

结论：当前线程池并不直接依赖 `MPSC/MPMC`，主路径仍以 `SPSC/SPMC` 为主。

### 14.10.2 Ring 算法补齐（数据结构层）

基于有界环形队列 slot-sequence 方案补齐四模式：

1. `SPSC`：单生产者入队 + 单消费者出队（保留现有快路径）。
2. `SPMC`：单生产者入队 + 多消费者 CAS 出队（保留现有实现）。
3. `MPSC`：多生产者 CAS 入队 + 单消费者出队（新增）。
4. `MPMC`：多生产者 CAS 入队 + 多消费者 CAS 出队（新增）。

关键点：

1. `head_`/`tail_` 仍保持单调递增，不复用语义位。
2. `Slot.seq` 作为可见性与占用状态判定，不引入额外锁。
3. CAS 失败路径增加轻量退让（`yield/backoff`），避免高并发下忙等放大。

### 14.10.3 对线程池调用方案的影响分析

#### A. 对 `StreamRuntime` 调度状态机的影响

1. **无接口改动**：`TrySchedule/WorkerLoop/TimerLoop` 状态机保持不变。
2. **无线程模型改动**：worker 数量、timer 线程、`kBatchBudget` 不因 14.10 直接变化。
3. **行为影响**：CAS 竞争升高时，`PollNext(0)` 返回 `kTimeout` 的概率可能上升，进而增加 `kStepNeedRetryLater` 和 timer_queue 压力。

#### A.1 为何移除 `SharedSpmcState`（设计必需性）

1. 当前 `STATELESS` 路径存在“源通道 -> `SharedSpmc` 二级队列 -> shard”双跳，增加一次内存复制与一次队列调度。
2. `SharedSpmc` 分发线程让 `STATELESS` 并发能力被“中间层兜底”，掩盖了源通道本身是否支持并发 `PollNext`，不利于能力边界清晰化。
3. Stop/Cancel 收敛路径变长（source cancel + dispatch drain + shard drain），定位收敛问题时链路更复杂。
4. 线程资源固定额外 +1（dispatch），在高并发任务并行运行时会放大线程切换与空转成本。
5. 移除后改为“多 shard 直接并发读取 source”，能把并发约束前置到通道能力校验阶段，失败早于运行期，语义更可控。

#### B. 对三种并行策略的影响

1. `NONE`：
- 通常仍是单消费者，`MPSC/MPMC` 仅作为通道能力增强，不改变调度拓扑。

2. `STATELESS`：
- 本 Sprint 直接纳入“去掉分发线程”改造：移除 `SharedSpmcState`，改为多个 shard 直接并发 `PollNext` 源通道。
- 新增前置校验：当 `parallelism > 1` 且策略为 `STATELESS` 时，源通道必须声明支持并发 `PollNext`，否则任务创建报错。
- 为避免共享 source 被单个 shard 提前 `Close`，引入 `StatelessSourceView` 作为装配层生命周期隔离包装（仅该场景使用）。
- 该改造减少额外 dispatch 线程和二次队列复制，降低上下文切换与内存压力。

3. `KEYED`：
- 当前依赖 `FanOut` 分发线程 + 每分区 `SPSC` ring，短期不变。
- `MPSC/MPMC` 不改变 keyed 的单分区单消费者语义，避免引入跨分区乱序风险。

#### C. 对 Stop/Cancel 语义的影响

1. 现有 `RequestStop -> input->Cancel -> runtime.TrySchedule` 流程保持。
2. 新增模式下重点关注“高竞争 + Cancel”时的收敛：
- 不允许出现 worker 永久自旋导致 `Join` 卡死。
- `PollNext` 必须在可预期时间内观察到取消或 drained。

### 14.10.4 落地策略（分阶段）

**阶段 1：Sprint 13 实施范围**

1. 仅补齐 `AtomicRing` 的 `MPSC/MPMC` 入队/出队实现。
2. `STATELESS` 场景移除 `SharedSpmcState`，改为 shard 直读 source。
3. 将 `mode_supported_` 从“仅 SPSC/SPMC”改为四模式支持。
4. `Open()` 不再因 `mpsc/mpmc` 返回 `ENOTSUP`。
5. 新增通道并发能力声明接口，用于调度装配阶段做硬性校验。
6. 并行场景增加 sink 并发 `Put` 能力门禁，不做隐式降级。

**阶段 2：后续可选优化**

1. 评估 `KEYED` 路径是否也可减少分发线程（当前不在 Sprint 13 范围）。
2. 评估 runtime 维度的自适应 backoff（按 `poll_timeouts` 动态调节重试节奏）。

### 14.10.5 IStreamChannel 能力模型（完整结构）

为避免调度阶段“隐式猜测通道能力”，本 Sprint 新增统一能力结构（建议放在 `istream_channel.h`）。

```cpp
enum class ProducerMode { SINGLE, MULTI };
enum class ConsumerMode { SINGLE, MULTI };
enum class OrderGuarantee { GLOBAL_FIFO, PER_PRODUCER_FIFO, PER_PARTITION_FIFO, NONE };
enum class BackpressurePolicy { DROP_ONLY, BLOCK_ONLY, DROP_OR_BLOCK };

struct StreamConcurrencyCaps {
  ProducerMode put_mode = ProducerMode::SINGLE;
  ConsumerMode poll_mode = ConsumerMode::SINGLE;
  uint32_t max_producers = 1;  // 0 表示不设上限
  uint32_t max_consumers = 1;  // 0 表示不设上限
  bool lock_free_put = false;
  bool lock_free_poll = false;
  bool cancel_wakeup_guaranteed = false;
};

struct StreamSemanticCaps {
  bool finite = false;
  bool supports_timeout_poll = true;
  bool supports_filter_pushdown = false;
  bool filter_requires_full_match = true;
  bool eof_reliable = true;
  OrderGuarantee ordering = OrderGuarantee::NONE;
  BackpressurePolicy backpressure = BackpressurePolicy::DROP_OR_BLOCK;
};

struct StreamPartitionCaps {
  bool has_partition_id = false;
  bool supports_route_by_partition_id = false;
  bool preserves_partition_order = false;
};

struct StreamChannelCapabilities {
  std::string channel_type = "stream";
  StreamConcurrencyCaps concurrency;
  StreamSemanticCaps semantics;
  StreamPartitionCaps partition;
};
```

并发能力接口契约（新增）：

```cpp
// IStreamChannel 默认返回“最保守能力”，避免历史通道未实现时出现误判
virtual StreamChannelCapabilities Capabilities() const {
  StreamChannelCapabilities caps;
  caps.channel_type = ChannelType::kStream;  // 复用已有通道类型常量
  caps.concurrency.put_mode = ProducerMode::SINGLE;
  caps.concurrency.poll_mode = ConsumerMode::SINGLE;
  caps.concurrency.max_producers = 1;
  caps.concurrency.max_consumers = 1;
  caps.concurrency.lock_free_put = false;
  caps.concurrency.lock_free_poll = false;
  caps.concurrency.cancel_wakeup_guaranteed = false;
  return caps;
}
```

说明：

1. 不新增 `StreamSinkKind`，通道类型继续复用 `ChannelType::{kDataFrame,kDatabase,kStream,kBlockStream}`。
2. `Capabilities()` 只描述能力，不描述配置意图（例如并发度目标由任务参数决定）。
3. 调度阶段必须“按能力硬校验”，不得隐式降级或自动插入兼容层。
4. `cancel_wakeup_guaranteed` 默认值为 `false`，仅由具备该能力的通道实现显式声明 `true`。

接口扩展：

1. `IStreamChannel::Capabilities() const`（默认返回最保守能力：单生产者 + 单消费者）。
2. `RingStreamChannel` 按 `ring_mode` 返回能力：
- `spsc`：`put=SINGLE, poll=SINGLE`
- `spmc`：`put=SINGLE, poll=MULTI`
- `mpsc`：`put=MULTI, poll=SINGLE`
- `mpmc`：`put=MULTI, poll=MULTI`
3. `TcpSessionMockStreamChannel` 透传内部 queue 的能力。
4. `FanInStreamChannel` 改为 ring 直读聚合（不再维护内部合并队列），默认声明 `poll=SINGLE`。

### 14.10.6 Scheduler 装配判定规则（含 STATELESS 改造）

`SchedulerPlugin::ExecuteStreamTask` 增加能力校验，拒绝隐式降级。

判定规则：

1. `strategy=NONE`：
- 不要求 `poll=MULTI`，按单消费者装配。

2. `strategy=STATELESS && parallelism > 1`：
- 要求 `source.Capabilities().concurrency.poll_mode == MULTI`。
- 要求 `max_consumers == 0 || max_consumers >= parallelism`。
- 不满足则任务创建失败，返回 `BAD_REQUEST` + 明确错误文案（包含期望能力与实际能力）。

3. `strategy=KEYED`：
- 维持 `FanOut` 分区语义，不改变单分区单消费者约束。
- 若 `parallelism > 1`，仍要求共享 sink 支持并发 `Put`。

4. `parallelism > 1` 的 sink 并发门禁（统一规则）：
- 若 `sink_type == stream`，要求 `sink_stream.Capabilities().concurrency.put_mode == MULTI`。
- 若 `sink_type != stream`，默认 `put_mode=SINGLE`、`max_producers=1`。
- 要求 `max_producers == 0 || max_producers >= parallelism`。
- 不满足则直接创建失败（`BAD_REQUEST`），不允许自动降级到 `NONE/1`。
- 说明：本 Sprint 对未声明并发 `Put` 能力的 sink，按 `SINGLE` 处理。该限制标记为 Sprint 13 范围约束，后续增强优先级低。

### 14.10.6.1 `StatelessSourceView` 装配层设计（仅共享 source 并发消费）

适用条件：

1. `strategy=STATELESS && parallelism > 1` 且多个 shard 共享同一个 source。

职责：

1. 生命周期隔离：每个 shard 拿到一个 `StatelessSourceView`，避免 shard 级 `Finalize()->Close()` 直接关闭共享 source。
2. 共享取消：任一 view `Cancel()` 触发一次 source `Cancel()`（once 语义）。
3. 延迟关闭：view `Close()` 仅做引用计数，最后一个 view 关闭时才触发 source `Close()`。
4. 数据路径零复制：`PollNext()` 直接转发到共享 source，不新增中间队列。

非适用场景：

1. `NONE`（单消费者直连）不使用 `StatelessSourceView`。
2. `KEYED` 使用既有 `FanOutPartitionView`。

标准错误返回（统一约束）：

1. 错误码：`BAD_REQUEST`（参数/能力不匹配）。
2. 错误消息模板：
`stream source capability mismatch: strategy=<...>, parallelism=<...>, required.poll_mode=MULTI, actual.poll_mode=<...>, actual.max_consumers=<...>`
3. 错误消息模板（sink）：
`stream sink capability mismatch: strategy=<...>, parallelism=<...>, required.put_mode=MULTI, actual.put_mode=<...>, actual.max_producers=<...>`
4. 诊断字段：`details.capabilities` 回填 source/sink 实际能力结构，便于 Web 与日志定位。

实现改动：

1. 删除 `SharedSpmcState` / `SharedSpmcInputView` 装配路径。
2. `STATELESS` 下 `input_ports` 使用 `StatelessSourceView` 包装共享 `source`（多 shard 并发 `PollNext`，生命周期隔离）。
3. 增加 sink 并发能力门禁，不满足时在任务创建阶段失败。
4. `StreamRuntime/ShardRunner` 状态机保持不变。

### 14.10.6.2 `FanInStreamChannel` 无锁聚合改造（Ring-only）

目标：

1. 将 `FanInStreamChannel` 从“forward 线程 + 互斥合并队列”改为“多 ring 输入直读聚合”。
2. 数据路径不新增中间缓存层，避免额外锁竞争与线程开销。

适用范围（Sprint 13）：

1. 仅支持输入源为 ring 能力通道（`RingStreamChannel` 或声明等效 ring 能力的实现）。
2. 输入能力不满足时直接创建失败（`BAD_REQUEST`），不回退到历史带锁合并路径。

数据路径与并发模型：

1. 删除 `FanIn` 内部 `merged_queue`、`queue_mu`、`forward_threads`。
2. `FanIn::PollNext(timeout)` 在调用线程内轮询各输入 `PollNext(0)`。
3. 使用 round-robin 游标作为轮询起点，降低固定序扫描带来的饥饿风险。
4. 命中 `kData` 立即返回，`StreamBatch` 内 `shared_ptr<RecordBatch>` 直接透传（零深拷贝）。
5. `timeout_ms>0` 时按截止时间循环轮询，采用轻量退让（`yield/短暂 sleep`）抑制空转。

语义约束：

1. 保证每个输入分支内部顺序（per-source FIFO）不被破坏。
2. 不保证跨输入分支的全局顺序（interleaving 由到达时序决定）。
3. 任一输入返回 `kError` 时按 fail-fast 处理，记录首错并返回错误事件。
4. EOF 规则为“所有输入均 EOF 后返回整体 EOF”。
5. `Cancel()` 仅向各输入透传，不引入额外 dispatch/join 收敛链路。

能力声明与装配门禁：

1. `FanIn::Capabilities()` 在 Sprint 13 固定为 `poll=SINGLE`（单消费者聚合视图）。
2. Scheduler 在构建 `FanIn` 时校验各输入具备 ring 能力与合法 `PollNext` 超时语义。
3. 能力不匹配时返回错误模板：
`stream fanin capability mismatch: source=<...>, reason=<...>`

实现改动：

1. 删除 `FanInStreamChannel` 里的合并队列与转发线程实现。
2. 新增输入状态位图（`done/error`）与 round-robin 游标。
3. 保持 `IStreamChannel` 接口签名不变，减少上层改动面。
4. 更新 `Capabilities()` 返回值与对应单元测试断言。

### 14.10.7 对线程池调用方案的具体影响

#### 变更前后对照（STATELESS）

1. 变更前：
- 线程构成：`N worker + 1 timer + 1 SharedSpmc dispatch`。
- 数据路径：`source -> SharedSpmc queue -> shard`。

2. 变更后：
- 线程构成：`N worker + 1 timer`（去掉 dispatch 线程）。
- 数据路径：`source -> StatelessSourceView(shard)`（直读转发，无中间队列）。

#### 对 `StreamRuntime` 的影响

1. `TrySchedule/WorkerLoop/TimerLoop` 代码路径不变。
2. worker 侧 `PollNext(0)` 的 CAS 竞争会增加，可能提高 `kStepNeedRetryLater` 比例。
3. timer queue 压力可能上升，需要通过指标观测决定是否调优 `kBatchBudget` 或 backoff 策略。

#### 线程池调优建议（Sprint 13）

1. 默认保留 `kBatchBudget=8`，先观察指标。
2. 若 `poll_timeouts` 明显上升且 CPU 空转，优先尝试：
- 降低 `stream_workers`
- 增大 `ring_size`
- 增加轻量 backoff（仅在 CAS 连续失败场景）
3. 不在本 Sprint 改动 `StreamRuntime` 状态机与队列模型。

### 14.10.8 验收与测试策略（增强）

并发正确性：

1. 无丢失：消费计数等于生产计数。
2. 无重复：消息 ID 去重后数量一致。
3. 可收敛：生产结束后消费者可到达 EOF/Drained。

稳定性：

1. Stop/Cancel 并发触发下无死锁、无悬挂。
2. 在高竞争 (`producer>=4, consumer>=4`) 下 `Join` 有明确超时上界。
3. `FanIn` 打开/关闭循环后无后台线程泄漏。

能力判定回归：

1. `STATELESS + parallelism>1` 且 `poll_mode=SINGLE` 必须创建失败。
2. `STATELESS + parallelism>1` 且 `poll_mode=MULTI` 必须创建成功。
3. `parallelism>1` 且 sink `put_mode=SINGLE` 必须创建失败（禁止隐式降级）。
4. 错误消息包含能力字段（`poll_mode/max_consumers`、`put_mode/max_producers`）便于定位。
5. `FanIn` 非 ring 输入必须创建失败，且错误文案可定位。

线程池影响回归：

1. 监控 `poll_timeouts`、`poll_errors`、`queue_depth_peak`，对比改造前后趋势。
2. 验证 timer_queue 不出现异常堆积（长时间 `kWaitingRetry` 不回收）。
3. 监控流任务线程数量变化，确认移除 `SharedSpmc` 后无额外分发线程泄漏。

性能基线：

1. 对比 `SPSC/SPMC/MPSC/MPMC` 吞吐、P99 延迟、CPU 占用。
2. 补充“STATELESS 直读前后”对比（吞吐、延迟、线程数）。
3. 输出基线报告，并记录推荐模式与适用场景。

---

## 测试与验收计划

核心验证命令：

```bash
cmake --build build -j4 --target flowsql test_stream test_scheduler_e2e
ctest --test-dir build -R test_stream --output-on-failure
ctest --test-dir build -R test_scheduler_e2e --output-on-failure
npm --prefix src/frontend run build
```

新增测试项：

1. `test_stream`：
- `MPSC/MPMC` 正确性与收敛测试。
- Stop/Cancel 稳定性。
- source 独占租约：同一 source 双任务并发执行时，第二个任务返回 `CONFLICT`。
- `FanIn` ring-only 无锁聚合回归：无丢失、无重复、per-source 顺序保持、无线程泄漏。

2. `test_scheduler_e2e`：
- `/tasks/stream/*`（TaskPlugin 对外）链路测试。
- `/scheduler/stream/*`（Scheduler 内部）执行链路测试。
- `task_id` 前缀与外部 `task_id` 透传拒绝测试。
- `stop` 成功终态为 `stopped`，与 `cancelled` 语义分离测试。
- `execute` 与 `modify/remove` 并发竞态回归（无 TOCTOU、无脏状态）。

3. Web 冒烟：
- `/api/tasks/stream/*` 与 `/api/channels/stream/*` 全链路。

---

## 实施顺序建议

1. 先改 14.8：完成路由分层和统一入口（最高优先级）。
2. 再改 14.9：补齐 Stream 管理和前端可视化。
3. 后改 14.10：并发实现与压测。
4. 14.7 可并行完成（低风险占位）。

---

## 风险与缓解

1. 路由迁移风险：旧 URI 下线导致调用方失效。  
缓解：同一次迭代同步更新 Web API、前端 API 和 E2E。

2. TaskPlugin 与 Scheduler 状态一致性风险。  
缓解：以 TaskPlugin 为任务元数据真相，Scheduler 仅提供运行态快照。

3. MPMC 并发竞态风险。  
缓解：先补强压力测试，再逐步替换模式分支。

4. `execute` 与 `modify/remove` 并发更新导致 TOCTOU 风险。  
缓解：短临界区两阶段切换 + 原子占用计数 + 失败补偿回滚。

5. 管理面锁策略引入死锁或性能退化风险。  
缓解：固定锁顺序、禁止锁内慢操作、并发路径压测与锁等待观测。

6. `FanIn` 无锁轮询在低流量下可能引入空转 CPU 开销。  
缓解：采用截止时间轮询 + 轻量退让，并纳入 `poll_timeouts` 与 CPU 指标回归。

---

## 后续议题（不在 Sprint 13 范围）

1. 同一 source 的并发消费（含同任务多 SQL 广播与跨任务 shared/broadcast）后续专题设计，本 Sprint 不实施。
