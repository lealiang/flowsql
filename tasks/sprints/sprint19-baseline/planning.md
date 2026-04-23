# Sprint 19 规划

## Sprint 信息

- **Sprint 周期**：Sprint 19
- **开始日期**：2026-04-21
- **预计工作量**：待评估
- **对应产品待办项**：Epic 18「通用基线检测插件」
- **Sprint 目标**：交付 `baseline` 通用插件的首版工程骨架与实现计划，完成 `T1 / T2` 热路径、异步正式重建闭环、正式训练与 `candidate builder`、`Baseline Source / EventCalendarSpec` 契约、`detector core` 抽取、`relation` 关系分布任务框架及其正式重建闭环，以及配套测试与诊断能力

---

## Sprint 目标

### 主要目标

1. 建立 `baseline` 插件的公共接口、插件注册骨架和 task 生命周期模型。
2. 落地 `T1 / T2` 的同步热路径与 `rebuild queue + worker + formal training + candidate builder` 的异步慢路径。
3. 收口 `Baseline Source` 与 `EventCalendarSpec` 的工程契约，避免算法设计与接口实现脱节。
4. 抽出 `T1 / T2` 的内部 `detector core`，让 `value / ratio task` 收口为编排层，并为 `relation task` 复用做好代码边界准备。
5. 落地 `T3` 的“一个 `relation task` = 一个关系分布规格”框架，补齐 basis / 摘要提取 / routed 接线 / 正式重建闭环。
6. 建立可验证、可调试、可维护的测试与诊断基础设施。

### 成功标准

- [x] `baseline` 插件可被 `PluginLoader` 正常加载，并能通过 `IQuerier` 查询到 `IBaselineService`
- [x] `IBaselineService / IBaselineTask / IBaselineValueTask / IBaselineRatioTask / IBaselineRelationTask` 接口稳定落地
- [x] `CreateValueTask / CreateRatioTask / CreateRelationTask` 能创建并管理 task 生命周期
- [x] `T1 / T2` 热路径接口可同步返回 `DetectorResult`
- [x] `T1 / T2` 在线评分主干已收口为内部 `ValueDetectorCore / RatioDetectorCore`，`value / ratio task` 不再自带一整套平行状态机
- [x] 热路径不会直接调用 `history_reader`
- [x] `RebuildRequest` 能入内部队列，由后台 worker 异步执行正式重建
- [x] `FormalModelTrainer / CandidateBuilder` 能把历史回放结果训练为可服务的 `candidate model`
- [x] `Baseline Source` 能以低频静态配置方式注入，并在本级基线不可服务时完成来源选择且不阻断本级训练
- [x] `EventCalendarSpec` 能以任务级静态规格注入，并保证训练 / 预测阶段使用一致的 `calendar_id + calendar_version`
- [x] `shadow baseline` 能在旧基线失配后接管在线评分，并在正式切换后退出
- [x] `T3` 可接收 `RelationObservationBlock`，完成摘要提取并路由到 `T1 / T2`
- [x] `relation task` 的正式重建可建立 / 刷新 `ServiceBasis / EvalBasis`，并在兼容 basis 上完成 `candidate vs incumbent` 比较
- [x] 所有涉及基线算法的代码都带清晰注释，重点解释算法语义、状态机和重建逻辑
- [x] 测试覆盖接口加载、task 生命周期、`T1 / T2`、慢路径重建、`shadow baseline`、`T3`、并发边界

---

## Story 开工前依赖闭合检查

每个 Story 开工前，必须先完成一次 `design.md -> plugin-design.md -> planning.md -> current code/tests` 的四向核对，确认不是“纸面可做、代码不可做”。

强制检查项：

1. **输入契约闭合**：当前 Story 要消费的配置、输入结构和元数据，已在前置 Story 中定义并能被当前代码持有
2. **状态生产闭合**：当前 Story 要读取的运行时状态，已由某个已完成 Story 真实产出，而不是只存在于未来设计里
3. **决策语义闭合**：`design.md` 与 `plugin-design.md` 对同一术语、同一选择规则的定义一致，不存在“业务设计一种语义、代码设计另一种语义”
4. **验证钩子闭合**：当前 Story 的每条验收标准，都能落到现有或本 Story 内可新增的测试钩子；禁止把核心验收建立在未来 Story 才会出现的状态上

若任一检查项不闭合，必须先调整 Story 边界：要么前移最小使能能力，要么把当前 Story 的契约收口到“当前可服务、可验证”的版本，再进入编码。

---

## 设计文档

- [统一基线算法设计](design.md)
- [统一基线插件代码设计](plugin-design.md)
- [统一基线方案与 Facebook Prophet 对比评估](prophet-comparison.md)

---

## 范围与边界

### 本 Sprint 纳入范围

- `baseline` 插件公共接口
- task 生命周期与配置解析
- `T1 / T2` 热路径评分骨架
- `T1 / T2` 在线评分主干抽取为内部 `detector core`
- `history_reader` 装配接口
- 重建队列、后台 worker、正式训练、`candidate builder`、candidate 验证、正式切换
- `Baseline Source` 低频配置与来源选择
- `EventCalendarSpec` 任务级注入与 `calendar_id / calendar_version` 契约
- `shadow baseline`
- `relation` 关系分布 task、basis / summary 提取、routed detector core 接线与正式重建
- 插件级与 task 级诊断
- 单元测试、集成测试、并发测试、基础性能冒烟

### 本 Sprint 不纳入范围

- Web / HTTP 管理 API
- 调度服务对 `baseline` 的外部编排
- 分布式部署与跨进程模型共享
- 新增大规模外部数学库所需的完整工程集成
- `T4` 或其他超出 `design.md` 当前正式范围的新特征类型

---

## 计划中的文件结构

### 新增公共接口

- 创建：`src/framework/interfaces/ibaseline_types.h`
- 创建：`src/framework/interfaces/ibaseline_service.h`

### 新增插件目录

- 创建：`src/plugins/baseline/CMakeLists.txt`
- 创建：`src/plugins/baseline/baseline_plugin.h`
- 创建：`src/plugins/baseline/baseline_plugin.cpp`
- 创建：`src/plugins/baseline/ibaseline_internal.h`
- 创建：`src/plugins/baseline/config_parser.h`
- 创建：`src/plugins/baseline/config_parser.cpp`

### task / model / rebuild 分层

