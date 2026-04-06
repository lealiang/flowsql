# Sprint15 Refactoring - 代码走读检视报告

## 1. 结论摘要

当前项目在功能演进上进展明显，但并发安全与结构复杂度已进入需要优先治理的阶段。  
本报告将问题分为 `P0/P1/P2` 三个级别：

- `P0`：必须先修复，否则存在稳定性/正确性风险
- `P1`：建议本迭代完成，降低后续维护和回归成本
- `P2`：可纳入持续重构项

---

## 2. P0（必须修复）

### P0-1 `SchedulerPlugin::channels_` 并发访问存在数据竞争

- 问题描述：
  - `channels_` 为共享 `unordered_map`，存在并发读写路径，但无统一互斥保护。
  - 写路径包括 `RegisterChannel`、组任务清理时 `erase`；读路径包括 `FindChannel`、预览接口。
  - `StreamTaskGroup` 后台线程会通过回调进入执行路径，导致管理面与执行面可并发访问该容器。
- 影响：
  - 触发未定义行为，表现为偶发崩溃、通道查找异常、难复现段错误。
- 修改建议：
  - 在 `SchedulerPlugin` 中新增 `channels_mu_`（建议 `std::shared_mutex`）。
  - 所有 `channels_` 读取使用共享锁，写入/删除使用独占锁。
  - 中期可考虑将内部通道统一收敛到线程安全 registry，减少散落访问。
- 关键证据：
  - `src/services/scheduler/scheduler_plugin.h`（`channels_` 定义）
  - `src/services/scheduler/scheduler_plugin.cpp`（`RegisterChannel` / `FindChannel`）
  - `src/services/scheduler/scheduler_stream_group.cpp`（`channels_.erase`）
  - `src/services/scheduler/stream_task_group.cpp`（后台线程执行）

### P0-2 `TaskPlugin` 单 SQLite 连接跨线程共享，状态更新判定存在竞态风险

- 问题描述：
  - `TaskPlugin` 里 API 线程、worker 线程、timeout 线程共享一个 `sqlite3* db_`。
  - 代码大量依赖 `sqlite3_changes(db_)` 判定更新是否生效，同时包含显式事务（`BEGIN IMMEDIATE`）。
  - 即便 SQLite 连接是 `FULLMUTEX`，也只能保证内部线程安全，无法保证业务层“本次语句对应本次 changes”这种语义一致性。
- 影响：
  - 状态迁移可能误判（例如取消、删除、终态更新）。
  - 在高并发下，可能出现难定位的任务状态不一致问题。
- 修改建议：
  - 短期：增加 `db_mu_`，将 `prepare-bind-step-changes-finalize` 及事务语句作为原子临界区。
  - 中期：拆分为 DB 专用执行线程（队列化）或读写分连接架构。
  - 对关键状态迁移（pending/running/terminal）补充并发一致性测试。
- 关键证据：
  - `src/services/task/task_plugin.h`（`sqlite3* db_` + 多线程成员）
  - `src/services/task/task_plugin.cpp`（`Start/WorkerLoop/TimeoutLoop`）
  - `src/services/task/task_plugin.cpp`（`UpdateStatus/DeleteTask/HandleCancel` 中 `sqlite3_changes` 与事务）

---

## 3. P1（建议本迭代修复）

### P1-1 `StreamTaskGroup` 持锁调用外部回调，锁占用时间不可控

- 问题描述：
  - `submit_fn_`、`query_fn_`、`stop_fn_`、`pre_stop_fn_` 在 `mu_` 持有期间被调用。
- 影响：
  - 锁竞争加重，状态查询与 stop 响应可能抖动。
  - 回调路径扩展后易形成锁顺序耦合，埋下死锁隐患。
- 修改建议：
  - 改为“两阶段模型”：锁内计算动作并记录，释放锁后执行回调，再锁内提交状态变更。

### P1-2 前后端 SQL 分句器重复实现且语义漂移

- 问题描述：
  - 前端 `Tasks.vue` 自行实现 `splitSqlStatements`，后端使用 `SplitSqlText`。
  - 当前空语句处理策略已不一致（前端忽略，后端报错）。
- 影响：
  - 前端判定与后端真实执行不一致，用户体验为“前端看似可执行，后端拒绝”。
