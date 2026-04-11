# Sprint 18 规划

## Sprint 信息

- **Sprint 周期**：Sprint 18
- **开始日期**：2026-04-10
- **预计工作量**：11 天（评估区间 10-12 天）
- **Sprint 目标**：完成 Epic 17 阶段一（Story 17.1），落地“执行实例单画布 DAG 可视化”，让多 SQL 编排关系、触发语义与运行状态可直观观测。
- **设计文档**：`tasks/sprints/sprint18/design.md`
- **关联待办**：`tasks/product_backlog.md`（Epic 17 / Story 17.1）
- **状态**：✅ 已完成（2026-04-11）

---

## 迭代边界（冻结）

### 本迭代范围（In Scope）

1. Runtime 可视化后端图结构契约（节点/边/事件）
2. TaskPlugin/Web 统一查询入口（按 task_id 返回图快照）
3. 前端单画布 DAG 渲染（节点：Channel/Operator；边：触发语义）
4. 运行时快照轮询与终态收敛
5. 多 SQL 串接自动连线（同名通道节点去重）
6. 自动化测试与前端构建回归
7. 原始 SQL 独立持久化（按 task_id 关联）与任务历史“任务类型”列展示
8. Stream 有状态通道重置能力（后端路由 + Web 入口 + 前端操作）

### 非本迭代范围（Out of Scope）

1. 阶段二可视化编辑器（Story 17.2）
2. 未提交 SQL 的预览态可视化
3. 新增 SQL 语法或执行入口
4. 跨任务 DAG 合并可视化

### 关键约束

1. 可视化对象是“执行实例（Runtime Instance）”，不是 SQL 文本本身。
2. 触发语义必须显式展示：`on_start` / `on_data` / `on_finish`。
3. 图结构以服务端返回为准；前端不再机械拆 SQL 猜图。
4. 任务入口保持不变，继续复用现有任务提交与状态接口体系。
5. Runtime Graph 采用新增 URI 三层映射并冻结：`/api/tasks/runtime/graph/query`、`/tasks/runtime/graph/query`、`/scheduler/runtime/graph/query`。
6. 不提供旧 URI 兼容别名，不做隐式兜底转发。
7. 任务 SQL 查询不允许从摘要字段退化恢复；payload 缺失按错误码显式报错。

---

## Story 列表

### Story 17.1：运行时任务编排可视化（阶段一）

**优先级**：P0  
**工作量估算**：11.2 PD  
**依赖**：Sprint 17 的 mixed/group 执行与状态字段基线

**验收标准**：

- [x] SQL 工作台中所有任务均可进入“可视化”页面（不按 task kind 限制）。
- [x] 可视化页面以单画布展示完整 DAG，包含 `Channel` 与 `Operator` 两类节点。
- [x] 同名通道节点去重，能将多 SQL 自动串接为同一张图。
- [x] 边能区分触发语义：`on_start`、`on_data`、`on_finish`。
- [x] 节点/边展示运行状态与关键指标（`status/phase/processed_rows/output_rows/error`）。
- [x] 支持轮询刷新，终态自动停止轮询。
- [x] 后端返回统一图结构 JSON，前端不依赖 SQL 字符串解析做主逻辑。
- [x] 新增 URI 的 Method/路径/错误码映射已冻结并在设计与计划文档同步。
- [x] 自动化测试通过（后端单测/集成 + 前端构建）。
- [x] 任务历史 `sql_text` 显示原始 SQL，不再显示 `[group] N SQL nodes`。
- [x] 任务历史新增“任务类型”列（`batch/stream`）。
- [x] payload 缺失时接口返回结构化错误（不退化）。
- [x] 通道管理页支持 Stream 通道“重置”操作（重置运行态，不改配置）。

---

## 任务分解与时间估计