- 创建：`src/plugins/baseline/task/baseline_task_base.h`
- 创建：`src/plugins/baseline/task/baseline_task_base.cpp`
- 创建：`src/plugins/baseline/task/task_registry.h`
- 创建：`src/plugins/baseline/task/task_registry.cpp`
- 创建：`src/plugins/baseline/task/value_task.h`
- 创建：`src/plugins/baseline/task/value_task.cpp`
- 创建：`src/plugins/baseline/task/ratio_task.h`
- 创建：`src/plugins/baseline/task/ratio_task.cpp`
- 创建：`src/plugins/baseline/task/relation_task.h`
- 创建：`src/plugins/baseline/task/relation_task.cpp`
- 创建：`src/plugins/baseline/model/task_spec.h`
- 创建：`src/plugins/baseline/model/series_override.h`
- 创建：`src/plugins/baseline/model/event_calendar_spec.h`
- 创建：`src/plugins/baseline/model/series_state.h`
- 创建：`src/plugins/baseline/model/series_store.h`
- 创建：`src/plugins/baseline/model/series_store.cpp`
- 创建：`src/plugins/baseline/model/formal_model_state.h`
- 创建：`src/plugins/baseline/model/formal_model.h`
- 创建：`src/plugins/baseline/model/formal_predictor.h`
- 创建：`src/plugins/baseline/model/shadow_state.h`
- 创建：`src/plugins/baseline/model/drift_state.h`
- 创建：`src/plugins/baseline/rebuild/rebuild_request.h`
- 创建：`src/plugins/baseline/rebuild/rebuild_queue.h`
- 创建：`src/plugins/baseline/rebuild/rebuild_queue.cpp`
- 创建：`src/plugins/baseline/rebuild/rebuild_worker.h`
- 创建：`src/plugins/baseline/rebuild/rebuild_worker.cpp`
- 创建：`src/plugins/baseline/rebuild/replay_runner.h`
- 创建：`src/plugins/baseline/rebuild/replay_runner.cpp`
- 创建：`src/plugins/baseline/rebuild/formal_model_trainer.h`
- 创建：`src/plugins/baseline/rebuild/formal_model_trainer.cpp`
- 创建：`src/plugins/baseline/rebuild/candidate_builder.h`
- 创建：`src/plugins/baseline/rebuild/candidate_builder.cpp`
- 创建：`src/plugins/baseline/rebuild/candidate_validator.h`
- 创建：`src/plugins/baseline/rebuild/candidate_validator.cpp`
- 创建：`src/plugins/baseline/solver/solver_backend.h`
- 创建：`src/plugins/baseline/solver/solver_backend.cpp`

### 检测内核与关系分布实现分区

- 创建：`src/plugins/baseline/detector/`
- 创建：`src/plugins/baseline/detector/detector_common.h`
- 创建：`src/plugins/baseline/detector/value_detector_core.h`
- 创建：`src/plugins/baseline/detector/value_detector_core.cpp`
- 创建：`src/plugins/baseline/detector/ratio_detector_core.h`
- 创建：`src/plugins/baseline/detector/ratio_detector_core.cpp`
- 创建：`src/plugins/baseline/relation/`
- 创建：`src/plugins/baseline/relation/relation_basis.h`
- 创建：`src/plugins/baseline/relation/relation_basis.cpp`
- 创建：`src/plugins/baseline/relation/relation_summary_extractor.h`
- 创建：`src/plugins/baseline/relation/relation_summary_extractor.cpp`
- 创建：`src/plugins/baseline/relation/relation_router.h`
- 创建：`src/plugins/baseline/relation/relation_router.cpp`

### 测试

- 创建：`src/tests/test_baseline/CMakeLists.txt`
- 创建：`src/tests/test_baseline/test_baseline_plugin.cpp`
- 创建：`src/tests/test_baseline/test_baseline_value_task.cpp`
- 创建：`src/tests/test_baseline/test_baseline_ratio_task.cpp`
- 创建：`src/tests/test_baseline/test_baseline_rebuild.cpp`
- 创建：`src/tests/test_baseline/test_baseline_relation_task.cpp`
- 创建：`src/tests/test_baseline/test_baseline_concurrency.cpp`

### 现有文件修改

- 修改：`src/CMakeLists.txt`

说明：

- 公共接口只放 `framework/interfaces`
- 插件实现细节全部留在 `src/plugins/baseline`
- `task/value_task`、`task/ratio_task` 在 `detector core` 抽取后只保留生命周期、重建编排和 JSON 诊断职责
- 测试单独建 `test_baseline`，避免污染现有测试目录

---

## Story 列表

### Story 18.1：公共接口与插件骨架

**优先级**：P0  
**工作量估算**：2 天  
**依赖**：无

**验收标准**：

- [x] 定义 `IID_BASELINE_SERVICE`
- [x] 落地 `IBaselineService`、`IBaselineTask`、`IBaselineValueTask`、`IBaselineRatioTask`、`IBaselineRelationTask`
- [x] `baseline` 插件可被加载
- [x] 通过 `IQuerier->First(IID_BASELINE_SERVICE)` 可获取服务接口

**文件**：

- 创建：`src/framework/interfaces/ibaseline_types.h`
- 创建：`src/framework/interfaces/ibaseline_service.h`
- 创建：`src/plugins/baseline/CMakeLists.txt`
- 创建：`src/plugins/baseline/baseline_plugin.h`
- 创建：`src/plugins/baseline/baseline_plugin.cpp`
- 创建：`src/plugins/baseline/solver/solver_backend.h`
- 创建：`src/plugins/baseline/solver/solver_backend.cpp`
- 修改：`src/CMakeLists.txt`

**任务分解**：

- [x] T18.1.1：定义 `BaselineTaskKind / BaselineDirection / BaselineSeverity / BaselineProvider / BaselineReasonCode / BaselineRebuildReason`
- [x] T18.1.2：定义 `DetectorResult / ValueObservation / RatioObservation / RelationObservationBlock / HistoryFetchRequest`
- [x] T18.1.3：定义 `IBaselineService` 与各类 task 接口
- [x] T18.1.4：实现 `BaselinePlugin` 最小骨架并注册 `IID_BASELINE_SERVICE`
- [x] T18.1.5：接入 CMake，确保插件可编译、可加载，并预留 `Eigen 3` 依赖接入点
- [x] T18.1.6：建立 `solver backend` 内部封装边界，确保后续求解器实现不泄漏数学库类型到公共接口
- [x] T18.1.7：编写插件加载与接口查询测试

---

### Story 18.2：Task 生命周期与最小配置解析

**优先级**：P0  
**工作量估算**：2 天  
**依赖**：Story 18.1

**验收标准**：

- [x] `CreateValueTask / CreateRatioTask / CreateRelationTask` 可创建 task
- [x] task 可返回 `Id / Name / Kind / ConfigJson`
- [x] `Close()` 后 task 立即失效
- [x] 配置 JSON 能校验 `T1 / T2 / T3` 所需最小规格
- [x] `Baseline Source / EventCalendarSpec` 的低频静态配置解析延后到 Story 18.10 / 18.11

**文件**：

- 创建：`src/plugins/baseline/config_parser.h`
- 创建：`src/plugins/baseline/config_parser.cpp`
- 创建：`src/plugins/baseline/model/task_spec.h`
- 创建：`src/plugins/baseline/task/baseline_task_base.h`
- 创建：`src/plugins/baseline/task/baseline_task_base.cpp`
- 创建：`src/plugins/baseline/task/task_registry.h`
- 创建：`src/plugins/baseline/task/task_registry.cpp`
- 修改：`src/plugins/baseline/baseline_plugin.h`
- 修改：`src/plugins/baseline/baseline_plugin.cpp`

**任务分解**：

- [x] T18.2.1：定义内部 `TaskSpec` 与 `T3` 关系分布规格结构
- [x] T18.2.2：实现 `config_json -> TaskSpec` 的最小解析与校验，不覆盖 `BaselineSourceConfig / EventCalendarSpec`
- [x] T18.2.3：实现 task 注册表与 `task_id` 分配
- [x] T18.2.4：实现 `IBaselineTask` 基类元信息与 `Close()`
- [x] T18.2.5：实现 `ListTasks()`
- [x] T18.2.6：编写 task 生命周期与错误配置测试

---

### Story 18.3：`T1 / T2` 热路径公共状态层

**优先级**：P0  
**工作量估算**：2 天  
**依赖**：Story 18.2

**验收标准**：

- [x] 可按 `(task, key)` 维护独立状态
- [x] 同一 `key` 的 `bucket_id` 顺序校验生效
- [x] gap、persistence、基础 flags 的共用状态闭环
- [x] 热路径公共状态层不依赖具体 `T1 / T2 / T3` 算法文件

**文件**：

