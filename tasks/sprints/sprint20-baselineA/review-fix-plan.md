# Sprint 20 BaselineA 复审修复计划

> **面向实现执行：** 本文用于承接 `Story 18.20` 最终一致性追加复审的整改工作。它替代原先仅面向 `14.4` 的局部修复计划，覆盖本轮新增识别出的正确性、设计一致性、热路径性能、内存边界与死代码清理问题。执行顺序固定为 `P0 -> P1 -> P2`，每完成一组任务都必须重新编译并复跑对应 baseline 测试。

## 1. 文档定位

本次追加复审不再局限于 `code-design.md 14.4`，而是按以下条款重新核对当前实现：

- `5.2.2 FusionResult`
- `7.8 冷启动与基线来源`
- `9.2.2 fusion/key_risk_fusion.*`
- `10.2.3 候选验证与正式切换`
- `11.1 高基数控制`
- `14.4 重建与切换`

结论是：baseline 主链已经具备可运行实现，但仍有若干残余问题没有按设计彻底收口，`Story 18.20` 不能再只按“测试通过”视为完成。

为避免和原 `14.4-fix-plan.md` 混淆，本文显式把问题分成两层：

- **A 类：原 14.4 继承问题**
  - 来自上一轮 `14.4` 定向修复计划的历史遗留项。
  - 主要集中在重建状态机、`candidate_fail`、`new lineage`、`insufficient_data / unavailable` 语义统一、`RemoveTaskContributions(task_id)` 清理验证。
- **B 类：本轮完整复审新增问题**
  - 来自本次对设计一致性、热路径性能、内存边界和死代码的追加审视。
  - 主要集中在 `FusionResult` 生命周期、`KeyRiskFusion` 热路径分配、锁争抢、高基数内存边界、死代码和测试 seam 污染。

后续每个整改任务都带有来源标签：

- `【14.4 继承】`
- `【新增复审】`
- `【两者交叉】`

## 2. 问题拆分

### 2.1 A 类：原 14.4 继承问题

以下问题继承自原 `14.4-fix-plan.md`，属于重建与切换闭环未完全收口：

1. **重建状态机中间态未完整落地**
   - `formal_model_state.h` 已定义 `building / built / validating`，但真实写路径仍主要停留在终态。

2. **`candidate_fail` 与 `new lineage` 仍需 task 级强证据**
   - 这两类场景虽然已有前一轮补修，但本轮复审认为还需要继续保留为重点验收项，防止后续重构回退。

3. **`insufficient_data / unavailable` 外部语义需继续统一**
   - 旧计划已经覆盖这一点，本轮保留为交叉验收项，因为它和状态机、热路径重构都会再次耦合。

4. **`RemoveTaskContributions(task_id)` 需要继续保留混合来源精确清理验证**
   - 旧计划只从 `14.4` 角度看“是否覆盖 direct / routed / pattern”；本轮会继续扩展为与新的 `KeyRiskFusion` 重构一起验收。

### 2.2 B 类：本轮新增复审问题

以下问题是这次完整复审新增识别出的内容，原 `14.4-fix-plan.md` 中没有体现：

1. **接口正确性与生命周期问题**
   - `BaselineRelationTask::SubmitBlock()` 返回的 `FusionResult` 目前借用了局部 `StoredFusionResult` 的字符串内存，存在悬空 `BaselineStringRef` 风险。

2. **设计契约与热路径分配问题**
   - `KeyRiskFusion` 虽然做了分片，但内部 `StoredFusionResult / StoredDominantPatternProjection` 仍大量使用 `std::string`、`std::vector`。
   - `RecomputeWindowRisk()` 每次更新都会构造临时 `vector`、排序、裁剪，且这些操作发生在 shard mutex 内。
   - 这与 `code-design.md` 对 `FusionResult` 固定上限数组、热路径避免动态分配的约束不一致。