- 修改建议：
  - 统一以后端分句规则为准。
  - 前端不再维护独立 lexer，改调用后端 split/classify 结果（或共享同一实现）。

### P1-3 Scheduler 运行态任务对象缺少回收策略

- 问题描述：
  - `stream_tasks_`、`stream_task_groups_` 长期累积，终态后主要释放 lease，但对象未按策略淘汰。
- 影响：
  - 长时间运行后内存增长、列表遍历成本上升。
- 修改建议：
  - 增加运行态 retention（按时间或数量）。
  - 将终态摘要持久化后淘汰内存对象。

---

## 4. P2（持续优化项）

### P2-1 God File 问题明显，职责边界不清

- 问题描述：
  - `scheduler_plugin.cpp`、`task_plugin.cpp` 体量过大，且混合路由、参数校验、业务流程、状态管理、存储访问。
- 影响：
  - 评审成本高，修改牵一发而动全身。
- 修改建议：
  - 按职责拆分：`routes`、`executor`、`store`、`channel_admin`、`runtime_sync`、`json_codec`。

### P2-2 错误 JSON 构造与字符串工具重复较多

- 问题描述：
  - `MakeErrorJson*` / `JsonError*` 在 scheduler/task/stream_group 多处重复。
- 影响：
  - 结构字段不一致、错误码口径不统一。
- 修改建议：
  - 抽公共 `json_error_builder` 与统一 `error_code -> http/status` 映射。

### P2-3 Scheduler 路由查找存在重复实现和重复遍历

- 问题描述：
  - `TaskPlugin` 多处通过 `Traverse + EnumRoutes` 动态查找 scheduler handler。
  - 同类逻辑在 `ProxySchedulerPost` 与局部执行路径重复实现。
- 影响：
  - 多 SQL 与轮询场景下增加不必要开销，维护负担上升。
- 修改建议：
  - 在 `Load/Start` 缓存关键路由 handler，并统一调用入口。

---

## 5. 测试缺口

当前测试以功能路径为主，并发与一致性方面仍有缺口：

- 缺少 `channels_` 并发读写压力测试（建议配合 TSAN）。
- 缺少 `TaskPlugin` 数据库状态迁移并发一致性测试（特别是 `cancel/delete/update` 交错）。
- 缺少长时间运行下的运行态对象回收验证测试。

相关测试文件：

- `src/tests/test_scheduler_e2e/test_scheduler_mutation_guard.cpp`
- `src/tests/test_scheduler_e2e/test_scheduler_e2e.cpp`
- `src/tests/test_task/test_task.cpp`

---

## 6. 建议的 Sprint15 重构落地顺序

1. `P0` 并发安全加固  
   `channels_` 加锁治理 + `TaskPlugin` DB 访问串行化（或统一执行线程）
2. `P1` 行为一致性收敛  
   SQL 分句规则统一、运行态对象回收策略落地
3. `P2` 结构化重构  
   大文件拆分、错误编码与 JSON 构造统一、路由代理收敛

该顺序可以在最小化回归风险的前提下，先解决“会出错”的问题，再解决“难维护”的问题。

---

## 7. P0 细化实施方案（Sprint15 优先落地）

### 7.1 P0-1 细化方案：`SchedulerPlugin::channels_` 并发安全与生命周期安全

#### 7.1.1 改造目标

- 消除 `channels_` 的并发读写数据竞争。
- 消除“返回 raw pointer 后容器元素被并发 `erase`”导致的悬空指针风险。
- 在不改变业务语义的前提下，尽量小改动落地。

#### 7.1.2 方案选择

- 本 Sprint 采用“低风险快速收敛”方案：
  - 增加 `channels_mu_` 保护 `channels_`。
  - 新增统一 helper，禁止散落直接访问 `channels_`。
  - `Find` 路径改为返回 `std::shared_ptr<IChannel>`（或由调用方保存 owner）保证生命周期。
- 暂不做“迁移到独立线程安全 registry”的结构重构，作为后续优化项。

#### 7.1.3 数据结构与接口调整

- 在 `SchedulerPlugin` 中新增：
  - `mutable std::mutex channels_mu_;`（如后续需要再升级为 `std::shared_mutex`）。
- 新增/调整通道访问 helper（命名可按代码风格微调）：
  - `RegisterManagedChannel(...)`
  - `EraseManagedChannel(...)`
  - `ClearManagedChannels()`
  - `FindManagedChannelShared(...)`
  - `SnapshotManagedChannels(...)`