- 创建：`src/plugins/baseline/model/series_state.h`
- 创建：`src/plugins/baseline/model/series_store.h`
- 创建：`src/plugins/baseline/model/series_store.cpp`
- 创建：`src/plugins/baseline/model/drift_state.h`
- 创建：`src/plugins/baseline/common/result_builder.h`

**任务分解**：

- [x] T18.3.1：定义 per-key `SeriesState`
- [x] T18.3.2：实现 `SeriesStore` 的查找、创建与并发保护
- [x] T18.3.3：实现 `bucket_id` 非递减校验
- [x] T18.3.4：实现 `DetectorResult.flags` 的基础填充
- [x] T18.3.5：编写共用状态层单元测试

---

### Story 18.4：`T1` 热路径首版

**优先级**：P0  
**工作量估算**：2 天  
**依赖**：Story 18.3

**验收标准**：

- [x] `IBaselineValueTask::SubmitObservation()` 可用
- [x] `T1a / T1b` 均通过 `ValueObservation` 接入
- [x] `sample_count` 在 `T1b` 场景下参与门控
- [x] 评分逻辑、状态迁移和关键变量有清晰注释

**文件**：

- 创建：`src/plugins/baseline/task/value_task.h`
- 创建：`src/plugins/baseline/task/value_task.cpp`
- 修改：`src/tests/test_baseline/test_baseline_plugin.cpp`

**任务分解**：

- [x] T18.4.1：实现 `IBaselineValueTask` 基本行为
- [x] T18.4.2：接线 `T1a` 与 `T1b` 的输入差异
- [x] T18.4.3：接入共用 `SeriesStore`
- [x] T18.4.4：填充首版 `DetectorResult`
- [x] T18.4.5：编写 `T1a / T1b` 热路径测试

---

### Story 18.5：`T2` 热路径首版

**优先级**：P0  
**工作量估算**：2 天  
**依赖**：Story 18.3

**验收标准**：

- [x] `IBaselineRatioTask::SubmitObservation()` 可用
- [x] `numerator / denominator` 输入链路稳定
- [x] 分母门控、基础结果结构和状态更新可用
- [x] 评分逻辑与关键阈值映射有清晰注释

**文件**：

- 创建：`src/plugins/baseline/task/ratio_task.h`
- 创建：`src/plugins/baseline/task/ratio_task.cpp`
- 修改：`src/tests/test_baseline/test_baseline_plugin.cpp`

**任务分解**：

- [x] T18.5.1：实现 `IBaselineRatioTask` 基本行为
- [x] T18.5.2：接线 `numerator / denominator`
- [x] T18.5.3：接入共用 `SeriesStore`
- [x] T18.5.4：填充首版 `DetectorResult`
- [x] T18.5.5：编写 `T2` 热路径测试

---

### Story 18.6：异步正式重建基础设施

**优先级**：P0  
**工作量估算**：3 天  
**依赖**：Story 18.4、Story 18.5

**验收标准**：

- [x] 内部维护 `rebuild queue + rebuild worker`
- [x] `SetHistoryReader()` 可装配三类 reader
- [x] `RequestRebuild()` 只入队，不阻塞热路径
- [x] `history_reader` 失败不会影响同步评分接口

**文件**：

- 创建：`src/plugins/baseline/rebuild/rebuild_request.h`
- 创建：`src/plugins/baseline/rebuild/rebuild_queue.h`
- 创建：`src/plugins/baseline/rebuild/rebuild_queue.cpp`
- 创建：`src/plugins/baseline/rebuild/rebuild_worker.h`
- 创建：`src/plugins/baseline/rebuild/rebuild_worker.cpp`

**任务分解**：

- [x] T18.6.1：定义 `RebuildRequest`
- [x] T18.6.2：实现线程安全队列
- [x] T18.6.3：实现后台 worker 生命周期
- [x] T18.6.4：接线三类 `HistoryReader`
- [x] T18.6.5：实现 `RequestRebuild()` 入队逻辑
- [x] T18.6.6：编写重建基础设施测试

---

### Story 18.7：正式模型状态与 replay / candidate skeleton

**优先级**：P0  
**工作量估算**：3 天  
**依赖**：Story 18.6

**验收标准**：

- [x] `T1 / T2` 的每个 `(task, key)` 都有独立的正式模型状态骨架
- [x] 异步重建不再只“调用 reader 后丢弃结果”，而是能把历史回放到 typed replay skeleton
- [x] task / series 快照能暴露 `formal_ready / formal_model_version / candidate_generation / last_replay_window`
- [x] 后续 `shadow baseline` 与 `candidate validator` 可直接复用该 skeleton，而不必再发明临时占位对象

**文件**：

- 创建：`src/plugins/baseline/model/formal_model_state.h`
- 创建：`src/plugins/baseline/rebuild/replay_runner.h`
- 创建：`src/plugins/baseline/rebuild/replay_runner.cpp`
- 修改：`src/plugins/baseline/task/value_task.h`
- 修改：`src/plugins/baseline/task/value_task.cpp`
- 修改：`src/plugins/baseline/task/ratio_task.h`
- 修改：`src/plugins/baseline/task/ratio_task.cpp`
- 修改：`src/tests/test_baseline/test_baseline_plugin.cpp`

**任务分解**：

- [x] T18.7.1：定义 per-series `FormalModelState`
- [x] T18.7.2：实现 `Value / Ratio` typed replay skeleton
- [x] T18.7.3：将异步重建结果写回序列级正式模型状态
- [x] T18.7.4：扩展 task / series snapshot，暴露正式模型与 candidate 元数据
- [x] T18.7.5：编写 replay / candidate skeleton 测试

---

### Story 18.8：正式模型 apply 契约与 predictor skeleton

**优先级**：P0  
**工作量估算**：3 天  
**依赖**：Story 18.7

**验收标准**：

- [x] `T1 / T2` 的正式模型都有统一的内部 apply / predict 契约
- [x] 异步重建不再只保留 replay 元数据，而是能产出可被热路径读取的 `candidate model`
- [x] 每个 `(task, key)` 都能区分 `candidate model` 与当前服务中的 `formal model`
- [x] 后续 `shadow baseline` 可直接冻结 `ref_model_id + μ_ref,t`，而不必在桥接阶段临时发明预测对象

**文件**：

- 创建：`src/plugins/baseline/model/formal_model.h`
- 创建：`src/plugins/baseline/model/formal_predictor.h`
- 创建：`src/plugins/baseline/model/formal_predictor.cpp`
- 修改：`src/plugins/baseline/model/formal_model_state.h`
- 修改：`src/plugins/baseline/task/value_task.cpp`
- 修改：`src/plugins/baseline/task/ratio_task.cpp`
- 修改：`src/tests/test_baseline/test_baseline_plugin.cpp`

**任务分解**：

- [x] T18.8.1：定义 `candidate model / formal model` 的内部载体与版本语义
- [x] T18.8.2：定义统一 `predict(formal_model, bucket_id)` 契约
- [x] T18.8.3：让异步重建把 replay skeleton 物化为最小可服务 `candidate model`
- [x] T18.8.4：扩展快照，暴露当前服务模型与候选模型元数据
- [x] T18.8.5：编写 formal apply / predictor skeleton 测试

---

### Story 18.9：正式训练与 `candidate builder`

**优先级**：P0  
**工作量估算**：3 天  
**依赖**：Story 18.8

**验收标准**：

- [x] `rebuild worker` 不再只回放历史，而是能把 replay 结果训练为结构化 `candidate model`
- [x] `FormalModelTrainer` 能复用统一求解器接口完成 `T1 / T2` 正式训练
- [x] `CandidateBuilder` 能输出训练窗口、holdout 尾段、模型版本等元数据
- [x] 训练失败会形成明确失败原因并留在慢路径，不影响热路径同步评分