3. **热路径锁争抢与慢路径侵入问题**
   - `RelationTask::SubmitBlock()` 在 `runtime_mutex_` 内调用 routed spec 物化逻辑，而该逻辑会触发 `source_resolver`、JSON 解析和 event calendar 裁剪。
   - `ValueDetectorCore / RatioDetectorCore` 仍使用单 task 全局 mutex，`SeriesStore` 也使用单 mutex，同一 task 下不同 key 会互相串行。

4. **内存边界、死代码与维护性问题**
   - `SeriesStore`、`runtime_by_key_`、`RelationTask::runtime_by_key_`、`KeyRiskFusion::states` 都是高基数常驻 map，目前缺少空闲 key 淘汰策略。
   - `formal_model_state.h` 已定义 `building / built / validating` 等状态，但真实写路径仍主要停留在终态。
   - `ValueDetectorCore` 保留了 `candidate_model` 在线可服务分支，但当前代码里看不到对应赋值路径。
   - `SeedMetricBasisForTesting()` 仍以生产符号导出，只被测试调用，属于测试缝隙侵入生产 ABI。

5. **时区语义与实现路径混淆问题**
   - 当前实现曾把 task `tz` 与进程环境 `TZ` 混为一谈，试图通过“单进程单时区注册”去掉热路径锁，这与设计文档中“`tz` 是 task-bound 业务时间语义”的定义冲突。
   - `config_parser.cpp` 仍把 `tz` 当必填字段，没有落实“缺省按业务默认时区建模”的需求。
   - `calendar_feature_helper.cpp` 通过 `setenv("TZ") + tzset() + localtime_r()` 实现按业务 `tz` 解释时间，导致出现 `TimezoneMutex` 这种由进程级副作用带来的热路径锁。
   - `EventCalendarEntry.tz` 在设计文档中是可选字段，但 compile 阶段曾错误收窄为必须显式提供。

## 3. 整改目标

- 先修复会导致错误结果或悬空引用的正确性问题。
- 把 `KeyRiskFusion` 与 `FusionResult` 内部存储重新收口到设计允许的固定上限、低分配实现。
- 把 resolver、JSON 解析、全局锁串行等慢路径行为从热路径剥离出去。
- 给高基数状态增加可解释、可验证的内存边界。
- 清理死分支、重复状态机与测试专用生产接口，降低后续漂移风险。

其中：

- **针对 A 类原 14.4 继承问题**
  - 目标是把原有闭环修复做成“不会被后续重构破坏”的稳定实现。
- **针对 B 类本轮新增复审问题**
  - 目标是补齐上一轮 `18.20` 未覆盖到的正确性、性能和内存边界问题。

## 4. 实施顺序

### 4.1 P0：先修正确性与协议实现偏差

#### 任务 P0-1【新增复审】：修复 `RelationTask::SubmitBlock()` 返回值生命周期

**修改文件：**

- `src/plugins/baseline/task/relation_task.cpp`
- `src/plugins/baseline/task/relation_task.h`
- `src/plugins/baseline/fusion/fusion_types.h`
- `src/tests/test_baseline/test_baseline_relation_task.cpp`
- `src/tests/test_baseline/test_baseline_plugin.cpp`

**实施要点：**

- `FusionResult` 的字符串引用必须指向 task-owned 或 snapshot-owned 的稳定存储，禁止再从局部 `StoredFusionResult` 借用 `c_str()`。
- 优先方案：
  - 先把最终选中的 `StoredFusionResult` 写入 `runtime_state.last_fusion_result`。
  - 再从该稳定存储物化 `FusionResult`，而不是从局部临时对象物化。
- 如果现有 `StoredFusionResult` 仍需要被复制，必须明确“返回引用的有效期”并让 backing storage 至少覆盖一次调用返回后的读取窗口。
- 新增回归测试，确保 `SubmitBlock()` 返回后读取 `out->key / dominant_single / dominant_pattern` 仍然稳定。

**验收标准：**

- `SubmitBlock()` 返回的 `FusionResult` 不再包含悬空 `BaselineStringRef`。
- 对应回归测试可稳定复现旧问题，并在修复后通过。

**实施补充（本轮已落地）：**