#### 7.1.4 代码改造清单（函数级）

- `src/services/scheduler/scheduler_plugin.h`
  - 增加 `channels_mu_` 和 helper 声明。
- `src/services/scheduler/scheduler_plugin.cpp`
  - `RegisterChannel`：改为受锁写入（或替换为新 helper）。
  - `FindChannel`：内部 map 查询改为受锁；优先返回 owner（`shared_ptr`）给调用链持有。
  - `HandleGetChannels`：改为“锁内快照、锁外序列化”。
  - `HandlePreviewDataframe`：内部 map 查询改为受锁。
  - `CleanupGroupRuntimeResources`：`channels_.erase` 改为受锁删除 helper。
  - `Stop`：`channels_.clear()` 改为受锁清理 helper。
- `src/services/scheduler/scheduler_stream_group.cpp`
  - 本地 `cleanup_local_resources` 中 `channels_.erase` 改为调用删除 helper。

#### 7.1.5 并发约束与锁顺序

- 约束 1：持有 `channels_mu_` 时禁止调用外部接口（`Traverse/List/Open` 等潜在重入路径）。
- 约束 2：`channels_mu_` 仅用于 map 访问，临界区只做常量时间操作。
- 约束 3：避免与 `stream_*_mu_` 形成交叉锁依赖；若必须同时使用，固定顺序并文档化（推荐先 `stream_*` 再 `channels_mu_`，且尽量避免）。

#### 7.1.6 测试与验收

- 单元测试：
  - 并发 `Register/Erase/Find` 压测，验证无崩溃、无异常返回。
  - group 启停与通道增删并发场景回归。
- 工具验证：
  - TSAN（优先）检查 `channels_` 相关 data race 为 0。
  - ASAN 回归无新增 UAF。
- 验收标准：
  - 不再出现 `channels_` 竞态告警或偶发段错误。
  - 现有 stream/batch 相关 e2e 用例全通过。

---

### 7.2 P0-2 细化方案：`TaskPlugin` SQLite 并发一致性治理

#### 7.2.1 改造目标

- 保证 `prepare-bind-step-changes-finalize` 语义原子，避免跨线程 `sqlite3_changes` 误判。
- 保证 `SELECT -> UPDATE` 等复合状态迁移在进程内串行执行。
- 不改变对外 API 与任务状态语义。

#### 7.2.2 方案选择

- 本 Sprint 采用“单连接 + 进程内串行化”方案：
  - 新增 `db_mu_`，统一保护所有 `db_` 访问。
  - 对存在嵌套调用的方法拆分 `NoLock` 内部函数，避免重入死锁。
- 暂不引入 DB actor 线程模型，避免本迭代改动面过大。

#### 7.2.3 数据结构与接口调整

- 在 `TaskPlugin` 新增：
  - `mutable std::mutex db_mu_;`
- 内部函数拆分（示意）：
  - `WriteTaskEventNoLock(...)`
  - `RunRetentionCleanupNoLock()`
  - 其他被 `UpdateStatus/DeleteTask/HandleCancel` 复用的 DB 逻辑按需下沉为 no-lock 版本。

#### 7.2.4 代码改造清单（函数级）

- `src/services/task/task_plugin.h`
  - 增加 `db_mu_` 与 `NoLock` helper 声明。
- `src/services/task/task_plugin.cpp`
  - `EnsureDb/EnsureSchema/CleanupOrphans`：受 `db_mu_` 保护。
  - `CreateTaskInternal`：DB 写入在 `db_mu_` 内完成；出锁后再操作任务队列 `mu_`。
  - `UpdateStatus`：`SELECT + UPDATE + changes + 事件写入 + retention` 统一在同一 DB 临界区完成。
  - `GetTask/ListTasks/ListTasksByKind/HandleDiagnostics`：读取统一受 `db_mu_` 保护。
  - `DeleteTask`：事务全程在 `db_mu_` 保护下执行，避免交叉语句污染。
  - `HandleCancel`：`UPDATE ... status='running'` + `changes` 在 `db_mu_` 内。
  - `TimeoutLoop`：轮询 SQL 与状态更新相关 DB 操作纳入 `db_mu_` 规则。
  - `Stop`：先停 worker/timeout 并 `join`，再在 `db_mu_` 下关闭连接。