**文件**：

- 创建：`src/plugins/baseline/rebuild/formal_model_trainer.h`
- 创建：`src/plugins/baseline/rebuild/formal_model_trainer.cpp`
- 创建：`src/plugins/baseline/rebuild/candidate_builder.h`
- 创建：`src/plugins/baseline/rebuild/candidate_builder.cpp`
- 修改：`src/plugins/baseline/model/formal_model.h`
- 修改：`src/plugins/baseline/model/formal_model_state.h`
- 修改：`src/plugins/baseline/model/formal_predictor.cpp`
- 修改：`src/plugins/baseline/solver/solver_backend.h`
- 修改：`src/plugins/baseline/solver/solver_backend.cpp`
- 修改：`src/plugins/baseline/task/value_task.cpp`
- 修改：`src/plugins/baseline/task/ratio_task.cpp`
- 修改：`src/plugins/baseline/CMakeLists.txt`
- 修改：`src/tests/test_baseline/test_baseline_plugin.cpp`

**任务分解**：

- [x] T18.9.1：定义正式训练输入、输出与失败码契约
- [x] T18.9.2：实现 `FormalModelTrainer`，把 replay 样本训练为最小可服务正式模型
- [x] T18.9.3：实现 `CandidateBuilder`，串起 replay、训练、holdout 切分与 metadata 产出
- [x] T18.9.4：将 `candidate model` 写回 `FormalModelState`
- [x] T18.9.5：编写正式训练与 `candidate builder` 测试

---

### Story 18.10：`Baseline Source` 配置与来源选择

**优先级**：P0  
**工作量估算**：2 天  
**依赖**：Story 18.2、Story 18.8、Story 18.9

**验收标准**：

- [x] task 配置可接收可选 `series_overrides / BaselineSourceConfig`
- [x] 可按低频静态配置保存某个 `(task, key)` 的 `BaselineSourceConfig`
- [x] 当 `self` 不可服务且存在可服务的配置来源时，热路径可选择 `self | configured_source | none`
- [x] 借用来源只影响当前评分解释，不中断本级序列的累计、训练与正式重建
- [x] 快照与结果证据能暴露当前来源类型与来源 key

补充约束：

- `Story 18.10` 中“可服务”沿用 `design.md` 第 `2.3` 节的 `Serviceable(source)` 语义
- `Story 18.10` 中“可服务”按当前工程闭环定义为：`formal_ready == true`，或 `candidate_state == trained` 且 `candidate_model` 可对当前 bucket 产出预测
- 该放宽只用于 `18.10` 的来源选择闭环；`18.12` 完成正式切换后，再把优先级明确收紧为“正式模型优先于 candidate 占位模型”

**文件**：

- 创建：`src/plugins/baseline/model/series_override.h`
- 修改：`src/plugins/baseline/model/task_spec.h`
- 修改：`src/plugins/baseline/config_parser.cpp`
- 修改：`src/plugins/baseline/task/baseline_task_base.h`
- 修改：`src/plugins/baseline/task/value_task.cpp`
- 修改：`src/plugins/baseline/task/ratio_task.cpp`
- 修改：`src/tests/test_baseline/test_baseline_plugin.cpp`

**任务分解**：

- [x] T18.10.1：定义 `SeriesOverride / BaselineSourceConfig` 的内部结构与校验规则
- [x] T18.10.2：实现 `config_json -> TaskSpec` 的 `series_overrides` 解析与存储
- [x] T18.10.3：实现来源选择器，按“当前可服务模型”统一输出 `self | configured_source | none`
- [x] T18.10.4：在 `T1 / T2` 热路径接线来源借用，但保持本级训练持续进行
- [x] T18.10.5：扩展快照与结果证据，暴露当前来源信息，并编写来源选择与冷启动借用测试

---

### Story 18.10A：`T1 / T2` 主评分链与 predictor 接线

**优先级**：P0  
**工作量估算**：2 天  
**依赖**：Story 18.8、Story 18.9、Story 18.10

**验收标准**：

- [x] `T1 / T2` 热路径不再固定返回 `0` 分；当存在可服务模型时，能基于 predictor 输出产生最小可解释的点异常分
- [x] `T1` 可基于当前观测变换值与服务模型预测值，产出首版 `raw_score / normalized_score / confidence / provider`
- [x] `T2` 可基于当前比例观测与服务模型预测值，产出首版 `raw_score / normalized_score / confidence / provider`
- [x] 当 `self` 不可服务但 `configured_source` 可服务时，主评分链能复用 `Baseline Source` 的选择结果，而不是再次走一套独立来源判断
- [x] 当不存在可服务模型时，仍保持当前仅观察语义，不伪造异常解释
- [x] 本 Story 不引入漂移证据累积器、`shadow baseline`、candidate 验证或正式切换状态机

**文件**：

- 修改：`src/plugins/baseline/task/value_task.cpp`
- 修改：`src/plugins/baseline/task/ratio_task.cpp`
- 修改：`src/plugins/baseline/common/result_builder.h`
- 修改：`src/tests/test_baseline/test_baseline_plugin.cpp`

**任务分解**：

- [x] T18.10A.1：定义 `T1 / T2` 主评分链的最小输入与输出语义，明确“当前观测 + predictor 输出 + 来源选择结果”三者的接线边界
- [x] T18.10A.2：在 `T1` 热路径接入服务模型预测值，完成首版点异常分、置信度和 `provider` 输出
- [x] T18.10A.3：在 `T2` 热路径接入服务模型预测值，完成首版点异常分、置信度和 `provider` 输出
- [x] T18.10A.4：统一“无可服务模型时仅观察”的退化路径，避免 `self / configured_source / none` 三套分支各自漂移
- [x] T18.10A.5：编写 `self`、`configured_source`、`none` 三类主评分链测试，确认不再固定返回 `0` 分

---

### Story 18.11：`EventCalendarSpec` 与日历版本契约

**优先级**：P0  
**工作量估算**：2 天  
**依赖**：Story 18.2、Story 18.8、Story 18.9、Story 18.10A

**验收标准**：

- [x] task 配置可接收可选 `EventCalendarSpec`
- [x] 正式模型 metadata 能记录 `calendar_id / calendar_version`
- [x] 训练与预测阶段若日历缺失或版本不一致，事件块会被禁用而不是导致热路径失败
- [x] task / series 快照可暴露日历启用状态与版本信息

**文件**：

- 创建：`src/plugins/baseline/model/event_calendar_spec.h`
- 修改：`src/plugins/baseline/model/task_spec.h`
- 修改：`src/plugins/baseline/config_parser.cpp`
- 修改：`src/plugins/baseline/model/formal_model.h`
- 修改：`src/plugins/baseline/model/formal_predictor.h`
- 修改：`src/plugins/baseline/model/formal_predictor.cpp`
- 修改：`src/plugins/baseline/rebuild/candidate_builder.h`
- 修改：`src/plugins/baseline/rebuild/candidate_builder.cpp`
- 修改：`src/plugins/baseline/rebuild/formal_model_trainer.h`
- 修改：`src/plugins/baseline/rebuild/formal_model_trainer.cpp`
- 修改：`src/plugins/baseline/task/value_task.cpp`
- 修改：`src/plugins/baseline/task/ratio_task.cpp`
- 修改：`src/tests/test_baseline/test_baseline_plugin.cpp`

**任务分解**：