- `RelationTask::SubmitBlock()` 已改为先把最终结果写入
  `runtime_state.last_fusion_result`，再从稳定存储物化 `FusionResult`。
- `QuerySeriesSnapshotJson()` 与 `SubmitBlock()` 现在共享同一份稳定结果存储，
  不再从局部临时 `StoredFusionResult` 借用字符串内存。
- 回归验证已补齐并通过：
  - `test_baseline` 中的 `Baseline relation task result lifetime`
  - `test_baseline_relation_task`

#### 任务 P0-2【新增复审】：收口 `KeyRiskFusion` 的内部结果表示

**修改文件：**

- `src/plugins/baseline/fusion/key_risk_fusion.h`
- `src/plugins/baseline/fusion/key_risk_fusion.cpp`
- `src/plugins/baseline/fusion/fusion_types.h`
- `src/framework/interfaces/ibaseline_types.h`
- `src/tests/test_baseline/test_baseline_plugin.cpp`

**实施要点：**

- `StoredFusionResult`、`StoredDominantSingleProjection`、`StoredDominantPatternProjection` 不再使用动态 `vector` 作为 dominant 存储。
- 引入固定上限数组 + `count` 的内部表示，必要时用固定容量字符串封装承载 feature / pattern / metrics。
- `RecomputeWindowRisk()` 改为小数组 top-k 选择或固定容量插入排序，禁止每次更新都构造临时 `vector` 再全量排序。
- `UpdateRelationFusionResult()` 中的 `metrics_hit / supporting_features` 复制也要同步改成固定上限写入。

**验收标准：**

- `FusionResult` 对内对外都符合 `5.2.2` 固定上限契约。
- `KeyRiskFusion` 热路径不再为 dominant 集合额外分配动态容器。

**实施补充（本轮已落地）：**

- `StoredFusionResult`、`StoredDominantSingleProjection`、
  `StoredDominantPatternProjection` 已统一改为固定上限数组 + `count`。
- `metrics_hit / supporting_features` 也已改为固定容量数组，不再在热路径里扩容。
- `KeyRiskFusion` 与 `RelationPatternFusion` 的 top-k 选择已收口为有界数组插入，
  不再构造临时 `vector` 做全量排序。
- `MaterializeStoredFusionResult()` 已按固定上限契约对外物化，
  `test_baseline`、`test_baseline_relation_task` 已回归通过。

#### 任务 P0-3【新增复审】：修正业务时区语义并移除进程级 TZ 副作用

**修改文件：**

- `src/plugins/baseline/config_parser.cpp`
- `src/plugins/baseline/model/calendar_feature_helper.h`
- `src/plugins/baseline/model/calendar_feature_helper.cpp`
- `src/plugins/baseline/model/event_calendar_matcher.cpp`
- `src/plugins/baseline/CMakeLists.txt`
- `src/tests/test_baseline/CMakeLists.txt`
- `src/tests/test_baseline/test_baseline_model_helpers.cpp`

**新增编译依赖：**

- ICU 开发库（`uc`、`i18n`）
- CMake 入口使用 `find_package(ICU REQUIRED COMPONENTS uc i18n)` 显式声明
- 项目级依赖说明同步记录到仓库 `README.md`

**实施要点：**

- 明确区分：
  - 宿主进程系统时区；
  - baseline task `tz`（业务数据时间语义）。
- `BaselineTaskSpec.tz`、`RelationTaskClockSpec.tz` 改为“可缺省、缺省值为 `Asia/Shanghai`”。
- `EventCalendarEntry.tz` 继续保持可选；`local_wall_clock` 场景下若未显式提供，则继承 task `tz`，不能强制与 task `tz` 做额外单值收窄。
- `calendar_feature_helper.*` 不再通过 `setenv("TZ") / tzset()` 实现任意时区换算，改为显式按入参 `tz` 做转换，彻底消除 `TimezoneMutex` 的来源。
- 优先使用已有系统依赖可提供的时区库能力，避免重新造轮子；本轮实现采用 ICU 显式换算。

**验收标准：**

