# Sprint 13 规划

## Sprint 信息

- **Sprint 周期**：Sprint 13
- **开始日期**：2026-04-03
- **预计工作量**：8 天
- **Sprint 目标**：将产品待办中「待规划（下个迭代）」的流式能力（Story 14.7~14.10）落为可执行计划，并形成跨进程入口、Web 管理与并发模式补齐的交付闭环。

---

## 迭代边界（冻结）

### 本迭代范围（In Scope）

1. **Story 14.7**：路径 B 接口占位（`IBlockStream*`）
2. **Story 14.8**：跨进程流通道与 TaskPlugin 统一入口
3. **Story 14.9**：Web 流式管理页面完整化
4. **Story 14.10**：Ring 并发模式补齐（`MPSC/MPMC`）
5. **测试与文档**：补齐单测、集成测试、E2E 冒烟与回归文档

### 非本迭代范围（Out of Scope）

1. Story 14.5：DPDK 网卡采集插件（路径 B）
2. Story 14.6：网络性能分析算子（路径 B）
3. 路径 B 数据面完整实现（真实 block stream 数据处理链路）
4. 多主机分布式编排（Orchestrator/Host/Executor）

### 关键约束

1. 继续执行 Sprint 12 已确认的单轨语义：`INTO` 决定 sink 通道类型，Scheduler 绑定真实 sink 并透传。
2. 保持 URI 语义清晰：stream 查询与管理走 stream 专属路由，不做 dataframe 路由兜底。
3. Stream 通道查询统一采用 `query` 语义：内部 `POST /channels/stream/query`，外部 `POST /api/channels/stream/query`。
4. TaskPlugin 与 Scheduler 路由分层：任务管理走 `/tasks/*`，执行走 `/scheduler/*`，不引入“内部路由”新概念。
5. 不引入 V1/V2 双轨兼容方案，按当前统一设计直接演进。
6. `builtin.*` 两段式数据库目标（`INTO <db_type>.<db_name>`）保持显式报错策略。
7. IRouterHandle 路由继续遵守精确匹配约束，不使用路径参数风格。

---

## Sprint 目标与成功标准

### 主要目标

1. 将 Story 14.7~14.10 从「待规划（下个迭代）」转换为可执行开发项并完成落地。
2. 打通流式任务跨进程控制链路（Web → Gateway → TaskPlugin/Scheduler）。
3. 补齐 Web 端 Stream 通道管理与 Stream 任务可视化操作。
4. 将 `ring_mode=mpsc/mpmc` 从 `ENOTSUP` 升级为可用实现并给出验证基线。

### 成功标准

- [x] `IBlockStreamChannel` / `IBlockStreamOperator` 接口占位可编译、可加载，不影响路径 A 已有能力。
- [x] 流式任务 `execute/stop/status/list` 在跨进程链路可用，错误可透传到 Web。
- [x] Web 端可完成 Stream 通道增删改、流式任务执行与状态查看。
- [x] `MPSC/MPMC` 模式通过并发正确性与 Stop/Cancel 稳定性测试。
- [x] `STATELESS` 并发路径移除 `SharedSpmcState`，改为 `StatelessSourceView` + shard 并发直读 source，并通过稳定性回归。
- [x] `IStreamChannel::Capabilities()` 完成落地，Scheduler 按能力硬校验，不做隐式降级。
- [x] `task_id` 统一由 TaskPlugin 按现有算法生成，Scheduler 仅返回 `runtime_task_id`。
- [x] `stop` 成功后任务进入 `stopped`，并与 `cancelled` 语义显式区分。
- [x] 同一 source 在 Sprint 13 采用独占租约，双任务并发消费返回 `CONFLICT`。
- [x] `execute` 与 `modify/remove` 并发场景无 TOCTOU（原子占用计数 + 两阶段切换 + 失败补偿）。
- [x] 非 stream sink 并发写能力未声明时按 `SINGLE` 处理，作为 Sprint 13 限制保留。
- [x] 自动化回归覆盖：`test_stream`、`test_scheduler_e2e`、Web 前端构建。