- [x] T18.11.1：定义 `EventCalendarSpec` 的内部结构与校验规则
- [x] T18.11.2：实现 `config_json -> TaskSpec` 的 `EventCalendarSpec` 解析与存储
- [x] T18.11.3：在正式训练链路记录 `calendar_id / calendar_version`
- [x] T18.11.4：在热路径 predictor 接线日历版本检查与自动禁用逻辑，并扩展快照与诊断
- [x] T18.11.5：编写日历缺失、版本不一致与自动降级测试

---

### Story 18.12：漂移证据、`shadow baseline` 与正式切换

**优先级**：P0  
**工作量估算**：3 天  
**依赖**：Story 18.9、Story 18.10、Story 18.10A、Story 18.11

**验收标准**：

- [x] 热路径发现 `shift_confirmed` 后可激活 `shadow baseline`
- [x] `shadow baseline` 激活时同步返回桥接结果
- [x] candidate 验证与正式切换闭环
- [x] `shadow` 的主要退出条件是“正式基线重建完成并切换成功”

**文件**：

- 创建：`src/plugins/baseline/model/shadow_state.h`
- 创建：`src/plugins/baseline/rebuild/candidate_validator.h`
- 创建：`src/plugins/baseline/rebuild/candidate_validator.cpp`
- 修改：`src/plugins/baseline/model/drift_state.h`
- 修改：`src/plugins/baseline/model/formal_model_state.h`
- 修改：`src/plugins/baseline/rebuild/candidate_builder.cpp`
- 修改：`src/plugins/baseline/task/baseline_task_base.h`
- 修改：`src/plugins/baseline/task/value_task.cpp`
- 修改：`src/plugins/baseline/task/ratio_task.cpp`
- 修改：`src/tests/test_baseline/test_baseline_plugin.cpp`

**任务分解**：

- [x] T18.12.1：定义 `ShadowState`
- [x] T18.12.2：接线漂移证据状态与重建触发
- [x] T18.12.3：实现 `shadow baseline` 接管与评分尺度放宽
- [x] T18.12.4：实现 candidate 验证与正式切换
- [x] T18.12.5：实现 `shadow` 退出逻辑
- [x] T18.12.6：编写 `shadow / rebuild / switch` 测试

**补充说明**：

- `Story 18.12` 先闭合 `shadow + candidate validator + formal apply` 的主状态机。
- `T1` 的 `sigma_ref` 正式尺度层，以及 `ShadowState` 中“引用模型身份”的最终工程收口，延后到 `Story 18.12A`。

---

### Story 18.12A：`T1` 正式尺度层与 `ShadowState` 引用模型收口

**优先级**：P1  
**工作量估算**：2 天  
**依赖**：Story 18.12

**验收标准**：

- [x] `T1` 正式模型可训练并持久化 `sigma_ref`
- [x] `T1` 热路径与 `shadow baseline` 评分统一切换到标准化残差口径
- [x] `ShadowState` 的工程表示不再依赖抽象 `ref_model_id`，而是显式冻结 `ref_kind / ref_source_key / ref_model_version + frozen_ref_model`
- [x] 测试覆盖 `sigma_ref` 训练 / 预测，以及 `shadow` 冻结引用模型后的接管行为

**文件**：

- 修改：`src/plugins/baseline/model/formal_model.h`
- 修改：`src/plugins/baseline/model/formal_predictor.h`
- 修改：`src/plugins/baseline/model/formal_predictor.cpp`
- 修改：`src/plugins/baseline/model/shadow_state.h`
- 修改：`src/plugins/baseline/rebuild/formal_model_trainer.h`
- 修改：`src/plugins/baseline/rebuild/formal_model_trainer.cpp`
- 修改：`src/plugins/baseline/task/value_task.cpp`
- 修改：`src/tests/test_baseline/test_baseline_plugin.cpp`

**任务分解**：

- [x] T18.12A.1：定义 `sigma_ref` 的训练、持久化与 predictor 输出契约
- [x] T18.12A.2：把 `T1` 正式评分与 `shadow baseline` 评分切换为标准化残差
- [x] T18.12A.3：将 `ShadowState.ref_model_id` 收口为显式冻结引用模型结构
- [x] T18.12A.4：编写 `sigma_ref / shadow ref` 测试

**实现回填**：

- `ValueFormalModel` 已持久化 `sigma_ref`，`formal_predictor` 对 `T1` 预测补出 `sigma_ref`
- `T1` 正式评分与 `shadow baseline` 评分都已切换为 `|residual| / (sigma_ref * rho_t)` 及其 `shadow` 放宽版本
- `QuerySeriesSnapshotJson()` 已补充 `formal_predict_sigma_ref / candidate_predict_sigma_ref`
- 为避免过早触发重建导致当前简化 holdout 验证无有效尾段，`shadow + rebuild` 激活增加了最小可验证窗口约束：连续确认点数至少为 3

---

### Story 18.13：`detector common` 与 `ValueDetectorCore`

**优先级**：P0  
**工作量估算**：2 天  
**依赖**：Story 18.12A

**验收标准**：

- [x] `src/plugins/baseline/detector/detector_common.h` 定义 `RebuildIntent / DetectorSubmitOutput / DetectorRebuildFailure / ValueSeriesSnapshot` 等内部契约
- [x] `ValueDetectorCore` 内部持有 `SeriesStore + ValueSeriesRuntimeState + series_override_map`，不再把这组热路径状态散落在 `BaselineValueTask`
- [x] `BaselineValueTask::SubmitObservation()` 改为委托 `ValueDetectorCore::Submit()`，task 层不再直接承载 `T1` 在线评分状态机
- [x] `BaselineValueTask::QuerySeriesSnapshotJson()` 改为基于 `ValueDetectorCore` 的结构化快照输出 JSON
- [x] `BaselineValueTask::ExecuteRebuild()` 的成功 / 失败回填统一经由 `ValueDetectorCore::ApplyFormalModel()` 与 `MarkRebuildFailure()`
- [x] `T1` 的 `self / configured_source / shadow / rebuild` 现有语义在重构后保持不回退

**文件**：

- 创建：`src/plugins/baseline/detector/detector_common.h`
- 创建：`src/plugins/baseline/detector/value_detector_core.h`
- 创建：`src/plugins/baseline/detector/value_detector_core.cpp`
- 修改：`src/plugins/baseline/task/value_task.h`
- 修改：`src/plugins/baseline/task/value_task.cpp`
- 修改：`src/plugins/baseline/CMakeLists.txt`
- 修改：`src/tests/test_baseline/test_baseline_value_task.cpp`
- 修改：`src/tests/test_baseline/test_baseline_plugin.cpp`

**任务分解**：

- [x] T18.13.1：定义 `detector_common` 公共契约，补齐 `RebuildIntent / DetectorSubmitOutput / DetectorRebuildFailure / ValueSeriesSnapshot`
- [x] T18.13.2：实现 `ValueDetectorCoreSpec` 与 `ValueDetectorCore` 的对象边界，接管 `profile / series_override_map / series_store / runtime_by_key`
- [x] T18.13.3：把 `ValueTaskHelper` 与 `SubmitObservation()` 的热路径主干迁入 `ValueDetectorCore::Submit()`
- [x] T18.13.4：把 `QuerySeriesSnapshotJson()` 的序列状态采集收口为 `ValueDetectorCore::BuildSeriesSnapshot()`
- [x] T18.13.5：把 `ApplyCandidateBuild / MarkCandidateFailure` 的序列状态回填迁入 `ValueDetectorCore`
- [x] T18.13.6：将 `BaselineValueTask` 瘦身为编排层，并编写 `T1` 回归测试

---

### Story 18.13A：`RatioDetectorCore` 与 `BaselineRatioTask` 瘦身