#### 7.2.5 锁顺序与死锁规避

- 原则 1：禁止 `db_mu_` 与 `mu_`（队列锁）交叉持有。
- 原则 2：持有 `db_mu_` 时禁止调用外部路由（如 `ProxySchedulerPost`）。
- 原则 3：需要 DB + 外部调用的流程分两段处理：先 DB（短临界区）后外部调用，再回到 DB。

#### 7.2.6 测试与验收

- 并发一致性测试新增场景：
  - 并发 `cancel/status/delete` 交错，验证状态机无非法跃迁。
  - 并发 `timeout + worker update`，验证终态唯一性。
  - 并发创建与列表查询，验证无脏读崩溃。
- 回归测试：
  - `src/tests/test_task/test_task.cpp` 全量通过。
  - scheduler e2e 与 web 基础流程回归通过。
- 工具验证：
  - TSAN/ASAN 无新增并发与内存问题。
- 验收标准：
  - 不再出现 `sqlite3_changes` 误判导致的状态错乱。
  - 高并发下任务状态稳定可复现、可解释。

---

### 7.3 P0 实施前风险收敛补充（必须满足）

#### 7.3.1 生命周期闭环补充（P0-1 必修）

- 问题：
  - 仅“加锁访问 `channels_`”不能解决执行期悬空引用问题。
  - 当前执行链路中仍存在 no-op `shared_ptr` 持有 raw channel 的路径。
- 必须补充的设计约束：
  - `SourceResolveResult` 增加 owner 持有集合（例如 `std::vector<std::shared_ptr<IChannel>> owners`）。
  - `ResolveSourceBindings` 对来自 `channels_` 的通道必须放入 `owners`，并在执行结束前保持存活。
  - `SinkBinding` 对来自 `channels_` 的 sink 必须持有 map 中真实 `shared_ptr`，禁止仅用 raw + no-op deleter。
  - `FindChannel` 增加 owner 版本（如 `FindChannelShared`），禁止新代码直接基于 raw pointer 建立长期引用。
- 验收标准：
  - 执行中并发 `modify/remove` 内部通道不出现 UAF。
  - TSAN/ASAN 下无新增生命周期相关告警。

#### 7.3.2 `db_mu_` 与 `NoLock` 调用闭环补充（P0-2 必修）

- 问题：
  - 仅列出 `WriteTaskEventNoLock/RunRetentionCleanupNoLock` 不足以避免嵌套锁风险。
- 必须补充的设计约束：
  - 建立统一规则：`*NoLock` 函数绝不加锁，只允许在已持有 `db_mu_` 的上下文调用。
  - 关键链路必须成对拆分：
    - `GetTask` / `GetTaskNoLock`
    - `DeleteTask` / `DeleteTaskNoLock`
    - `UpdateStatus` / `UpdateStatusNoLock`
    - `WriteTaskEvent` / `WriteTaskEventNoLock`
  - 在 `TaskPlugin` 文档与代码注释中补充“锁归属约定”，避免后续维护误用。
- 验收标准：
  - 不存在 `db_mu_` 的递归加锁路径。
  - 并发 `cancel/delete/status` 压测中无死锁、无状态写入丢失。

#### 7.3.3 Retention 与热路径解耦补充（P0-2 强烈建议）

- 问题：
  - `UpdateStatus` 临界区内执行 retention 清理会放大锁占用时间。
- 设计调整：
  - `UpdateStatus` 只负责状态落库与事件记录，不直接执行 retention 清理。
  - retention 改为后台触发（周期任务或阈值触发批处理），独立于热路径。
  - retention 执行时限制单轮清理量，避免长事务阻塞。
- 验收标准：
  - 高并发状态更新时，`status/list/cancel` 延迟稳定。
  - retention 开启后无明显尾延迟抖动。

#### 7.3.4 全局锁序表补充（P0-1/P0-2 共同约束）

- 必须新增的锁顺序定义（文档级 + 代码注释级）：

| 锁名 | 允许嵌套 | 备注 |
|---|---|---|
| `db_mu_` | 不与 `mu_` 同时持有 | DB 临界区禁止外部调用 |
| `mu_`（Task 队列锁） | 不与 `db_mu_` 同时持有 | 仅用于队列操作 |
| `channels_mu_` | 尽量不与 `stream_*_mu_` 嵌套 | 仅保护 `channels_` map |
| `stream_*_mu_` | 可内部短嵌套，需固定顺序 | 禁止跨层长临界区 |
| `stream_channel_refs_mu_` | 与 `channels_mu_` 禁止交叉长持有 | lease/ref 独立保护 |