- 缺省不提供 `tz` 的 value / relation task 配置可被正常解析，且内部时区为 `Asia/Shanghai`。
- `local_wall_clock` 事件在 `entry.tz` 缺省时仍可合法 compile，并继承 task `tz`。
- `calendar_feature_helper.cpp` 中不再存在 `setenv("TZ")`、`tzset()` 和 `TimezoneMutex`。
- 现有 `DST / America/New_York / Asia/Shanghai` 时间语义测试继续通过。

**实施补充（本轮已落地）：**

- `config_parser.cpp` 已把 task `tz` 收口为可缺省，缺省值为 `Asia/Shanghai`。
- `calendar_feature_helper.cpp` 已移除 `setenv("TZ")`、`tzset()` 和
  `TimezoneMutex`，改为 ICU 显式时区换算。
- `EventCalendarEntry.tz` 继续保持可选，`local_wall_clock` 在缺省时继承 task `tz`。
- 编译依赖已补齐：
  - `src/plugins/baseline/CMakeLists.txt`
  - `src/tests/test_baseline/CMakeLists.txt`
  - `README.md`
- 对应测试已通过：
  - `test_baseline_model_helpers`
  - `test_baseline`

### 4.2 P1：热路径性能、高基数内存边界与 14.4 交叉收口

#### 任务 P1-1【新增复审】：把 routed spec 物化从 `RelationTask` 热路径锁内剥离

**修改文件：**

- `src/plugins/baseline/task/relation_task.cpp`
- `src/plugins/baseline/relation/relation_router.cpp`
- `src/plugins/baseline/relation/relation_router.h`
- `src/tests/test_baseline/test_baseline_relation_task.cpp`

**实施要点：**

- `MaterializeMetricRuntime()` 不能继续在 `runtime_mutex_` 内直接触发 `ResolveBaselineSource()`、JSON 解析与 `CropEventCalendarSpec()`。
- 优先按“快照 runtime 输入 -> 锁外构建 next specs -> 锁内原子替换”的两阶段模式重构。
- 对 routed feature 的重建条件做显式缓存，例如基于 `service_basis` 或 routed spec 输入变化触发，而不是每个 block 都重建一次。
- 若 `source_resolver` 没有版本号能力，至少保证：
  - 首次建立 routed runtime 时解析一次；
  - 后续只有在 basis / routed spec 真实变化时才刷新。

**验收标准：**

- `SubmitBlock()` 的锁内路径不再包含 resolver 调用和 JSON 解析。
- 同一 key 同一 metric 的 routed runtime 在 basis 不变时可复用。

**实施补充（本轮已落地）：**

- `RelationTask::SubmitBlock()` 已按“两阶段刷新”收口：
  - 锁内只读取当前 runtime 快照与刷新判定；
  - `MaterializeMetricRuntime()` 在锁外执行；
  - 锁内只回写更新后的 runtime。
- routed runtime 的复用条件已显式绑定到 basis 变化，
  basis 不变时不会重复触发 resolver / JSON 解析。
- 回归验证已通过：
  - `test_baseline` 中的 `Baseline relation task reuses routed runtime`
  - `test_baseline_relation_task`

#### 任务 P1-2【新增复审】：细化 `ValueDetectorCore / RatioDetectorCore` 锁粒度

**修改文件：**

- `src/plugins/baseline/detector/value_detector_core.h`
- `src/plugins/baseline/detector/value_detector_core.cpp`
- `src/plugins/baseline/detector/ratio_detector_core.h`
- `src/plugins/baseline/detector/ratio_detector_core.cpp`
- `src/plugins/baseline/model/series_store.h`
- `src/tests/test_baseline/test_baseline_concurrency.cpp`

**实施要点：**

- 现有单 task 全局 mutex 需要细化为固定数量的 shard 级；禁止按 key 动态生成 mutex。
- `SeriesStore` 与 detector runtime 最好采用相同分片键，避免一个请求先后卡在两个全局锁上。
- `baseline_source_config_by_key_` 继续保持只读 map，不进入争用路径。
- `BuildSeriesSnapshot()`、`ApplyFormalModel()`、`MarkRebuildFailure()` 也要按同样粒度收口，避免热路径细化后慢路径又回退成全局串行。

