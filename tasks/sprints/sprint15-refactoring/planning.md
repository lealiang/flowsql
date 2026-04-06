# Sprint 15 Refactoring 规划

## Sprint 信息

- **Sprint 周期**：Sprint 15（Refactoring）
- **开始日期**：2026-04-06
- **当前状态**：已完成（P0/P1/P2 本轮计划项全部落地）
- **Sprint 目标**：在不改变对外行为的前提下，完成并发安全加固、执行一致性治理与结构化重构收敛。

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

## 验证与验收（已执行）

- [x] `cmake --build build -j4`
- [x] `./build/output/test_framework`
- [x] `./build/output/test_task`
- [x] `./build/output/test_scheduler_mutation_guard`
- [x] `./build/output/test_scheduler_e2e`