- 固定规则：
  - 持锁期间不得调用外部回调或插件接口。
  - 若确需嵌套，必须在文档中声明顺序并在代码处注释。
- 验收标准：
  - 代码审查可按锁序表机械检查。
  - 并发回归中无锁序反转导致的死锁。

---

## 8. P0 实施顺序建议（执行级）

1. 先落地 P0-1：`channels_` 并发安全 + 生命周期修复（影响范围小，收益直接）。  
2. 再落地 P0-2：`db_mu_` 串行化 + no-lock helper 拆分（改动较大，需分批提交）。  
3. 最后补并发测试与 sanitizer 验收，形成 Sprint15 基线。

---

## 9. P1 细化实施方案（P0 稳定后执行）

### 9.1 P1-1 细化方案：`StreamTaskGroup` 回调改为“两阶段执行”

#### 9.1.1 改造目标

- 将 `submit_fn_ / query_fn_ / stop_fn_ / pre_stop_fn_` 从 `mu_` 临界区中移出。
- 缩短锁持有时间，避免状态面（`Snapshot/Status`）被慢回调阻塞。
- 保持当前节点状态语义与 stop/cancel 行为不变。

#### 9.1.2 设计方案

- 统一采用“两阶段模型”：
  - 阶段 A（锁内）：只做状态判定、动作收集、最小状态标记。
  - 阶段 B（锁外）：执行外部回调（submit/query/stop/pre-stop）。
  - 阶段 C（锁内）：回写回调结果并推进状态机。
- 关键点：
  - `TrySubmitReadyNodes` 增加“提交中”保护标记（例如 `submit_inflight`），避免并发轮次重复提交。
  - `RefreshNodeStates` 先收集 `runtime_task_id` 列表，锁外查询快照，锁内按 `runtime_task_id` 合并。
  - `HandleStopSignal` 锁内标记 `stop_sent` 与目标状态，锁外批量执行 `stop_fn_`。
  - `pre_stop_fn_` 由锁内置位、锁外调用，保证只触发一次。

#### 9.1.3 代码改造清单（函数级）

- `src/services/scheduler/stream_task_group.h`
  - `NodeState` 增加 in-flight 状态位（如 `submit_inflight`）。
  - 按需补充辅助结构（提交动作/查询动作）。
- `src/services/scheduler/stream_task_group.cpp`
  - `TrySubmitReadyNodes`：改为两阶段提交。
  - `RefreshNodeStates`：改为两阶段查询与合并。
  - `HandleStopSignal`：改为锁外执行 `stop_fn_` 与 `pre_stop_fn_`。
  - 仅在锁内修改 `nodes_`、`error_*`、`status_`，外部回调全部锁外。

#### 9.1.4 并发约束与死锁规避

- 禁止在持有 `mu_` 时调用任何外部回调。
- `RunLoop` 中同一节点的状态跃迁必须由锁内单点提交，避免竞态覆盖。
- stop/cancel 与 fail 的优先级保持不变：`failed` 优先于 `stopping`。

#### 9.1.5 测试与验收

- 回归现有 group 语义测试（含 stopped/cancelled/skipped 断言）。
- 新增测试：
  - 慢 `submit_fn_`/慢 `query_fn_` 场景下，`Snapshot()` 不出现长时间阻塞。
  - stop 与 query 并发时，节点状态不出现非法回退。
- 验收标准：
  - 回调执行期间状态接口可稳定响应。
  - 无死锁、无状态抖动。

---

### 9.2 P1-2 细化方案：前后端 SQL 分句规则统一

#### 9.2.1 改造目标

- 移除前端自维护 SQL 分句器，统一以后端 `SplitSqlText` 为唯一语义源。
- 消除“前端可执行、后端拒绝”不一致（尤其空语句、注释、引号边界）。

#### 9.2.2 设计方案

- 在 `TaskPlugin` 新增统一分析接口（建议）：
  - `POST /tasks/sql/analyze`
  - 入参：`{"sql_text":"..."}`
  - 出参：`statements`、`statement_count`、`task_kind`（`batch/stream/mixed/unknown`）、`sql_index`（错误时）