**优先级**：P0  
**工作量估算**：2 天  
**依赖**：Story 18.13、Story 18.12

**验收标准**：

- [x] `RatioDetectorCore` 内部持有 `SeriesStore + RatioSeriesRuntimeState + series_override_map`，`BaselineRatioTask` 不再自带完整 `T2` 热路径状态机
- [x] `BaselineRatioTask::SubmitObservation()` 改为委托 `RatioDetectorCore::Submit()`
- [x] `BaselineRatioTask::QuerySeriesSnapshotJson()` 改为基于 `RatioDetectorCore` 的结构化快照输出 JSON
- [x] `BaselineRatioTask::ExecuteRebuild()` 的成功 / 失败回填统一经由 `RatioDetectorCore::ApplyFormalModel()` 与 `MarkRebuildFailure()`
- [x] `T2` 的 `self / configured_source / shadow / rebuild` 现有语义在重构后保持不回退
- [x] `ValueDetectorCore / RatioDetectorCore` 的公共命名、结构和注释风格一致，但不强行模板化抹平数学差异

**文件**：

- 创建：`src/plugins/baseline/detector/ratio_detector_core.h`
- 创建：`src/plugins/baseline/detector/ratio_detector_core.cpp`
- 修改：`src/plugins/baseline/task/ratio_task.h`
- 修改：`src/plugins/baseline/task/ratio_task.cpp`
- 修改：`src/plugins/baseline/CMakeLists.txt`
- 修改：`src/tests/test_baseline/test_baseline_ratio_task.cpp`
- 修改：`src/tests/test_baseline/test_baseline_plugin.cpp`

**任务分解**：

- [x] T18.13A.1：定义 `RatioDetectorCoreSpec / RatioSeriesSnapshot`，与 `detector_common` 契约对齐
- [x] T18.13A.2：实现 `RatioDetectorCore` 的对象边界，接管 `profile / series_override_map / series_store / runtime_by_key`
- [x] T18.13A.3：把 `RatioTaskHelper` 与 `SubmitObservation()` 的热路径主干迁入 `RatioDetectorCore::Submit()`
- [x] T18.13A.4：把 `QuerySeriesSnapshotJson()` 的序列状态采集收口为 `RatioDetectorCore::BuildSeriesSnapshot()`
- [x] T18.13A.5：把 `ApplyCandidateBuild / MarkCandidateFailure` 的序列状态回填迁入 `RatioDetectorCore`
- [x] T18.13A.6：将 `BaselineRatioTask` 瘦身为编排层，并编写 `T2` 回归测试

---

### Story 18.13B：关系分布静态规格、basis 与摘要提取

**优先级**：P1  
**工作量估算**：3 天  
**依赖**：Story 18.2、Story 18.13A

**验收标准**：

- [x] `RelationTaskSpec` 与 `design.md` 中的 `T3TaskSpec` 语义完全一致，仅代码命名从 `T3` 收口为 `relation`
- [x] 同一 `RelationTaskSpec` 下全部 `metrics[]` 共享相同时间粒度、`bucket_id` 对齐方式与 group 划分口径，且只允许可加和质量指标
- [x] relation basis 对象可表达 `SupportExplicit / StableHeadSet / head_proto_q / ServiceBasis / EvalBasis / lineage compatibility`
- [x] 关系摘要提取可覆盖 `entropy_shannon / top1_share / headK_share / out_of_support_share / stable_g[i]_share`，并在 `active_count[m]` 存在时启用 `distinct_group_count`
- [x] 摘要提取对单个 `metric_m` 的热路径复杂度保持 `O(nnz)`，不依赖完整 group 值域空间大小
- [x] 仅完成规格、basis 与摘要提取，不在本 Story 接线 routed detector core

**文件**：

- 创建：`src/plugins/baseline/relation/relation_basis.h`
- 创建：`src/plugins/baseline/relation/relation_basis.cpp`
- 创建：`src/plugins/baseline/relation/relation_summary_extractor.h`
- 创建：`src/plugins/baseline/relation/relation_summary_extractor.cpp`
- 修改：`src/plugins/baseline/model/task_spec.h`
- 修改：`src/plugins/baseline/config_parser.cpp`
- 修改：`src/tests/test_baseline/test_baseline_relation_task.cpp`

**任务分解**：

- [x] T18.13B.1：补齐 `RelationTaskSpec`、`RelationSupportPolicySpec`、`RelationSummaryPolicySpec` 与配置解析约束
- [x] T18.13B.2：实现 `ServiceBasis / EvalBasis` 的数据结构，以及 `group_space_version` 兼容性与新 lineage 判断规则
- [x] T18.13B.3：实现 `SupportExplicit / StableHeadSet / head_proto_q` 的派生逻辑
- [x] T18.13B.4：实现核心摘要特征提取与可选 `distinct_group_count`
- [x] T18.13B.5：编写 `exact_sparse / topk_other / active_count 可选` 三类摘要提取测试

---

### Story 18.13C：`relation task` 热路径与 routed detector core 接线

**优先级**：P1  
**工作量估算**：3 天  
**依赖**：Story 18.13A、Story 18.13B

**验收标准**：

- [x] `IBaselineRelationTask::SubmitBlock()` 可用
- [x] 算法类型 `T3` 在代码结构中落地为“一个 `relation task` = 一个关系分布规格”
- [x] `relation task` 内部按 `value_cores_by_routed_feature / ratio_cores_by_routed_feature` 组织 routed detector core，而不是创建内部 child task
- [x] 一个 `RelationObservationBlock` 只解析一次，不为每个摘要特征重复拆解
- [x] 核心摘要特征可按设计路由到 `ValueDetectorCore / RatioDetectorCore`
- [x] task 级结果与重建意图可从多个 routed detector core 汇总，而不是简单暴露单个 routed feature 的结果

**文件**：

- 修改：`src/plugins/baseline/task/baseline_task_base.h`
- 修改：`src/plugins/baseline/task/baseline_task_base.cpp`
- 创建：`src/plugins/baseline/relation/relation_router.h`
- 创建：`src/plugins/baseline/relation/relation_router.cpp`
- 修改：`src/plugins/baseline/baseline_plugin.h`
- 修改：`src/plugins/baseline/baseline_plugin.cpp`
- 修改：`src/tests/test_baseline/test_baseline_relation_task.cpp`
- 修改：`src/tests/test_baseline/test_baseline_plugin.cpp`

**任务分解**：

- [x] T18.13C.1：实现 `BaselineRelationTask` 最小骨架、生命周期和 `SubmitBlock()` 入口
- [x] T18.13C.2：实现 relation task 的 per-key basis / runtime 状态容器
- [x] T18.13C.3：实现 routed feature 注册表与 `summary_feature -> value/ratio detector core` 的静态映射
- [x] T18.13C.4：接线 `RelationObservationBlock -> 摘要提取 -> routed detector core.Submit()`
- [x] T18.13C.5：实现 task 级结果合成与 `RebuildIntent` 聚合，确保不为每个摘要特征单独创建公开 task
- [x] T18.13C.6：编写关系块热路径、单次解析与 routed 接线测试

---

### Story 18.13D：`relation task` 正式重建、`EvalBasis` 与正式切换

**优先级**：P1  
**工作量估算**：4 天  
**依赖**：Story 18.13C

**验收标准**：