| 任务 | 状态 | 内容 | 主要文件 | 估算（PD） | 依赖 | 输出/验收点 |
|---|---|---|---|---:|---|---|
| T1 | 已完成 | 冻结 Runtime Graph URI 与 JSON 契约（Method/路径/错误码/字段） | `sprint18/design.md`、`scheduler_*`、`task_plugin.cpp` | 0.8 | - | 契约冻结，字段与 URI 语义文档化 |
| T2 | 已完成 | Scheduler 侧构建 DAG 快照（节点去重、边触发语义、依赖映射） | `scheduler_stream_group_*` | 1.6 | T1 | 可按 runtime_task_id 输出完整图结构 |
| T3 | 已完成 | TaskPlugin 转译并对外暴露任务图查询能力 | `task_plugin.cpp`、`web_server.cpp` | 1.0 | T2 | 通过 task_id 查询图快照 |
| T4 | 已完成 | 前端 Runtime 页改为图驱动渲染（非 SQL 行渲染） | `TaskRuntime.vue` | 1.8 | T1/T3 | 单画布 DAG 展示完成 |
| T5 | 已完成 | 边触发语义样式与节点状态联动（on_start/on_data/on_finish） | `TaskRuntime.vue`、样式文件 | 1.0 | T4 | 触发语义可视化可区分 |
| T6 | 已完成 | 增量刷新与终态收敛（cursor 轮询 + `snapshot_time_ms` 展示） | `TaskRuntime.vue`、`api/index.js` | 0.8 | T4 | 轮询稳定，无重复抖动 |
| T7 | 已完成 | SQL 工作台可视化入口与跳转链路收口 | `Tasks.vue`、`router/index.js` | 0.5 | T4 | 所有任务可进入可视化页 |
| T8 | 已完成 | 后端测试补齐（图结构正确性、触发语义、多 SQL 串接） | `test_scheduler_e2e.cpp`、`test_task.cpp` | 1.0 | T2/T3 | 关键场景自动化覆盖 |
| T9 | 已完成 | 前端回归与文档同步（README/迭代文档） | `README.md`、`planning.md` | 0.5 | T5/T6/T7 | 能力描述与实现一致 |
| T10 | 已完成 | 新增 `task_sql_payloads` 表与读写接口（按 task_id 关联） | `task_store_sqlite.*`、`task_plugin.cpp` | 0.8 | T1 | 原始 SQL 与 sqls_json 独立持久化 |
| T11 | 已完成 | 任务查询链路改造为 payload 强依赖（移除退化） | `task_plugin.cpp`、`task_store_sqlite.*` | 0.6 | T10 | 列表/详情/runtime graph 统一读 payload |
| T12 | 已完成 | 前端任务历史新增“任务类型”列并校准展示 | `Tasks.vue` | 0.2 | T3 | 任务类型列稳定展示 |
| T13 | 已完成 | SQL 持久化专项测试（group 原文展示、payload 异常） | `test_task.cpp` | 0.6 | T10/T11 | 无退化、错误码可验证 |
| T14 | 已完成 | Stream 通道重置能力（`/channels/stream/reset` + `/api/channels/stream/reset` + Web 按钮） | `scheduler_routes.cpp`、`web_server.cpp`、`Channels.vue`、`test_scheduler_e2e.cpp` | 0.5 | T1 | 可显式重置有状态通道，`in_use/source_in_use` 冲突受控 |

**合计**：11.7 PD

---

## 实施顺序（建议）

```text
Day 1: T1（图结构契约冻结）
Day 2-3: T2（Scheduler 图快照输出）
Day 4: T3（TaskPlugin/Web 查询链路）
Day 5-6: T4 + T5（前端单画布 DAG + 触发语义）
Day 7: T6 + T7（轮询收敛 + 工作台入口）
Day 8: T8（后端测试补齐）
Day 9: T10（SQL payload 持久化）
Day 10: T11 + T12（查询链路改造 + 任务类型列）
Day 11: T13 + T9（专项测试 + 文档收口）
```

---

## 风险与缓解

| 风险 | 可能性 | 影响 | 缓解措施 |
|---|---|---|---|
| 后端图结构字段不稳定，前端渲染频繁返工 | 中 | 中 | T1 先冻结契约，再进入前端开发 |
| 多 SQL 场景节点去重或连线错误 | 中 | 高 | 增加固定样例回归（链式、并行、混合） |
| 轮询导致页面抖动或状态倒退 | 中 | 中 | 使用 `cursor` 增量机制 + 终态停止轮询（`snapshot_time_ms` 仅用于展示） |
| 触发语义展示与真实调度语义不一致 | 低 | 高 | 语义由后端显式输出，前端只做渲染映射 |

---

## 交付物清单

1. Sprint 18 计划文档：`tasks/sprints/sprint18/planning.md`
2. Runtime Graph 查询接口与契约实现
3. 前端单画布 DAG 可视化页面（执行实例）
4. 自动化测试与回归结果
5. 文档同步（产品待办与能力说明）

---

## 后续衔接（Sprint 18 外）

- Story 17.2：可视化任务编辑与提交（阶段二）
- 草稿态编排编辑、后端拓扑校验、保存与发布流程