**验收标准：**

- 同一 task 下不同 key 的在线提交不再完全串行。
- 并发测试能够覆盖多 key 提交与关闭路径，不出现状态串扰。

**实施补充（本轮已落地）：**

- `ValueDetectorCore / RatioDetectorCore` 已统一改为固定 `64` 路 shard runtime，
  明确禁止按 key 动态生成 mutex。
- cross-key source fallback 读取已按 shard id 有序加锁，避免死锁。
- 热路径重建状态写入（`MarkRebuildEnqueued / Building / Built / Validating`）
  也已经落在同一组 shard runtime 内，避免回退到额外全局锁。
- 回归验证已通过：
  - `test_baseline_value_task`
  - `test_baseline_ratio_task`
  - `test_baseline_concurrency`

#### 任务 P1-3【新增复审】：补齐高基数状态的内存边界

**修改文件：**

- `src/plugins/baseline/model/series_store.h`
- `src/plugins/baseline/detector/value_detector_core.*`
- `src/plugins/baseline/detector/ratio_detector_core.*`
- `src/plugins/baseline/task/relation_task.*`
- `src/plugins/baseline/fusion/key_risk_fusion.*`
- `src/tests/test_baseline/test_baseline_plugin.cpp`
- `src/tests/test_baseline/test_baseline_concurrency.cpp`

**实施要点：**

- 为 `SeriesStore`、`runtime_by_key_`、`RelationTask::runtime_by_key_`、`KeyRiskFusion::states` 增加空闲 key 清理策略。
- 推荐使用：
  - 最近 bucket / 最近访问时间戳；
  - 最大空闲窗口阈值；
  - task snapshot / service stats 可观测的当前 key 数量。
- 若本轮不做自动淘汰，也必须至少把“上界由 task 生命周期决定”的事实明确写入 snapshot 与 review 文档，并补一个后续行动项。优先建议本轮直接实现轻量淘汰。

**验收标准：**

- 高基数 key 不会在长生命周期 task 中无限常驻。
- snapshot 或 service stats 能观察当前状态规模。

**实施补充（本轮已落地）：**

- `value / ratio detector core` 已引入统一的轻量空闲 key 清理：
  - 固定 `bucket gap` 阈值；
  - 递增 bucket 驱动的一次有界 opportunistic prune；
  - 固定 `scan_limit`，避免热路径退化成全量扫描。
- `RelationTask::runtime_by_key_` 已增加 `last_bucket_id`，并在提交路径内做同样的有界清理。
- `KeyRiskFusion` 已增加同口径空闲 key 清理，不再只依赖 task close 时的 `RemoveTaskContributions(task_id)`。
- 可观测性已补齐：
  - `ValueTask / RatioTask` 的 task snapshot 新增 `idle_prune_bucket_gap`、`pruned_key_count_total`
  - `RelationTask` 的 task snapshot 新增 `idle_prune_bucket_gap`、`pruned_key_count_total`
  - `BaselinePlugin::QueryServiceStatsJson()` 新增 `key_fusion_idle_prune_bucket_gap`、`key_fusion_pruned_key_count_total`
- 回归测试已补齐：
  - `test_baseline_value_task` / `test_baseline_ratio_task` 断言 stale key 会被清理
  - `test_baseline` 断言 relation runtime 与 key fusion key 数会回落，且旧 key 快照消失

### 4.3 P2：原 14.4 继承问题与死代码清理

#### 任务 P2-1【14.4 继承 + 新增复审】：补齐重建中间态并去掉无效分支

**修改文件：**

- `src/plugins/baseline/model/formal_model_state.h`
- `src/plugins/baseline/task/value_task.cpp`
- `src/plugins/baseline/task/ratio_task.cpp`
- `src/plugins/baseline/task/relation_task.cpp`
- `src/plugins/baseline/detector/value_detector_core.cpp`
- `src/plugins/baseline/detector/ratio_detector_core.cpp`
- `src/tests/test_baseline/test_baseline_plugin.cpp`