- [x] `relation task` 可装配 relation history reader，并通过异步 `RebuildRequest` 触发正式重建
- [x] 正式重建可基于历史 relation block 建立 / 刷新 `ServiceBasis`
- [x] candidate 与 incumbent 的比较统一在共同 `EvalBasis` 上进行，不直接比较不同 basis 下的摘要损失
- [x] 兼容 basis 刷新时，可在 `EvalBasis` 上完成摘要重放、routed detector 验证损失聚合与正式切换
- [x] `group_space_id / group_space_version` 不兼容变化时，relation task 按新 lineage 处理，不做 incumbent 直接比较
- [x] relation task 的 `shadow` 语义继续落在 routed 摘要特征序列层，task 级只负责编排与切换协调

**文件**：

- 修改：`src/plugins/baseline/task/baseline_task_base.h`
- 修改：`src/plugins/baseline/task/baseline_task_base.cpp`
- 修改：`src/plugins/baseline/detector/value_detector_core.h`
- 修改：`src/plugins/baseline/detector/value_detector_core.cpp`
- 修改：`src/plugins/baseline/detector/ratio_detector_core.h`
- 修改：`src/plugins/baseline/detector/ratio_detector_core.cpp`
- 修改：`src/plugins/baseline/relation/relation_basis.h`
- 修改：`src/plugins/baseline/relation/relation_basis.cpp`
- 修改：`src/plugins/baseline/relation/relation_summary_extractor.h`
- 修改：`src/plugins/baseline/relation/relation_summary_extractor.cpp`
- 修改：`src/plugins/baseline/relation/relation_router.h`
- 修改：`src/plugins/baseline/relation/relation_router.cpp`
- 修改：`src/tests/test_baseline/test_baseline_plugin.cpp`
- 修改：`src/tests/test_baseline/test_baseline_relation_task.cpp`

**任务分解**：

- [x] T18.13D.1：实现 relation block 历史回放与重建输入收集
- [x] T18.13D.2：实现 candidate `ServiceBasis` 构建、兼容性判断与 `EvalBasis` 选择
- [x] T18.13D.3：实现基于 `EvalBasis` 的摘要重放与 routed detector 验证损失聚合
- [x] T18.13D.4：实现 relation task 的正式切换、basis 刷新与失败回填
- [x] T18.13D.5：实现 `group_space_id / group_space_version` 不兼容时的新 lineage 分支
- [x] T18.13D.6：编写 relation rebuild / eval basis / formal switch 测试

---

### Story 18.14：测试、诊断与稳定性补强

**优先级**：P0  
**工作量估算**：3 天  
**依赖**：Story 18.1 ~ 18.10、Story 18.10A、Story 18.11、Story 18.12、Story 18.12A、Story 18.13、Story 18.13A、Story 18.13B、Story 18.13C、Story 18.13D

**验收标准**：

- [x] 建立 `src/tests/test_baseline/` 测试工程
- [x] 覆盖插件加载、task 生命周期、`T1 / T2`、重建、`Baseline Source`、`EventCalendarSpec`、`shadow`、`T3`、并发边界
- [x] 提供 task 级和 series 级诊断接口
- [x] 基本性能冒烟测试可运行

**文件**：

- 创建：`src/tests/test_baseline/test_common.h`
- 创建：`src/tests/test_baseline/CMakeLists.txt`
- 创建：`src/tests/test_baseline/test_baseline_plugin.cpp`
- 创建：`src/tests/test_baseline/test_baseline_value_task.cpp`
- 创建：`src/tests/test_baseline/test_baseline_ratio_task.cpp`
- 创建：`src/tests/test_baseline/test_baseline_rebuild.cpp`
- 创建：`src/tests/test_baseline/test_baseline_relation_task.cpp`
- 创建：`src/tests/test_baseline/test_baseline_concurrency.cpp`
- 修改：`src/CMakeLists.txt`
- 修改：`src/tests/test_baseline/CMakeLists.txt`

**任务分解**：

- [x] T18.14.1：建立测试工程与编译入口
- [x] T18.14.2：编写插件加载与 IID 查询测试
- [x] T18.14.3：编写 task 生命周期测试
- [x] T18.14.4：编写 `T1 / T2` 热路径测试
- [x] T18.14.5：编写 `history_reader` 缺失 / 失败测试
- [x] T18.14.6：编写 `Baseline Source` 与 `EventCalendarSpec` 测试
- [x] T18.14.7：编写 `shadow baseline` 激活 / 退出测试
- [x] T18.14.8：编写 `T3` 关系块输入测试
- [x] T18.14.9：编写并发与基本性能冒烟测试

---

## 推荐实施顺序

### 必须顺序执行

1. Story 18.1：公共接口与插件骨架
2. Story 18.2：Task 生命周期与配置解析
3. Story 18.3：`T1 / T2` 热路径公共状态层

### 可并行阶段

在 Story 18.3 完成后，可并行推进：

- Story 18.4：`T1` 热路径首版
- Story 18.5：`T2` 热路径首版

### 慢路径闭环阶段

在 `T1 / T2` 热路径打通后，继续推进：

4. Story 18.6：异步正式重建基础设施
5. Story 18.7：正式模型状态与 replay / candidate skeleton
6. Story 18.8：正式模型 apply 契约与 predictor skeleton
7. Story 18.9：正式训练与 `candidate builder`
8. Story 18.10：`Baseline Source` 配置与来源选择
9. Story 18.10A：`T1 / T2` 主评分链与 predictor 接线
10. Story 18.11：`EventCalendarSpec` 与日历版本契约
11. Story 18.12：漂移证据、`shadow baseline` 与正式切换
12. Story 18.12A：`T1` 正式尺度层与 `ShadowState` 引用模型收口

### 检测内核抽取阶段

13. Story 18.13：`detector common` 与 `ValueDetectorCore`
14. Story 18.13A：`RatioDetectorCore` 与 `BaselineRatioTask` 瘦身

### 关系分布阶段

15. Story 18.13B：关系分布静态规格、basis 与摘要提取
16. Story 18.13C：`relation task` 热路径与 routed detector core 接线
17. Story 18.13D：`relation task` 正式重建、`EvalBasis` 与正式切换

### 收尾阶段

18. Story 18.14：测试、诊断与稳定性补强

---

## `18.13` 系列领取顺序与并行边界

这一段专门约束 `detector core -> relation` 的实施顺序，避免编码时又退回“先写 relation，再临时补 core”的错误路径。

### Phase R1：先抽 `ValueDetectorCore`

先领取：Story 18.13

完成判据：

- `BaselineValueTask` 已改为持有 `ValueDetectorCore`
- `SubmitObservation / QuerySeriesSnapshotJson / ExecuteRebuild` 三条主链都已通过 core
- task 层不再同时保留另一份 `series_store_ / runtime_by_key_` 热路径状态

本阶段禁止并行：

- 不要提前做 `18.13B / 18.13C / 18.13D`
- 不要一边抽 `ValueDetectorCore`，一边让 `relation task` 直接依赖未稳定的 core 接口

原因：

- `detector_common` 的命名、`RebuildIntent`、snapshot 结构必须先稳定
- 否则后续 `RatioDetectorCore` 和 `relation task` 会跟着漂

### Phase R2：再抽 `RatioDetectorCore`

领取顺序：Story 18.13A

完成判据：

- `BaselineRatioTask` 已改为持有 `RatioDetectorCore`
- `ValueDetectorCore / RatioDetectorCore` 的公共命名、接口风格、慢路径回填语义一致
- `T1 / T2` 的 task 层都已收口为“生命周期 + 重建编排 + JSON 诊断”

本阶段并行建议：

- 不建议与 `18.13B` 并行编码

原因：

- `18.13B` 的 routed detector 规格、摘要路由目标、`RebuildIntent.routed_feature_id` 都依赖双 core 的最终接口形态
- 若 `18.13B` 过早开工，极易在 `relation` 侧固化一套过时接口