---

## Story 列表

### Story 14.7：路径 B 接口占位（`IBlockStream*`）

**优先级**：P1  
**工作量估算**：1 天  
**依赖**：无

**验收标准**：
- [x] 新增 `IBlockStreamChannel` / `IBlockStreamOperator` 接口头文件（仅契约占位，不含数据面实现）
- [x] 明确 `block_stream` 调度入口契约和生命周期边界
- [x] 提供最小编译与加载验证，确保占位接口不会影响路径 A 现有能力

**任务分解**：
- [x] T1：新增 `src/framework/interfaces/iblock_stream_channel.h`
- [x] T2：新增 `src/framework/interfaces/iblock_stream_operator.h`
- [x] T3：更新接口导出与构建依赖（`src/framework/CMakeLists.txt`、必要 include 路径）
- [x] T4：Scheduler 增加 `block_stream` 占位分支与明确错误码（`BAD_REQUEST` + `BLOCK_STREAM_NOT_IMPLEMENTED`）
- [x] T5：补充最小验证测试（编译 + 路由可达 + 明确报错）

---

### Story 14.8：跨进程流通道与 TaskPlugin 统一入口

**优先级**：P0  
**工作量估算**：3 天  
**依赖**：无（可与 Story 14.7 并行）

**验收标准**：
- [x] 补齐跨进程流式任务提交与管理链路（Web → Gateway → TaskPlugin → Scheduler）
- [x] 统一 Stream 任务的提交、停止、状态、列表入口，减少与批处理入口割裂
- [x] 路由完成明确分层：TaskPlugin 对外 `/tasks/stream/*`，Scheduler 内部 `/scheduler/stream/*` 与 `/scheduler/batch/execute`
- [x] `task_id` 由 TaskPlugin 统一生成（沿用 `MakeNowTaskId(seq)`）；Scheduler 不接收外部 `task_id`
- [x] `/tasks/stream/status` 显式返回任务视图状态与运行态状态映射（覆盖 `pending/running/stopped/cancelled/failed`）
- [x] `stop` 成功后终态为 `stopped`，`cancelled` 保留为强制中断语义
- [x] 补齐跨进程错误透传与诊断字段，保证失败原因在 Web 端可定位
- [x] 新增端到端回归测试，覆盖跨进程流式任务 `execute/stop/status/list` 主链路

**任务分解**：
- [x] T6：TaskPlugin 新增 `/tasks/stream/execute|stop|status|list`，统一流任务管理入口与 `task_id` 生成
- [x] T7：Scheduler 路由迁移到 `/scheduler/batch/execute`、`/scheduler/stream/*`，并返回 `runtime_task_id`
- [x] T8：WebServer 新增 `/api/tasks/stream/*` 代理，统一转发到 TaskPlugin
- [x] T9：Task 元数据扩展（`task_kind`、`runtime_task_id`）与错误诊断字段透传（`error`、`op_stats`、状态快照）
- [x] T10：TaskStatus 扩展 `stopped` 并补齐 `submitted/pending -> pending` 在内的状态映射
- [x] T11：新增跨进程 E2E（Web → TaskPlugin → Scheduler → StreamRuntime），覆盖 execute/stop/status/list、`stopped/cancelled` 语义与外部 `task_id` 拒绝回归

---

### Story 14.9：Web 流式管理页面完整化

**优先级**：P0  
**工作量估算**：2 天  
**依赖**：Story 14.8

