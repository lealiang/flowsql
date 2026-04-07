# Sprint 15 Refactoring 规划

## Sprint 信息

- **Sprint 周期**：Sprint 15（Refactoring）
- **开始日期**：2026-04-06
- **当前状态**：进行中（P0/P1/P2/R1-R5 已完成，待收口与提交）
- **Sprint 目标**：在不改变对外行为的前提下，完成并发安全加固、执行一致性治理与结构化重构收敛。
- **设计文档**：`tasks/sprints/sprint15-refactoring/design.md`

---

## 已实施任务（全部完成）

### P0（必须修复）

- [x] **P0-1**：`SchedulerPlugin::channels_` 并发访问治理（统一加锁访问与生命周期安全）
- [x] **P0-2**：`TaskPlugin` SQLite 并发一致性治理（统一 DB 临界区与状态迁移原子性）

### P1（本迭代完成）

- [x] **P1-1**：`StreamTaskGroup` 回调持锁路径收敛（降低锁持有时间与死锁风险）
- [x] **P1-2**：前后端 SQL 分句语义统一（以后端规则为准）
- [x] **P1-3**：Scheduler 运行态对象回收策略落地（终态 retention 与清理机制）

### P2（结构优化）

- [x] **P2-2**：错误 JSON 构造统一到公共模块 `json_error_builder`
- [x] **P2-3（阶段 A）**：`TaskPlugin` 调用 Scheduler 的入口收敛到 `ProxySchedulerPost`
- [x] **P2-3（阶段 B）**：引入 `ISchedulerControlService`，`TaskPlugin` 统一走类型化接口调用并移除旧路由扫描回退
- [x] **P2-1（第一步）**：无状态工具拆分  
  - `scheduler_json_codec`（stream task/group 快照编码）  
  - `task_sql_utils`（task 侧 SQL 工具函数）

---

## 本迭代追加任务（R1-R5）

### R1：拆分 `SchedulerPlugin` 巨文件（高收益）

- [x] 将 `scheduler_plugin.cpp` 按职责拆分为 `routes / stream_executor / channel_admin / runtime_retention` 四类模块
- [x] 保持 `IRouterHandle` 与 `ISchedulerControlService` 对外契约不变
- [x] 补充对应单测，确保拆分后行为一致

### R2：抽离 `TaskPlugin` 的 SQLite 存储层

- [x] 新增 `task_store_sqlite.*`，承载任务表 CRUD、事件写入、诊断与 retention
- [x] `TaskPlugin` 只保留编排、调度调用与状态机驱动
- [x] 维持 `db_mu_` 规则与事务语义不回退（`db_mu_` 收敛至 store 内部）

### R3：收敛“source/sink 解析 + lease + capability 校验”为执行计划对象

- [x] 引入 `StreamExecutionPlan` 与 `LeaseToken (RAII)`，统一执行前校验
- [x] 消除散落在 `ExecuteStreamTask` 的重复校验与回滚分支
- [x] 保证 TOCTOU 防护语义不弱化

### R4：统一 Scheduler 控制调用客户端（生产与测试共用）

- [x] 新增 `scheduler_control_client.*`，统一 `Classify/Execute/Stop/Status` 调用
- [x] `TaskPlugin` 与 `test_task` 复用该客户端，去除重复 URI 分发表
- [x] 保持现有错误码与返回结构一致

### R5：错误码与响应契约类型化

- [x] 建立统一错误码常量/枚举与映射表
- [x] 统一 `scheduler/task/stream_group` 的错误构造入口与字段口径
- [x] 增加契约测试，校验关键 API 的 `error/error_code/error_stage` 稳定性

---

## 验证与验收（已执行）

- [x] `cmake --build build -j4`
- [x] `./build/output/test_framework`
- [x] `./build/output/test_task`
- [x] `./build/output/test_scheduler_mutation_guard`
- [x] `./build/output/test_scheduler_e2e`