**实施要点：**

- `building / built / validating` 等中间态必须有真实写入点，不再只定义枚举不使用。
- 这项既是原 `14.4` 状态机遗留问题，也是本轮复审识别出的死代码/维护性问题。
- 检查 `ValueDetectorCore` 的 `candidate_model` 在线可服务分支：
  - 若确认不再需要，直接删除并同步清理 `candidate_self / candidate_source` 相关分支；
  - 若必须保留，则补齐赋值与状态转移路径，不能只保留读分支。
- `value_task.cpp` 与 `ratio_task.cpp` 中重复的 build / validate / apply / failure 映射逻辑，抽成共享 helper，避免三处状态机继续漂移。
- 同步复核：
  - `candidate_fail`
  - `new lineage`
  - `insufficient_data / unavailable`
  - `RemoveTaskContributions(task_id)`
  这些原 14.4 项在重构后不能回退。

**验收标准：**

- 状态机中间态可在 snapshot 或测试中被观测。
- detector 内不再保留无赋值来源的死分支。

**实施补充（本轮已落地）：**

- `FormalModelState` 与 relation key runtime 已补 `stage_trace`，统一暴露
  `stage_seen_building / stage_seen_built / stage_seen_validating`。
- `ValueDetectorCore` 在线来源已收口为 `formal-only`，不再把
  `candidate_model` 当作可服务来源；`candidate_self / candidate_source`
  证据分支已删除。
- `RatioDetectorCore` 的证据状态也已同步去掉 candidate 在线语义，保持与
  `T1` 同口径。
- `value / ratio / relation` 的 rebuild 流程都已增加真实写点：
  `rebuild_pending -> building -> built -> validating -> final`，
  并在 snapshot 中可直接观测。

#### 任务 P2-2【新增复审】：移除生产 ABI 中的测试缝隙

**修改文件：**

- `src/plugins/baseline/task/relation_task.h`
- `src/plugins/baseline/task/relation_task.cpp`
- `src/tests/test_baseline/test_baseline_relation_task.cpp`
- `src/tests/test_baseline/test_baseline_plugin.cpp`

**实施要点：**

- `SeedMetricBasisForTesting()` 不再以 `visibility("default")` 暴露在生产头文件中。
- 优先改为：
  - test-only helper；
  - friend fixture；
  - 或仅在测试目标可见的内部接口。
- 目标是保留测试能力，但不把“直接篡改 runtime state”的能力暴露给生产 ABI。

**验收标准：**

- 生产头文件不再暴露仅供测试使用的导出接口。
- 原有测试仍可通过新的 test seam 完成验证。

**实施补充（本轮已落地）：**

- `BaselineRelationTask::SeedMetricBasisForTesting()` 已从生产头文件与生产实现中移除。
- 新增 test-only helper：
  - `src/tests/test_baseline/relation_task_test_access.h`
- 生产类仅保留 `friend struct RelationTaskTestAccess;` 作为测试访问授权；
  - 不再导出测试专用方法；
  - 不再向生产 ABI 暴露可直接篡改 runtime state 的公开符号。
- 现有依赖点已完成迁移：
  - `test_baseline_relation_task.cpp`
  - `test_baseline_plugin.cpp`
- 额外核查要求：
  - `rg -n "SeedMetricBasisForTesting" src/plugins/baseline src/framework` 无生产命中；
  - `nm -D build-codex/output/libflowsql_baseline.so | rg "SeedMetricBasisForTesting|RelationTaskTestAccess"` 无导出符号。

## 5. 验证矩阵

每完成一组任务后至少执行以下验证：

### P0 后

- `cmake --build /mnt/d/working/flowSQL/build --target test_baseline test_baseline_relation_task test_baseline_plugin`
- `build/output/test_baseline_relation_task`
- `build/output/test_baseline`

### P1 后

- `cmake --build /mnt/d/working/flowSQL/build --target test_baseline test_baseline_value_task test_baseline_ratio_task test_baseline_relation_task test_baseline_concurrency`
- `build/output/test_baseline`
- `build/output/test_baseline_concurrency`