- `sql_text` 分句只走 `SplitSqlText`。
- 每条语句类型由后端调用 scheduler classify 得到，前端只消费分析结果：
  - 编辑态提示、执行模式切换、multi-sql 校验全部基于 analyze 返回。
- 兼容策略（本 Sprint）：
  - 保留 `/tasks/sql/classify` 供单 SQL 调用；
  - 前端迁移后不再使用本地 `splitSqlStatements`。

#### 9.2.3 代码改造清单（函数级）

- `src/services/task/task_plugin.h/.cpp`
  - 新增 `HandleSqlAnalyze`。
  - `EnumRoutes` 注册 `/tasks/sql/analyze`。
  - 复用现有 `SplitSqlText` 和 scheduler classify 代理逻辑。
- `src/frontend/src/api/*`（对应 API 封装文件）
  - 新增 `analyzeSqlText` 请求封装。
- `src/frontend/src/views/Tasks.vue`
  - 删除本地 `splitSqlStatements`。
  - 用 analyze 结果驱动：
    - `isMultiSql` 判定
    - `sqlTaskKind` 判定
    - sync/async 按钮可用性
    - 执行前校验与错误展示

#### 9.2.4 交互与性能约束

- 编辑器分析请求采用防抖（建议 300~500 ms）并丢弃过期响应（保留现有 `seq` 机制）。
- 执行前再做一次 analyze，确保提交时语义一致。

#### 9.2.5 测试与验收

- 后端测试：
  - `sql_text` 空字符串、空语句、未闭合注释/引号返回一致错误与 `sql_index`。
  - 多语句 `batch/stream/mixed` 判定正确。
- 前端测试：
  - 删除本地分句逻辑后，multi-sql 执行路径与按钮状态正常。
  - 异常提示与后端错误码一致。
- 验收标准：
  - 分句与类型判定前后端完全一致。
  - 不再出现分句语义漂移导致的误导性 UI。

---

### 9.3 P1-3 细化方案：Scheduler 运行态对象回收策略

#### 9.3.1 改造目标

- 防止 `stream_tasks_ / stream_task_groups_` 无限增长。
- 在保留近期诊断能力的同时，限制内存占用与列表遍历成本。

#### 9.3.2 设计方案

- 引入“终态保留策略”：
  - 时间维度：`stream_runtime_retention_s`（例如默认 600 s）
  - 数量维度：`stream_runtime_max_count`（例如默认 2000）
- 仅回收终态对象（`stopped/cancelled/failed`），运行中对象禁止回收。
- 回收触发点：
  - `/scheduler/stream/list`、`/scheduler/stream/status`、`/scheduler/stream/stop` 入口处执行 sweep。
- 回收内容：
  - `stream_tasks_` 终态项
  - `stream_task_groups_` 终态项
  - 同步清理关联索引：`stream_group_node_owners_`、`stream_group_node_sources_`、`stream_group_share_set_snapshots_` 等
  - 先释放 lease，再删除运行态对象

#### 9.3.3 数据结构与接口调整

- `SchedulerPlugin` 新增终态时间索引（示意）：
  - `runtime_terminal_ms_`（`runtime_task_id -> terminal_ts`）
- 新增 helper（示意）：
  - `MarkRuntimeTerminal(task_id, ts_ms)`
  - `SweepRuntimeRetainedObjects(now_ms)`
- `Option` 增加 retention 参数解析。

#### 9.3.4 行为约束

- 对于已回收的 runtime task，scheduler 返回 `NOT_FOUND` 是允许行为。
- TaskPlugin 在 task 已终态时，status 接口可回退到元数据结果，不依赖 scheduler 常驻对象。
- group 的诊断窗口由 retention 参数控制，而非无限期保留。

#### 9.3.5 测试与验收

- 新增测试：
  - 大量短生命周期 stream 任务后，`stream_tasks_`/`stream_task_groups_` 大小可回落。
  - retention 时间到期后对象被回收，关联索引同步清理。
  - 回收后 TaskPlugin 终态查询仍可返回稳定结果。
- 验收标准：
  - 长时间运行内存增长趋势可控。
  - list/status 时延不随历史终态任务线性劣化。

---

## 10. P1 实施顺序建议（执行级）