**验收标准**：
- [x] 在现有只读基础上补齐 Stream 通道管理能力（增删改与配置编辑）
- [x] Stream 通道查询统一使用 `query` 路由，不再提供 `list` 兼容路径
- [x] `modify/remove` 对运行中引用通道返回 `CONFLICT`，`query` 返回 `in_use` 字段
- [x] `execute` 与 `modify/remove` 并发下保持原子一致性，不出现 TOCTOU
- [x] 同一 source 双任务并发执行返回 `CONFLICT`（Sprint 13 独占租约策略）
- [x] `stream_channel_store` 持久化到 `flowsql_meta.db`，并通过配置项指定数据库路径
- [x] 新增流式任务管理页面，支持 `execute/stop/status/list` 的可视化操作
- [x] 流任务页面默认并入 `Tasks.vue`，仅在交互差异过大时拆分 `StreamTasks.vue`
- [x] 展示流式任务核心指标与失败信息（`status/error/op_stats`），支持快速定位
- [x] 补齐前后端联调与 E2E 冒烟用例，覆盖主流程

**任务分解**：
- [x] T12：后端补齐 Stream 通道管理接口（`query/add/modify/remove`），并移除 `list` 语义入口
- [x] T13：StreamPlugin 增加 `db_path` 配置解析，`stream_channel_store` 落库到 `flowsql_meta.db`（含建表与 YAML 首次导入）
- [x] T14：实现 TOCTOU 防护（短临界区 + 两阶段切换 + 失败补偿），并固化锁顺序约束
- [x] T15：实现 source 独占租约（执行前申请、终态释放、占用冲突 `CONFLICT`）
- [x] T16：WebServer 代理补齐 `/api/channels/stream/query|add|modify|remove`
- [x] T17：前端 `api/index.js` 同步 Stream 通道管理与流式任务 API（统一 `query`）
- [x] T18：`Channels.vue` 增加 Stream 通道增删改 UI 与配置编辑表单（基于 `in_use` 控制可编辑性）
- [x] T19：流任务视图优先并入 `Tasks.vue`（标签页/分区）；若差异过大再拆分 `StreamTasks.vue`
- [x] T20：补齐前后端联调用例与页面冒烟回归（含 execute-vs-modify/remove 并发回归）

---

### Story 14.10：Ring 并发模式补齐（`MPSC/MPMC`）

**优先级**：P0  
**工作量估算**：2 天  
**依赖**：Story 14.1 既有实现

**验收标准**：
- [x] 将 `ring_mode=mpsc/mpmc` 从 `ENOTSUP` 升级为可用实现
- [x] `STATELESS + parallelism > 1` 改为 `StatelessSourceView` + shard 并发直读 source，不再依赖 `SharedSpmcState`
- [x] 引入 `IStreamChannel::Capabilities()` 能力模型并完成装配硬校验（不匹配时 `BAD_REQUEST`）
- [x] 并行场景增加 sink 并发 `Put` 门禁，不满足时直接失败（禁止隐式降级）
- [x] 非 stream sink 并发写能力未声明时按 `SINGLE` 处理，并明确为 Sprint 13 限制
- [x] 覆盖并发正确性测试（无丢失、无重复、可收敛）
- [x] 覆盖 Stop/Cancel 场景下的稳定性测试
- [x] 补充性能基线数据，与现有 `SPSC/SPMC` 路径对比

**任务分解**：
- [x] T21：实现 `AtomicRing` 的 MPSC 入队并发路径
- [x] T22：实现 `AtomicRing` 的 MPMC 入队/出队并发路径
- [x] T23：更新 `RingStreamChannel` 模式分发，开放 `mpsc/mpmc` 模式支持
- [x] T24：在 `IStreamChannel` 增加 `Capabilities()`，并在 `Ring/TcpMock/FanIn` 等通道实现能力声明
- [x] T25：实现 `StatelessSourceView`（共享 `PollNext`、`Cancel` once、`Close` 引用计数延迟关闭）
- [x] T26：移除 `SharedSpmcState` / `SharedSpmcInputView` 装配路径，`STATELESS` 改为 `StatelessSourceView` 装配
- [x] T27：Scheduler 增加 source/sink 双侧能力门禁与标准错误返回（`BAD_REQUEST` + `details.capabilities`），并标注非 stream sink 并发写为 Sprint 13 限制
- [x] T28：补充 `test_stream` 并发正确性与收敛测试（含 Stop/Cancel、高竞争、共享 source 生命周期与 source 独占租约冲突）
- [x] T29：补充 `test_scheduler_e2e` 回归（`poll_mode/max_consumers`、`put_mode/max_producers`、`task_id` 规则、`stopped/cancelled` 语义、execute-vs-modify/remove 竞态）
- [x] T30：补充性能基线测试与结果记录（含 `STATELESS` 改造前后对比）

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