### P2 后

- `cmake --build /mnt/d/working/flowSQL/build --target test_baseline test_baseline_value_task test_baseline_ratio_task test_baseline_relation_task test_baseline_model_helpers test_baseline_task_headers test_baseline_rebuild test_baseline_concurrency`
- `build/output/test_baseline`
- `build/output/test_baseline_value_task`
- `build/output/test_baseline_ratio_task`
- `build/output/test_baseline_relation_task`
- `build/output/test_baseline_model_helpers`
- `build/output/test_baseline_task_headers`
- `build/output/test_baseline_rebuild`
- `build/output/test_baseline_concurrency`

### 5.1 A/B 类问题对应关系

- **A 类原 14.4 继承问题**
  - 主要由 `P2-1` 收口，并在 `test_baseline`、`test_baseline_rebuild`、`test_baseline_relation_task` 中复核。
- **B 类本轮新增复审问题**
  - `P0-1`、`P0-2`、`P1-1`、`P1-2`、`P1-3`、`P2-2` 为主要承载任务。
- **A/B 交叉问题**
  - `KeyRiskFusion` 重构后，`RemoveTaskContributions(task_id)` 需要继续回归验证。
  - 状态机补全后，`candidate_fail / new lineage / unavailable` 需要继续保持原 14.4 语义。

## 6. 完成判定

只有同时满足以下条件，才算本轮复审问题收口完成：

- **A 类原 14.4 继承问题完成条件**
  - `candidate_fail`、`new lineage`、`insufficient_data / unavailable`、`RemoveTaskContributions(task_id)` 在重构后仍然具备稳定证据。
  - 状态机中间态不再只是定义存在，而是可观测、可测试。
- **B 类本轮新增复审问题完成条件**
  - `RelationTask::SubmitBlock()` 返回结果不再存在悬空引用风险。
  - `KeyRiskFusion` 的 dominant 结构与热路径实现符合固定上限、低分配契约。
  - relation、value、ratio 三条热路径的锁粒度与慢路径边界已收口。
  - 高基数状态具备明确的淘汰或边界控制机制。
  - 测试专用生产 ABI 已清理。
- `RelationTask::SubmitBlock()` 返回结果不再存在悬空引用风险。
- `KeyRiskFusion` 的 dominant 结构与热路径实现符合固定上限、低分配契约。
- relation、value、ratio 三条热路径的锁粒度与慢路径边界已收口。
- 高基数状态具备明确的淘汰或边界控制机制。
- 状态机中间态、死分支和测试专用生产 ABI 已清理。
- 全量 baseline 测试重新编译、重新执行并通过。

## 7. 当前状态

截至本次文档回填时：

- `review-fix-plan.md` 中列出的整改任务，当前已具备以下状态：
  - 已完成：`P0-1`、`P0-2`、`P0-3`、`P1-1`、`P1-2`、`P1-3`、`P2-1`、`P2-2`
  - 未纳入本轮同步：`planning.md` 中对旧文件名的历史引用
- 本轮已经重新编译并执行通过的验证包括：
  - `/mnt/d/working/flowSQL/build-codex/output/test_baseline`
  - `/mnt/d/working/flowSQL/build-codex/output/test_baseline_value_task`
  - `/mnt/d/working/flowSQL/build-codex/output/test_baseline_ratio_task`
  - `/mnt/d/working/flowSQL/build-codex/output/test_baseline_relation_task`
  - `/mnt/d/working/flowSQL/build-codex/output/test_baseline_concurrency`
  - `/mnt/d/working/flowSQL/build-codex/output/test_baseline_model_helpers`
  - `/mnt/d/working/flowSQL/build-codex/output/test_baseline_task_headers`
  - `/mnt/d/working/flowSQL/build-codex/output/test_baseline_rebuild`
- 原 `14.4` 定向修复项仍保留 `A 类` 分区，仅用于帮助区分历史继承问题与本轮完整复审新增问题。