1. 先做 `P1-2`（语义统一，改动边界清晰、收益直接）。  
2. 再做 `P1-1`（并发模型重构，需更严格测试）。  
3. 最后做 `P1-3`（回收策略上线，结合真实运行压测调参）。

---

## 11. P2 细化实施方案（结构优化阶段）

### 11.1 P2-1 细化方案：God File 拆分与职责边界重建

#### 11.1.1 改造目标

- 将 `scheduler_plugin.cpp`、`task_plugin.cpp` 从“大而全”改造成“薄插件 + 领域服务”。
- 降低单文件认知负担和回归半径，提升代码可读性与可测性。
- 保持现有对外路由与行为不变，重构以“等价迁移”为主。

#### 11.1.2 目标架构

- `SchedulerPlugin` 只保留：
  - 插件生命周期（`Load/Start/Stop`）
  - 路由注册（`EnumRoutes`）
  - 依赖装配（service wiring）
- 领域逻辑下沉为组件：
  - `SchedulerSqlExecutor`：`HandleExecute`、`ResolveSourceBindings`、`ResolveStreamSink`、执行链路
  - `SchedulerStreamRuntimeService`：stream task/group 生命周期、lease 与清理
  - `SchedulerChannelAdminService`：stream channel CRUD/query/definitions/preview
  - `SchedulerJsonCodec`：snapshot/error 序列化

- `TaskPlugin` 只保留：
  - 生命周期、路由装配
  - 请求入口转发到 service
- 领域逻辑下沉为组件：
  - `TaskStoreSqlite`：全部 SQLite 读写（含状态迁移）
  - `TaskBatchExecutor`：batch 任务执行流程
  - `TaskStreamProxyService`：stream 任务代理与状态同步
  - `TaskJsonCodec`：list/detail/diagnostics 响应组装

#### 11.1.3 文件拆分建议（首轮）

- Scheduler：
  - 新增 `src/services/scheduler/scheduler_sql_executor.{h,cpp}`
  - 新增 `src/services/scheduler/scheduler_stream_runtime_service.{h,cpp}`
  - 新增 `src/services/scheduler/scheduler_channel_admin_service.{h,cpp}`
  - 新增 `src/services/scheduler/scheduler_json_codec.{h,cpp}`
  - `scheduler_plugin.cpp` 仅保留 facade 逻辑
- Task：
  - 新增 `src/services/task/task_store_sqlite.{h,cpp}`
  - 新增 `src/services/task/task_batch_executor.{h,cpp}`
  - 新增 `src/services/task/task_stream_proxy_service.{h,cpp}`
  - 新增 `src/services/task/task_json_codec.{h,cpp}`
  - `task_plugin.cpp` 仅保留 facade 逻辑

#### 11.1.4 迁移策略（分步可回滚）

1. 第一步：提取“纯函数/无状态工具”到 codec/util 文件（风险最低）。  
2. 第二步：提取“读写服务”但保持旧函数签名，`Plugin` 内只做委托。  
3. 第三步：收敛依赖注入与对象生命周期，移除遗留重复代码。  
4. 第四步：按模块补齐单测，减少对 e2e 的依赖。

#### 11.1.5 测试与验收

- 行为一致性：
  - 重构前后关键 API 响应字段对比一致（golden response）。
- 覆盖率提升：
  - 新组件具备独立单测，不再只能走集成路径验证。
- 复杂度指标：
  - `scheduler_plugin.cpp` 与 `task_plugin.cpp` 行数显著下降（建议目标：各下降 40% 以上）。

---

### 11.2 P2-2 细化方案：错误 JSON 与响应编码能力统一

#### 11.2.1 改造目标

- 消除 `MakeErrorJson* / JsonError*` 多处重复实现。
- 统一错误响应字段结构、错误码与附加字段（`error_stage/sql_index/details`）表达方式。
- 减少字段不一致导致的前端分支逻辑膨胀。

#### 11.2.2 设计方案

- 新增统一构造模块（建议）：
  - `src/framework/core/json_error_builder.{h,cpp}`
- 提供标准能力：
  - `BuildErrorJson(error)`
  - `BuildExecutionErrorJson(error, error_code, error_stage)`
  - `BuildExecutionErrorWithSqlIndexJson(error, error_code, error_stage, sql_index)`
  - `BuildCapabilityMismatchJson(...)`（按需扩展）