- [x] `test_stream`：MPSC/MPMC 并发正确性、Stop/Cancel 稳定性、高竞争收敛、共享 source 生命周期回归
- [x] `test_scheduler_e2e`：跨进程流式任务 `execute/stop/status/list`，source/sink 能力不匹配 `BAD_REQUEST`，`task_id`/`runtime_task_id` 规则，`stopped/cancelled` 状态语义回归
- [x] 并发一致性：`execute` 与 `modify/remove` 竞态无 TOCTOU，失败路径无脏状态
- [x] Web/前端冒烟：Stream 通道增删改 + 流式任务可视化主流程
- [x] 线程池影响观测：`poll_timeouts`、`poll_errors`、`queue_depth_peak`、线程数量变化

---

## 实施顺序

```text
Day 1: Story 14.7 与 Story 14.8 并行启动（14.7 接口占位 + 14.8 入口改造）
Day 2-4: Story 14.8（跨进程统一入口 + 错误透传 + E2E）
Day 5-6: Story 14.9（后端管理接口 + 前端页面完整化）
Day 7-8: Story 14.10（MPSC/MPMC + Capabilities + StatelessSourceView + 并发/性能验证）
```

---

## 风险与缓解

| 风险 | 可能性 | 缓解措施 |
|------|--------|---------|
| TaskPlugin 与 Scheduler 路由职责交叉，导致链路不清晰 | 中 | 先冻结统一入口契约，再按契约改造代理路径并补 E2E |
| Stream 通道增删改涉及运行态与持久态一致性 | 中 | 采用单一权威存储（`flowsql_meta.db`），所有写操作走同一路由并加回读校验 |
| `execute` 与 `modify/remove` 存在 TOCTOU 并发竞态 | 高 | 短临界区两阶段切换、原子 in-use 计数、失败补偿回滚，并补并发回归测试 |
| 管理面加锁策略可能引发死锁或吞吐下降 | 中 | 固定锁顺序并禁止锁内慢操作，增加锁等待与冲突率观测 |
| 多插件共用 `flowsql_meta.db` 带来锁竞争 | 中 | 启用 WAL、缩短事务、将慢操作放锁外并观测写冲突重试率 |
| MPMC 并发实现引入竞态，导致偶发丢数或不收敛 | 高 | 先补压力测试与模型测试，再逐步实现，失败案例固化为回归 |
| `STATELESS` 直读 source 后 `PollNext` 竞争上升，可能导致 timeout 比例升高 | 中 | 增加能力门禁与观测指标，必要时按基线调整 `stream_workers`、`ring_size` 和 backoff |
| 共享 source 生命周期管理不当导致提前 `Close`，影响并发 shard 正常消费 | 中 | 使用 `StatelessSourceView` 做生命周期隔离，增加共享 source 回归测试 |
| 同一 source 采用独占租约会限制并行消费能力 | 低 | 作为 Sprint 13 约束先保证正确性，shared/broadcast 模型后续专题设计 |
| Web 展示字段与后端返回不一致 | 中 | 先定义统一 JSON 契约并在前后端共享字段命名 |

---

## 交付物清单

1. Sprint 13 计划文档：`tasks/sprints/sprint13/planning.md`
2. 路径 B 接口占位：`iblock_stream_channel.h`、`iblock_stream_operator.h`
3. 跨进程统一入口改造：TaskPlugin/Scheduler/Web 流式任务路由链路
4. Web 流式管理增强：Stream 通道管理 + 流式任务页面
5. Ring 并发增强：`MPSC/MPMC` 实现与测试基线