### Phase R3：冻结 relation 静态规格与摘要提取

领取顺序：Story 18.13B

完成判据：

- `RelationTaskSpec` 已与 `design.md` 对齐
- `ServiceBasis / EvalBasis / lineage compatibility` 数据结构已稳定
- 核心摘要特征提取与 `active_count` 可选增强已可单测

本阶段可并行点：

- `18.13B.2` basis 结构
- `18.13B.4` 摘要提取

前提：

- `18.13B.1` 的 `RelationTaskSpec` 与配置解析约束必须先收口
- `18.13B.3` 的 `SupportExplicit / StableHeadSet` 规则先稳定

说明：

- `18.13B` 完成前，不要启动 `relation task` 热路径接线
- 先把“摘要是什么、basis 是什么、比较基准是什么”冻结下来，再谈 block 如何路由

### Phase R4：接 `relation task` 热路径

领取顺序：Story 18.13C

完成判据：

- `IBaselineRelationTask::SubmitBlock()` 可用
- 一个 block 只解析一次
- routed feature 已通过 `ValueDetectorCore / RatioDetectorCore` 评分
- task 级结果与 `RebuildIntent` 聚合可工作

本阶段可并行点：

- `18.13C.2` per-key basis/runtime 容器
- `18.13C.3` routed feature 注册表

前提：

- `18.13C.1` relation task 骨架和 `SubmitBlock()` 入口先落地

本阶段禁止并行：

- 不要提前实现 `18.13D` 的 relation rebuild

原因：

- 没有先稳定热路径的 routed feature 身份和汇总语义，慢路径重放就没有可靠目标

### Phase R5：最后做 relation 正式重建与 `EvalBasis`

领取顺序：Story 18.13D

完成判据：

- relation history replay 可工作
- `candidate vs incumbent` 统一在 `EvalBasis` 上比较
- 兼容 basis 刷新可正式切换
- `group_space_id / group_space_version` 不兼容时按新 lineage 处理

本阶段并行建议：

- 不与其他核心 story 并行

原因：

- 这是 `relation` 语义最重的一段，实现时必须同时盯住 basis、摘要重放、routed detector 损失聚合和 formal switch
- 若再并行推进其他核心 story，最容易把 `EvalBasis` 比较偷简化掉

### 推荐领取链

严格推荐按下列顺序领取：

1. Story 18.13
2. Story 18.13A
3. Story 18.13B
4. Story 18.13C
5. Story 18.13D
6. Story 18.14

只有以下小粒度并行是允许的：

- `18.13B.2` 与 `18.13B.4`
- `18.13C.2` 与 `18.13C.3`

除此之外，不建议把 `18.13` 系列并行展开。

---

## 里程碑建议

### 里程碑 A：接口与对象模型稳定

包含：

- Story 18.1
- Story 18.2
- Story 18.3

目标：

- 插件可加载
- task 可创建与管理
- 热路径共用状态结构稳定

### 里程碑 B：`T1 / T2 + replay + trainer + source + scoring + calendar` 闭环

包含：

- Story 18.4
- Story 18.5
- Story 18.6
- Story 18.7
- Story 18.8
- Story 18.9
- Story 18.10
- Story 18.10A
- Story 18.11

目标：

- `T1 / T2` 同步评分可用
- 正式重建异步化
- 正式训练与 `candidate builder` 闭环
- 主评分链可消费 `self / configured_source` 的 predictor 输出
- `Baseline Source / EventCalendarSpec` 契约落地

### 里程碑 C：`shadow + detector core + relation + 测试与压测`

包含：

- Story 18.12
- Story 18.12A
- Story 18.13
- Story 18.13A
- Story 18.13B
- Story 18.13C
- Story 18.13D
- Story 18.14

目标：

- `shadow baseline` 与正式切换闭环
- `T1` 正式尺度层与 `ShadowState` 引用模型收口
- `T1 / T2` 在线检测主干抽为内部 `detector core`
- `relation` 关系分布规格 task、basis、摘要提取与正式重建闭环落地
- 插件具备完整测试与诊断能力

---

## 风险与缓解

| 风险 | 可能性 | 缓解措施 |
|------|--------|---------|
| `IBaselineTask` 生命周期处理不清，导致悬空指针 | 中 | 强制插件持有对象，统一通过 `Close()` 销毁，并为失效访问写测试 |
| 热路径与慢路径边界被打破，`history_reader` 意外进入同步调用 | 高 | 在接口层与实现层双重约束，并编写失败 / 缺失测试 |
| 正式训练链路停留在 replay / skeleton，无法真正产出可服务模型 | 高 | 单列 `FormalModelTrainer / CandidateBuilder` story，先闭合训练契约再做 `shadow` 与切换 |
| `Baseline Source` 配错来源口径，导致跨特征或跨粒度借用 | 中 | 在配置解析与来源选择层双重校验 `feature` 身份、粒度与对齐方式 |
| `EventCalendarSpec` 的 `calendar_id / calendar_version` 漂移，导致训练与预测语义不一致 | 中 | 在模型 metadata 中记录版本，并在 predictor 层强制比对，不一致则自动禁用事件块 |
| `shadow baseline` 与正式切换状态机复杂，容易产生重复切换或错误退出 | 高 | 单独抽出状态结构与验证组件，并要求清晰注释 |
| `detector core` 抽取不彻底，导致 task 与 core 双持热路径状态，后续语义漂移 | 高 | 强制把 `series_store_ / runtime_by_key_ / source 选择 / drift / shadow / rebuild 回填` 全部收口到 core，task 只保留编排职责 |
| `T3` 被实现成“每个摘要特征一个 task”，导致性能退化和语义走偏 | 中 | 在 `TaskSpec` 和 `relation_task` 结构中明确“一 task = 一关系分布规格” |
| `relation` 正式重建直接比较不同 basis 下的摘要损失，导致切换结论失真 | 高 | 在单列的 relation rebuild Story 中强制要求 `candidate vs incumbent` 统一投影到 `EvalBasis` 比较 |
| 过早自写通用数学与容器基础设施，造成实现面失控 | 中 | 严格遵守“优先复用成熟开源能力，不重复造轮子”原则 |
| 高并发场景下 per-key 状态更新竞态 | 中 | 明确并发约束，先做不同 key 并发，再验证同 key 顺序要求 |

---

## 实现原则摘录

- 热路径输入统一使用强类型结构体，不使用 JSON。
- 低频配置、快照和诊断接口允许使用 JSON。
- 对外同步接口只负责当前结果解释；正式重建必须异步。
- `history_reader` 只能进入慢路径。
- `BaselineSourceConfig` 与 `EventCalendarSpec` 都属于低频静态配置，不进入运行时 `Observation`。
- 借用 `Baseline Source` 只影响当前评分解释，不得中断本级训练与正式重建。
- 训练与预测若使用事件层，必须保证 `calendar_id + calendar_version` 一致；不一致时自动禁用事件块。
- `T1 / T2` 的 task 颗粒度是标量特征规格。
- `T3` 的 task 颗粒度是关系分布规格。
- 代码命名层不得继续使用 `T3` 作为目录、文件和核心对象名；统一使用 `relation` 表达工程职责。
- `relation` 的正式重建与切换必须经过 `EvalBasis`；禁止直接拿不同 basis 下的摘要损失做 `candidate vs incumbent` 比较。
- 涉及基线算法的代码必须有清晰注释，尤其是评分逻辑、状态迁移、`shadow baseline`、正式重建和 `T3` 摘要提取相关代码。

---

## Sprint 回顾

（待 Sprint 19 完成后填写）