- 采用统一 key 规范：
  - 必选：`error`
  - 可选：`error_code`、`error_stage`、`sql_index`、`details`

#### 11.2.3 改造范围

- 替换以下文件中的局部 JSON error 构造函数：
  - `src/services/scheduler/scheduler_plugin.cpp`
  - `src/services/scheduler/scheduler_stream_group.cpp`
  - `src/services/task/task_plugin.cpp`
- 保留原错误码语义，不做业务码调整（本轮仅统一构造方式）。

#### 11.2.4 兼容与风险控制

- 字段名保持兼容，不做破坏性重命名。
- 文本内容允许微调，但 `error_code` 与 `error_stage` 不变。
- 若存在历史特殊字段，先在 builder 中支持，再逐步清理调用侧分叉。

#### 11.2.5 测试与验收

- 单元测试（builder）：
  - 不同输入组合的 JSON 结构与可选字段存在性。
- 回归测试（服务层）：
  - 关键错误路径 golden 校验（至少覆盖 scheduler/task/stream_group）。
- 验收标准：
  - 仓内不再新增局部 `MakeErrorJson*` 变体。
  - 前端错误处理逻辑分支减少、口径一致。

---

### 11.3 P2-3 细化方案：Scheduler 路由调用去重与接口化

#### 11.3.1 改造目标

- 消除 `TaskPlugin` 内“每次请求 `Traverse + EnumRoutes`”的重复开销与重复代码。
- 降低因函数指针缓存不当带来的生命周期风险（参考历史崩溃经验）。
- 为后续服务内调用建立类型化接口，弱化 URI 字符串耦合。

#### 11.3.2 分阶段方案

- 阶段 A（本轮建议先做）：
  - 统一 `TaskPlugin` 内 scheduler 调用入口，移除 `ExecuteOneTask` 的重复路由扫描代码。
  - `ProxySchedulerPost` 保持单点实现，所有 scheduler 调用都走该入口。

- 阶段 B（建议在 P2 后段实施）：
  - 新增内部接口 `ISchedulerControlService`（建议路径：`src/framework/interfaces/ischeduler_control_service.h`）。
  - `SchedulerPlugin` 实现该接口，并在内部直接复用现有 handler 逻辑。
  - `TaskPlugin` 优先通过 `querier_->First(IID_SCHEDULER_CONTROL_SERVICE)` 调用类型化方法：
    - `ClassifySql`
    - `ExecuteBatch`
    - `ExecuteStream`
    - `StopStream`
    - `QueryStreamStatus`
  - 路由代理作为降级路径（可在后续完全移除）。

#### 11.3.3 生命周期与安全约束

- 不长期缓存跨插件函数指针（避免插件卸载后悬挂调用）。
- 若采用接口指针，建议按调用时临时获取（`First`），避免长期持有。
- `Unload/Stop` 阶段显式清理调用入口状态，防止析构晚于依赖释放。

#### 11.3.4 改造清单（函数级）

- `src/services/task/task_plugin.cpp`
  - 删除 `ExecuteOneTask` 内局部 `EnumRoutes` 扫描逻辑，统一复用 `ProxySchedulerPost`。
  - scheduler 调用点全部收敛到一处。
- `src/services/task/task_plugin.h`
  - 新增（或调整）scheduler 调用代理封装声明。
- `src/services/scheduler/scheduler_plugin.h/.cpp`
  - （阶段 B）实现 `ISchedulerControlService` 接口方法。
- `src/framework/interfaces/*`
  - （阶段 B）新增接口与 IID 定义。

#### 11.3.5 测试与验收

- 功能回归：
  - batch execute / sql classify / stream execute/stop/status 流程全通过。
- 生命周期回归：
  - 启停、卸载、重载场景下无崩溃（重点验证调用入口清理）。
- 性能观察：
  - 高频轮询场景下 CPU 开销下降（至少消除重复 route 扫描）。

---

## 12. P2 实施顺序建议（执行级）

1. 先做 `P2-2`（抽公共 builder，低风险、收益立竿见影）。  
2. 再做 `P2-3` 阶段 A（先消除重复扫描与重复实现）。  
3. 再做 `P2-1`（组件拆分，改动面最大，建议在前两项稳定后推进）。  
4. 最后做 `P2-3` 阶段 B（接口化替换 URI 调用）。
