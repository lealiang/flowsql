# Sprint 19：统一基线插件代码设计

## 1. 背景与目标

`design.md` 已完成统一基线方案的算法设计，覆盖：

- `T1`：数值时序基线
- `T2`：比例 / 率基线
- `T3`：关系分布摘要特征基线

术语边界：

- `T1 / T2 / T3` 只保留为算法分类术语，用于和 `design.md` 保持一致。
- 代码对象、目录、文件、类名、结构体名不得继续使用 `T3` 作为命名主词。
- 对应 `T3` 的代码侧统一使用 `relation` 命名，例如：
  - `RelationTaskSpec`
  - `BaselineRelationTask`
  - `src/plugins/baseline/relation/`
- 若某段文字同时讨论算法分类和代码对象，应明确写成“算法类型 `T3`，代码命名 `relation`”，避免混用。

本文件只讨论代码设计，不重复展开算法公式。目标是回答以下问题：

1. 统一基线能力在 `flowsql` 中应以什么形态落地。
2. 插件应如何命名。
3. 对外 `interface` 如何设计。
4. `task` 的正式颗粒度如何定义，尤其是 `T3`。
5. 热路径、慢路径、配置、重建和依赖复用在工程上如何收口。

设计边界：

- 本文只定义插件、接口、对象关系、生命周期和实现拆分建议。
- 本文不替代 `design.md` 中的算法规格。
- 本文不进入具体 `.cpp` 实现细节。

---

## 2. 设计结论总览

本轮代码设计先给出结论：

1. 统一基线能力不设计为 `flowsql` 算子，而是设计为通用插件。
2. 插件正式命名为 `baseline`。
3. 主接口命名为 `IBaselineService`。
4. 任务对象不采用泛化的 `IHandle`，而采用语义明确的 `IBaselineTask`。
5. 热路径数据统一使用强类型结构体；低频配置和诊断信息允许使用 JSON。
6. 对外主调用语义采用同步模式，但内部必须严格拆为：
   - 热路径同步评分
   - 慢路径异步正式重建
7. `history_reader` 不允许进入热路径，正式重建只能由插件内部后台线程 / 队列处理。
8. `task` 的颗粒度按“检测规格实例”定义：
   - `T1 / T2`：一个 task 对应一个标量特征规格
   - 算法类型 `T3`：代码侧一个 `relation task` 对应一个关系分布规格
9. 对内真正的建模与状态颗粒度始终是 `Series = (task, key, routed_feature)`；对外不暴露这一实现细节。
10. `BaselineSourceConfig` 与 `EventCalendarSpec` 都属于低频静态配置，不进入热路径 `Observation`。
11. `v1` 不新增专门的 series 管理接口；`BaselineSourceConfig` 通过 `config_json` 中的 `series_overrides` 注入，`EventCalendarSpec` 通过 task 级配置注入。
12. 慢路径内部对象链路固定为：`replay_runner -> formal_model_trainer -> candidate_builder -> candidate_validator`。

---

## 3. 插件定位与命名

### 3.1 为什么不用算子

若直接将基线能力做成 `flowsql` 算子，会带来两个问题：

1. 算子天然贴近数据流执行图，容易把基线功能与具体业务流和 SQL 计划绑定过紧。
2. 基线内部需要维护长期状态、后台重建线程、`shadow baseline` 和正式模型切换，这些都更接近“进程内长期服务”，而不是一次性执行的算子。

因此，统一基线能力应作为进程内通用插件存在，由上游业务模块、流式处理模块或其他服务通过 `interface` 调用。

### 3.2 正式命名

正式命名采用：

- 插件目录：`src/plugins/baseline`
- 插件类：`BaselinePlugin`
- 主接口：`IBaselineService`
- 任务接口：`IBaselineTask`
- 子接口：
  - `IBaselineValueTask`
  - `IBaselineRatioTask`
  - `IBaselineRelationTask`

命名理由：

- `baseline` 与算法设计文档中的主术语完全一致。
- 名称不绑定网络流量分析场景，具备通用性。
- 名称足够短，不会在类名、接口名和目录名上产生冗长负担。

---

## 4. 总体架构

### 4.1 进程内插件模式

统一基线插件遵循当前代码库的现有插件模式：

- 插件本体实现 `IPlugin`
- 同时暴露业务能力接口 `IBaselineService`
- 由 `PluginLoader` 注册并通过 `IQuerier` 被其他插件发现

形式上与现有 `npi`、`database` 等插件一致，只是业务职责不同。

### 4.2 热路径与慢路径分离

必须明确区分两条路径：

1. 热路径：同步调用，立即返回结果
2. 慢路径：异步重建，不得阻塞调用方

热路径职责：

- 接收当前观测
- 找到对应 task 与 key 的在线状态
- 执行当前 bucket 的评分
- 更新漂移证据状态
- 必要时激活 `shadow baseline`
- 必要时生成 `RebuildRequest` 并入内部队列
- 立即返回 `DetectorResult`

慢路径职责：

- 消费重建请求队列
- 调用 `history_reader`
- 拉取历史
- 训练 candidate model
- 执行 holdout / prequential 验证
- 切换正式模型

明确约束：

- `SubmitObservation()` / `SubmitBlock()` 绝不能直接调用 `history_reader`
- 正式重建必须在插件内部后台线程中完成
- 热路径只负责“发现需要重建”，不负责“完成重建”

### 4.3 同步对外、异步对内

本插件采用：

- 对外同步接口
- 对内双路径执行模型

这样既保持接入方调用简单，又不让慢路径污染热路径时延。

---

## 5. Task 颗粒度正式定义

### 5.1 总原则

本插件中，`task` 的正式语义不是“某次调用”，也不是“某个 key 的一次实例”，而是：

**一个检测规格实例。**

它定义的是：

- 该类数据的输入形态
- 该类数据的静态配置
- 该类数据的建模规则
- 该类数据的重建与诊断边界

### 5.2 对内与对外颗粒度分离

必须区分两种颗粒度：

1. 对外 `task` 颗粒度
2. 对内状态 / 建模颗粒度

对外：

- 调用方持有的是 `IBaselineTask`

对内：

- 插件内部真正维护的状态单元仍然是按 `Series` 或 `key` 展开的细粒度对象

这两者不能混淆，否则接口会变重或实现会失控。

### 5.3 `T1 / T2` 的 task 颗粒度

对 `T1 / T2`：

- 一个 task 对应一个标量特征规格
- task 内部允许接收很多 `key`
- 插件内部为每个 `key` 维护独立状态

例如：

- `bytes_total`
- `success_rate`
- `avg_rtt`

都分别对应不同 task；但某个 task 内部可以同时服务：

- `service_a`
- `service_b`
- `service_c`

这样的设计优点是：

- 配置自然
- task 数量可控
- 不需要为每个新 key 创建 / 删除 task
- 每个 key 仍保持独立建模状态

### 5.4 `T3` 的 task 颗粒度：关系分布规格

对算法类型 `T3`，task 颗粒度不能沿用“单标量特征”理解。代码侧的正式结论是：

**一个 `relation task` 对应一个关系分布规格。**

所谓“关系分布规格”，是指一套统一的关系分布建模模板，固定以下内容：

1. `key` 的语义  
   例如 `server_ip:port`、`service`、`tenant`

2. `group` 的语义  
   例如 `client_group`、`peer_group`、`dst_country_group`

3. `group_space_id / group_space_version`  
   即对端分组空间的定义与版本

4. `mass_metrics[]`  
   例如 `conn_count / bps / pps`

5. `support_policy`

6. `summary_policy`

7. `K_head / K_stable`

8. basis / support / stable head 的建立与切换规则

9. 输入块的物理结构

因此，一个 `relation task` 不是：

- 某个 key 的单条关系
- 某个摘要特征
- 某个单独 metric

而是：

**“这类关系分布如何输入、如何比较、如何建模”的整套规格。**

### 5.5 为什么 `T3` 必须按关系分布规格建 task

原因有 4 个：

1. `T3` 的原始输入天然是一个 block，而不是标量  
   一个 `RelationObservationBlock` 内部共享：
   - `group_idx`
   - `totals`
   - `active_count`
   - 多个 `mass_metric`

2. 摘要特征是 task 内派生结果，不是上游原始输入  
   例如：
   - `entropy_shannon`
   - `top1_share`
   - `out_of_support_share`
   - `stable_headK_mix_drift`

3. basis / support / stable head 属于同一规格下协同工作的一组状态  
   不适合拆成多个独立 task

4. 若按“摘要特征 task”或“单 metric task”拆分，会导致同一块数据被重复上传和重复解析，性能不可接受

### 5.6 Task 颗粒度结论

正式结论收口如下：

- `T1 / T2`：一个 task = 一个标量特征规格
- 算法类型 `T3`：代码侧一个 task = 一个关系分布规格
- task 内部可承载大量 `key`
- 内部真正的状态与重建颗粒度按 `(task, key)` 展开
- 对算法类型 `T3`，若进一步 routed 到摘要特征序列，则内部建模颗粒度进一步展开为 `(task, key, routed_feature)`，但这只是实现细节，不进入对外接口语义

---

## 6. 接口设计原则

### 6.1 热路径数据用结构体

热路径输入统一使用强类型结构体，不使用 JSON。

原因：

- 进程内调用不需要序列化 / 反序列化
- 高基数、高频输入必须避免 JSON 开销
- `RelationObservationBlock` 含有共享索引和多指标数组，JSON 结构不适合热路径

### 6.2 低频配置和诊断可用 JSON

低频接口允许使用 JSON：

- `Create*Task(config_json, ...)`
- `ConfigJson()`
- `QueryTaskSnapshotJson(...)`
- `QuerySeriesSnapshotJson(...)`

边界：

- JSON 只出现在低频配置、诊断、导出接口
- JSON 不进入 `SubmitObservation()` / `SubmitBlock()` 热路径

### 6.3 静态配置注入边界

`Create*Task(config_json)` 不只是“创建 task”，还承担低频静态配置注入职责。

本轮需要明确注入边界：

- task 级静态配置：
  - `feature profile`
  - `tz / delta`
  - `T3` 关系分布规格
  - `EventCalendarSpec`
- series 级静态覆盖：
  - `BaselineSourceConfig`

这里必须区分 2 个层次：

1. `EventCalendarSpec` 是 task 级静态规格  
   同一个 task 下所有 `key` 共用同一日历定义与版本语义。

2. `BaselineSourceConfig` 是 series 级静态覆盖  
   因为同一 task 会承载大量 `key`，不同 `key` 可配置不同的来源列表。

`v1` 的正式约束是：

- 不为 `BaselineSourceConfig` 单独新增一个公共管理接口
- 调用方通过 `Create*Task(config_json)` 一次性注入：
  - task 主规格
  - 可选 `series_overrides`
  - 可选 `EventCalendarSpec`
- 插件在 task 创建时把这些 JSON 配置解析为内部对象并长期持有

这样做的原因是：

- 保持 `IBaselineService` 首版接口收敛
- 不把低频配置更新问题过早扩展成另一套管理面协议
- 先把实现真正需要的配置语义闭合，再决定后续是否增加在线更新接口

若后续确实需要运行时变更 `BaselineSourceConfig`，应新增独立的低频管理接口，而不是把热路径接口塞满配置语义。

### 6.4 Task 对象由插件持有

任务对象采用：

- 插件拥有对象生命周期
- 调用方只借用 `IBaselineTask*`
- 调用方不得 `delete`
- 任务销毁统一通过 `Close()`

不采用：

- 泛化 `IHandle`
- COM 式 `Retain() / Release()`

这样可以降低 `v1` 的复杂度。

`Close()` 的正式语义进一步收口为同步关闭屏障：

- `Close()` 是低频管理接口，不是热路径操作
- 一旦 `Close()` 开始执行，该 task 必须先拒绝新的：
  - `SubmitObservation()`
  - `SubmitBlock()`
  - `SetHistoryReader()`
  - `RequestRebuild()`
- `Close()` 返回前，必须保证：
  - 该 task 的 in-flight 热路径调用已全部退出
  - 队列中属于该 task 的未执行重建请求已取消或排空
  - 后台 worker 不再持有该 task 的内部状态引用
- 只有在以上条件满足后，task 对象才允许真正销毁

这样定义的目的，是避免 `Close()` 返回后后台线程仍访问 task 内部状态，造成悬空指针或 `use-after-free`。

### 6.5 主接口同步，正式重建异步

同步接口只负责当前结果解释，不负责慢路径训练完成。

因此：

- `SubmitObservation()` / `SubmitBlock()` 返回的是“当前可用基线下的结果”
- 若已经触发漂移：
  - 可以返回 `shadow baseline` 解释下的结果
  - 同时在 `DetectorResult.flags` 或诊断信息中标出 `RebuildQueued`

---

## 7. 接口分层

### 7.1 总体分层

推荐接口分层如下：

```text
IBaselineService
  ├── CreateValueTask()    -> IBaselineValueTask
  ├── CreateRatioTask()    -> IBaselineRatioTask
  └── CreateRelationTask() -> IBaselineRelationTask

IBaselineTask
  ├── Id / Name / Kind / ConfigJson
  ├── QueryTaskSnapshotJson
  ├── QuerySeriesSnapshotJson
  ├── RequestRebuild
  └── Close

IBaselineValueTask
  ├── SetHistoryReader(IBaselineValueHistoryReader*)
  └── SubmitObservation(const ValueObservation&, DetectorResult*)

IBaselineRatioTask
  ├── SetHistoryReader(IBaselineRatioHistoryReader*)
  └── SubmitObservation(const RatioObservation&, DetectorResult*)

IBaselineRelationTask
  ├── SetHistoryReader(IBaselineRelationHistoryReader*)
  └── SubmitBlock(const RelationObservationBlock&, DetectorResult*)
```

### 7.2 为什么不做单一 `Submit()`

不建议把 `T1 / T2 / T3` 强行统一成一个 `Submit()`：

- `T1` 是标量值
- `T2` 是分子 / 分母
- `T3` 是整块稀疏关系分布

如果强行统一，会导致：

- 输入结构退化成大而全 union
- 多数字段在大多数任务里无意义
- 调用方容易误用
- 热路径分支和校验逻辑变重

因此，统一抽象落在 `IBaselineTask` 层，热路径输入则按类型拆分。

### 7.3 `IBaselineService`

职责：

- 创建 task
- 枚举 task
- 提供插件级统计与诊断

不负责：

- 直接处理高频观测
- 暴露每个 task 的热路径细节

### 7.4 `IBaselineTask`

职责：

- 暴露 task 元信息
- 暴露诊断接口
- 接收手动重建请求
- 提供销毁入口

不负责：

- 定义具体热路径输入结构

### 7.5 三类专用 task

#### `IBaselineValueTask`

用于：

- `T1a`
- `T1b`

运行时输入：

- `ValueObservation`

差异控制：

- `T1a / T1b` 的区别主要通过 task 配置里的 `feature_type / profile`
- `sample_count` 仅在 `T1b` 场景下有意义

#### `IBaselineRatioTask`

用于：

- `T2`

运行时输入：

- `RatioObservation`

原因：

- `T2` 的分子 / 分母语义明确，单独拆出最自然

#### `IBaselineRelationTask`

用于：

- `T3`

运行时输入：

- `RelationObservationBlock`

原因：

- `T3` 的高性能前提就是整块输入
- 一个 block 内共享 `group_idx` 和多指标数组

### 7.6 推荐方法签名草案

以下签名是 `v1` 推荐的正式草案，用于指导后续头文件设计：

```cpp
interface IBaselineTask {
    virtual ~IBaselineTask() = default;

    virtual const char* Id() const = 0;
    virtual const char* Name() const = 0;
    virtual BaselineTaskKind Kind() const = 0;
    virtual const char* ConfigJson() const = 0;

    virtual int QueryTaskSnapshotJson(std::string* out_json) const = 0;
    virtual int QuerySeriesSnapshotJson(const BaselineStringRef& key,
                                        std::string* out_json) const = 0;

    virtual int RequestRebuild(const BaselineStringRef& key,
                               BaselineRebuildReason reason) = 0;

    virtual int Close() = 0;
};

interface IBaselineValueHistoryReader {
    virtual ~IBaselineValueHistoryReader() = default;
    virtual int Fetch(const HistoryFetchRequest& req,
                      std::function<int(const ValueObservation&)> on_point) = 0;
};

interface IBaselineRatioHistoryReader {
    virtual ~IBaselineRatioHistoryReader() = default;
    virtual int Fetch(const HistoryFetchRequest& req,
                      std::function<int(const RatioObservation&)> on_point) = 0;
};

interface IBaselineRelationHistoryReader {
    virtual ~IBaselineRelationHistoryReader() = default;
    virtual int Fetch(const HistoryFetchRequest& req,
                      std::function<int(const RelationObservationBlock&)> on_block) = 0;
};

interface IBaselineValueTask : public IBaselineTask {
    virtual int SetHistoryReader(IBaselineValueHistoryReader* reader) = 0;
    virtual int SubmitObservation(const ValueObservation& obs,
                                  DetectorResult* out) = 0;
};

interface IBaselineRatioTask : public IBaselineTask {
    virtual int SetHistoryReader(IBaselineRatioHistoryReader* reader) = 0;
    virtual int SubmitObservation(const RatioObservation& obs,
                                  DetectorResult* out) = 0;
};

interface IBaselineRelationTask : public IBaselineTask {
    virtual int SetHistoryReader(IBaselineRelationHistoryReader* reader) = 0;
    virtual int SubmitBlock(const RelationObservationBlock& block,
                            DetectorResult* out) = 0;
};

interface IBaselineService {
    virtual ~IBaselineService() = default;

    virtual int CreateValueTask(const char* config_json,
                                IBaselineValueTask** out) = 0;
    virtual int CreateRatioTask(const char* config_json,
                                IBaselineRatioTask** out) = 0;
    virtual int CreateRelationTask(const char* config_json,
                                   IBaselineRelationTask** out) = 0;

    virtual void ListTasks(std::function<void(const char* task_id,
                                              const char* task_name,
                                              BaselineTaskKind kind)> cb) = 0;

    virtual int QueryServiceStatsJson(std::string* out_json) const = 0;
};
```

补充约束：

- `Create*Task()` 使用 JSON，只承担低频配置注入职责
- `SubmitObservation()` / `SubmitBlock()` 必须保持热路径纯结构体输入
- `SetHistoryReader()` 是低频装配接口，不允许在高频路径反复切换
- `RequestRebuild()` 只负责入队，不等待结果
- `Close()` 返回后，`IBaselineTask*` 立即失效
- `ListTasks()` 只做管理面枚举，不暴露内部 per-key 状态

其中，`Create*Task(config_json)` 在 `v1` 中还必须承载以下静态配置：

- `TaskSpec`
- 可选 `series_overrides`
- 可选 `EventCalendarSpec`

推荐把 `config_json` 的职责理解为：

```text
TaskConfigJson = {
  name,
  key,
  feature,
  feature_type,
  feature_profile,
  tz,
  delta,
  series_overrides?,
  event_calendar_spec?
}
```

其中：

- 顶层标量字段直接定义任务本体，不额外套一层 `task_spec`
- `series_overrides` 是稀疏表，只覆盖少量需要特殊 `BaselineSourceConfig` 的 key
- `event_calendar_spec` 是可选 task 级日历规格

这 3 类配置都属于低频管理面，不进入热路径输入结构。

其中有 2 条必须正式细化：

1. `SetHistoryReader()` 的所有权与切换契约

- `SetHistoryReader()` 不转移 reader 所有权
- reader 由调用方持有
- 调用方必须保证：自 `SetHistoryReader(reader)` 成功返回起，直到
  - `SetHistoryReader(nullptr)` 成功返回，或
  - `Close()` 返回
  之前，reader 始终有效
- `v1` 不允许在该 task 存在已入队或正在执行的重建任务时热切换 reader
- 若调用发生在重建进行中，建议直接返回 `busy` 类错误，并保持当前 reader 绑定不变

2. `Close()` 的关闭语义

- `Close()` 定义为同步关闭
- 它不是“标记一下稍后销毁”，而是“返回时必须已经不可再用”
- 因此，所有慢路径引用和队列请求都必须在 `Close()` 返回前被取消、排空或安全退出

这些规则的目标，是把 task 生命周期和后台重建线程之间的边界一次性写死，避免实现阶段出现多种不兼容做法。

这些签名是代码设计层的正式建议；具体 IID、错误码和命名细枝末节，可在实现阶段再做一次统一收口。

---

## 8. 核心数据结构

### 8.1 通用字符串、枚举与结果

建议先定义统一的借用字符串载体：

```text
BaselineStringRef = {
  data,
  size
}
```

正式语义：

- `BaselineStringRef` 是只读、非 owning 的字符串视图
- 允许空字符串，空字符串统一表示为：
  - `size = 0`
  - `data` 可为 `nullptr` 或指向任意可读地址，但实现层应优先规范为 `nullptr`
- 对热路径输入中的 `key`、以及 `QuerySeriesSnapshotJson()` / `RequestRebuild()` 这类低频接口中的 key，都统一使用 `BaselineStringRef`

内存语义必须明确：

- 调用方只需保证 `BaselineStringRef` 指向的内存在“本次接口调用返回前”有效
- 插件若要跨调用保存 `key`，必须复制内容，不能长期借用调用方内存
- 后台重建、快照索引、task 内部状态表等所有跨调用持久化场景，都必须基于插件内部自有拷贝

这样设计的目的，是避免在公共接口中直接暴露 `std::string` 所有权语义，同时把热路径字符串的借用边界写清楚。

建议统一定义以下通用枚举：

- `BaselineTaskKind`
- `BaselineDirection`
- `BaselineSeverity`
- `BaselineProvider`
- `BaselineReasonCode`
- `BaselineRebuildReason`

热路径统一输出：

- `DetectorResult`

`DetectorResult` 只保留高频必要字段：

- `status`
- `raw_score`
- `normalized_score`
- `confidence`
- `persistence`
- `direction`
- `severity`
- `provider`
- `reason`
- `flags`

详细解释不进入热路径结构体，而通过快照 / 诊断接口查询。

### 8.2 `ValueObservation`

用于 `T1a / T1b`：

```text
ValueObservation = {
  key,            // BaselineStringRef
  bucket_id,
  value,
  sample_count
}
```

说明：

- `sample_count` 对 `T1a` 可忽略
- 对 `T1b`，它是样本量门控与置信度修正输入

### 8.3 `RatioObservation`

用于 `T2`：

```text
RatioObservation = {
  key,            // BaselineStringRef
  bucket_id,
  numerator,
  denominator
}
```

说明：

- 比例值在插件内部派生，不作为热路径主输入

### 8.4 `RelationObservationBlock`

用于算法类型 `T3` 的 `relation task`：

```text
RelationObservationBlock = {
  key,            // BaselineStringRef
  bucket_id,
  nnz,
  group_idx[],
  metric_count,
  metrics[]
}
```

其中每个 `RelationMetricBlock` 至少包含：

- `total`
- `active_count`
- `values[]`

说明：

- `metrics[]` 的顺序由 `RelationTaskSpec.metrics[]` 固定
- block 内共享 `group_idx`
- 这正是关系分布基线的高性能输入基础

### 8.5 `HistoryFetchRequest`

正式重建慢路径统一读取请求：

```text
HistoryFetchRequest = {
  key,            // BaselineStringRef
  bucket_start,
  bucket_end
}
```

说明：

- `history_reader` 的输入颗粒度按 `(task, key, time_range)` 组织
- 这与 `design.md` 中的正式重建语义一致

### 8.6 `TaskSpec / SeriesOverride / EventCalendarSpec`

为了把 `BaselineSourceConfig` 和日历语义落到代码对象，内部至少需要以下结构：

```text
ScalarTaskSpec = {
  kind,
  task_name,
  feature_profile,
  tz,
  delta,
  event_calendar_spec?,
  series_overrides?
}

RelationSupportPolicySpec = {
  k_support,
  min_hist_share,
  min_active_ratio
}

RelationSummaryPolicySpec = {
  k_head,
  k_stable
}

RelationTaskSpec = {
  task_id,
  task_name,
  feature_base,
  group_space_id,
  group_space_version?,
  metric_set_id,
  metrics[],
  encode_type,
  support_policy,
  summary_policy,
  config_json
}

SeriesOverride = {
  key,
  baseline_sources[]
}

BaselineSourceRef = {
  source_key
}
```

说明：

- `ScalarTaskSpec` 对应 `T1 / T2` 的对外 task 规格。
- `RelationTaskSpec` 对应算法类型 `T3` 的代码侧 task 规格。
- `RelationTaskSpec` 必须与 `design.md` 中的 `T3TaskSpec` 数学语义保持完全一致，只允许代码命名从 `T3` 收口为 `relation`，不允许字段减配。
- `task_id` 是 task 注册完成后的稳定静态标识；它不要求直接来自 `config_json`，但一旦 task 创建成功，就必须成为 `RelationTaskSpec` 的组成部分。
- `support_policy` 必须完整持有 `k_support / min_hist_share / min_active_ratio`；不得只保留 `k_support` 这种不完整占位版本。
- `summary_policy` 必须完整持有 `k_head / k_stable`。
- 同一 `RelationTaskSpec` 下的全部 `metrics[]` 必须共享相同的时间粒度、`bucket_id` 对齐方式与 group 划分口径。
- `metrics[]` 必须与 `design.md` 一致，只接受可加和质量指标；不得在代码设计层缩成“随便几个字符串”。
- 非可加和统计量不得直接进入 `metrics[]`；若业务上需要这类量，必须先由上游改写成可加和组成量再进入关系分布基线。
- `encode_type` 的代码侧取值与 `design.md` 一致，首版收口为 `exact_sparse | topk_other`。
- `EventCalendarSpec` 直接作为 `ScalarTaskSpec` 的可选组成部分
- `SeriesOverride` 是稀疏表，不要求覆盖 task 内所有 key
- `BaselineSourceRef` 在 `T1 / T2` 中只需要 `source_key`
- 对算法类型 `T3`，来源选择发生在 routed 后的内部摘要序列层；公共配置层仍保持“同一关系 task 内的同类来源”语义，不把 routed 细节暴露到对外接口

`v1` 的工程约束：

- `ScalarTaskSpec / RelationTaskSpec` 一经创建即视为静态配置
- `series_overrides` 只允许在 task 创建时注入
- 热路径只读取已解析好的内部结构，不解析 JSON

### 8.6.1 内部 routed detector core

为让关系分布摘要特征复用现有 `T1 / T2` 检测主干，而不是在 `relation task` 里复制一套评分状态机，代码侧必须先抽出内部 `routed detector core`。

本节不是在现有 `T1 / T2` 旁边再新增一套平行实现，而是要把 `value_task / ratio_task` 中本来就同构、且后续 `relation task` 也必须复用的在线检测主干抽出来。也就是说：

- 文件形态上，`detector/` 是新增目录与新增文件。
- 算法语义上，`detector/` 承接的是现有 `T1 / T2` 热路径主干的内核抽取。
- `value task / ratio task` 后续都改为调用 detector core，而不是继续各自保留一套完整状态机。

`v1` 的设计取向是“显式双核、避免过度抽象”：

- 明确保留 `ValueDetectorCore` 与 `RatioDetectorCore` 两套 core，不做模板大一统
- 公共边界只抽到“状态机职责”和“调用契约”这一层
- 值变换、方差、评分公式、`shadow` 更新公式仍分别留在 value / ratio 各自 core 中

这样做的原因很直接：

- `T1/T1b` 与 `T2` 的数学差异是实质性的，强行合并成模板层只会让代码更绕
- 但两者在工程流程上高度相似：输入校验、时序推进、formal/source 选择、漂移证据、`shadow baseline`、重建意图、慢路径回填
- `relation task` 需要复用的是这条工程流程，而不是重新发明一套针对摘要特征的状态机

#### 8.6.1.1 目标与非目标

设计目标：

- `value task / ratio task` 与 `relation task` 共享同一套在线检测流程骨架
- `relation task` 路由摘要特征时，不创建内部 child task，也不走插件公开接口回调
- 热路径评分、漂移证据、`shadow baseline`、`Baseline Source`、正式模型状态与重建意图都由 detector core 统一维护
- task 层只负责生命周期、`history_reader`、正式重建编排、诊断聚合与对外 JSON

明确非目标：

- 不再新增一套与现有 `value_task / ratio_task` 并存的“第三套实现”
- 不把 `RelationObservationBlock`、basis / support / stable head 这类关系分布特有逻辑塞进 detector core
- 不把 detector core 暴露成新的公开插件接口；它只是插件内部对象

推荐文件与命名：

```text
src/plugins/baseline/
  detector/
    detector_common.h
    value_detector_core.h/.cpp
    ratio_detector_core.h/.cpp
```

#### 8.6.1.2 现有代码的抽取矩阵

为了避免“说抽内核，但真正编码时还是整段复制”，这里明确现有 `value_task.cpp / ratio_task.cpp` 中哪些职责要下沉、哪些要保留在 task 层。

| 现有职责 | 新归属 | 设计说明 |
| --- | --- | --- |
| `BuildProfile / ValidateObservation / TransformValue / ComputeRho` | detector core | 这是标量在线检测的前处理语义，普通 task 与 relation routed feature 都要复用 |
| `series_store_`、`series_runtime_`、`series_runtime_mutex_` | detector core | 这些是单特征热路径状态，不应继续挂在 task 层 |
| `ResolveServiceableBaseline()`、formal/candidate/source 选择 | detector core | 这是基线可服务选择逻辑，`relation task` 也必须复用 |
| `SubmitObservation()` 中的在线评分、漂移更新、`shadow` 激活与更新 | detector core | 这是这次抽内核的核心目标 |
| `ApplyCandidateBuild()`、`MarkCandidateFailure()` 的状态写回 | detector core | 慢路径结果应直接回填到 core 持有的序列状态 |
| `QuerySeriesSnapshotJson()` 的字段收集 | detector core 生成结构化快照，task 负责序列化 JSON | 快照语义是序列级，归 core；JSON 输出是接口层，归 task |
| task 注册、`config_json` 解析、`history_reader` 绑定 | task 层 | 这是对外接口与生命周期职责 |
| `RequestRebuild()`、重建队列入队、后台 worker 协调 | task 层 | detector core 只产生意图，不直接碰队列 |
| `ExecuteRebuild()` 中的历史读取、candidate build、validate、full train | task 层 | 这是慢路径编排，不是在线检测内核 |
| relation 的 basis / support / stable head / 摘要提取 | relation task | 这是关系分布特有逻辑，不能下沉到 detector core |

上表的直接含义是：

- `value_task / ratio_task` 未来不再直接持有 `series_store_ + series_runtime_`
- 当前 `SubmitObservation()` 中的大部分热路径代码，应迁入 `ValueDetectorCore::Submit()` / `RatioDetectorCore::Submit()`
- task 层保留“编排者”角色，detector core 保留“单特征状态机”角色

#### 8.6.1.3 detector core 持有的对象与状态

核心边界：

```text
ValueDetectorCore
  - 拥有某个 routed value feature 的 task 内静态配置
  - 内部按 key 维护 SeriesStore + ValueSeriesRuntimeState
  - 接收 ValueObservation
  - 产出 DetectorResult + RebuildIntent
  - 提供结构化 series snapshot
  - 接收慢路径正式切换 / 失败回填

RatioDetectorCore
  - 拥有某个 routed ratio feature 的 task 内静态配置
  - 内部按 key 维护 SeriesStore + RatioSeriesRuntimeState
  - 接收 RatioObservation
  - 产出 DetectorResult + RebuildIntent
  - 提供结构化 series snapshot
  - 接收慢路径正式切换 / 失败回填
```

`v1` 不建议再把 per-key 状态继续拆成很多微对象，而是直接以当前已经存在的 `ValueSeriesRuntimeState / RatioSeriesRuntimeState` 为基础，把它们从 task 层挪到 detector core 内部持有。原因：

- 这些状态字段已经与当前热路径一一对应，工程含义明确
- 先完成“职责迁移”比先做“状态重构”更重要
- 过早拆分成很多小对象，只会增加迁移成本和回归风险

因此，detector core 的内部形态建议直接收口为：

```text
ValueDetectorCore
  -> spec_
  -> profile_
  -> series_override_map_
  -> series_store_
  -> runtime_mutex_
  -> runtime_by_key_

RatioDetectorCore
  -> spec_
  -> profile_
  -> series_override_map_
  -> series_store_
  -> runtime_mutex_
  -> runtime_by_key_
```

其中：

- `series_store_` 继续负责 `gap / persistence / observation_count / out_of_order` 这类公共时序状态
- `runtime_by_key_` 负责保存该特征类型特有的在线检测状态，如 `last_x / last_rho / drift_state / shadow_state / formal_state`
- `profile_` 是已经从 task 级静态配置导出的、可直接热路径使用的特征参数
- `series_override_map_` 是该 routed feature 上可直接使用的 `Baseline Source` 配置

#### 8.6.1.4 detector common 的公共结构

建议在 `detector_common.h` 中定义所有 detector core 共用的轻量契约：

```text
RebuildIntent = {
  required,
  reason,
  rebuild_start_hint,
  bucket_end,
  routed_feature_id
}

DetectorSubmitOutput = {
  detector_result,
  rebuild_intent
}

DetectorRebuildFailure = {
  key,
  request_bucket_start,
  request_bucket_end,
  candidate_state
}
```

字段说明：

- `required`：当前提交后是否需要 task 层发起正式重建
- `reason`：本次建议重建的原因，通常来自漂移确认或显式重建请求
- `rebuild_start_hint`：建议从哪个 bucket 开始取历史数据；对漂移型重建默认对应 `τ_hat`
- `bucket_end`：当前 detector 希望重建覆盖到的终点
- `routed_feature_id`：该重建意图来自哪个 routed feature。普通 `value task / ratio task` 下它等于该 task 的自身 feature；对 `relation task`，它等于某个摘要特征身份

这里特意保留 `routed_feature_id`，原因是 `relation task` 需要把多个 routed detector core 的重建意图汇总成一次关系分布重建请求；没有这个字段，后续诊断只能看到“task 要重建”，看不到是哪些摘要特征触发了它。

#### 8.6.1.5 detector core 的接口设计

最小接口建议：

```text
class ValueDetectorCore {
 public:
  Submit(obs, out_submit)
  BuildSeriesSnapshot(key, out_snapshot)
  ApplyFormalModel(key, apply_result)
  MarkRebuildFailure(failure_result)
  Size()
}

class RatioDetectorCore {
 public:
  Submit(obs, out_submit)
  BuildSeriesSnapshot(key, out_snapshot)
  ApplyFormalModel(key, apply_result)
  MarkRebuildFailure(failure_result)
  Size()
}
```

建议补齐成如下更具体的输入输出：

```text
ValueDetectorCoreSpec = {
  owner_task_id,
  routed_feature_id,
  feature_profile,
  event_calendar_spec?,
  series_overrides?
}

RatioDetectorCoreSpec = {
  owner_task_id,
  routed_feature_id,
  feature_profile,
  event_calendar_spec?,
  series_overrides?
}

ValueSeriesSnapshot = {
  series_state,
  runtime_state,
  formal_prediction,
  candidate_prediction
}

RatioSeriesSnapshot = {
  series_state,
  runtime_state,
  formal_prediction,
  candidate_prediction
}
```

说明：

- 对普通 `value task / ratio task`，`routed_feature_id` 就是该 task 的自身 feature
- 对 `relation task`，`routed_feature_id` 是摘要特征身份，例如 `client_group_mix_bps_entropy_shannon`
- `BuildSeriesSnapshot(...)` 返回结构化快照，由 task 层再决定如何输出 JSON
- `ApplyFormalModel(...)` 不是“只写入 formal model 指针”这么窄，它同时负责回填 `candidate / validation / switch_state / replay_window` 等重建成功结果
- 对 `relation task` 的 basis 刷新，`ApplyFormalModel(...)` 还必须支持“覆盖或清空旧 formal model”，避免 basis 已切换但 routed detector 仍沿用旧摘要语义下训练出的模型
- `MarkRebuildFailure(...)` 则负责回填 fetch 失败、训练失败、验证失败等慢路径失败结果

这里保留 `ApplyFormalModel(...)` 这个名字，是为了减少当前文档和计划的连锁改动；但在实现语义上，应把它理解为“应用一次成功的重建结果”，而不是仅替换一个模型指针。

#### 8.6.1.6 `Submit()` 的标准热路径流程

为了让编码时不再出现“value/ratio 各写一套相似大函数”，两类 core 都应遵循同一套处理阶段，只是在数学细节上各自实现。

标准流程如下：

1. 输入校验  
   由 detector core 自己调用对应的 `ValidateObservation(...)`

2. 观测归一化  
   - value：计算 `x_t = transform(value)`、`rho_t`、`gate_score`、`gate_shift`
   - ratio：计算 `p_t = numerator / denominator`、`rho_t`、`gate_score`、`gate_shift`

3. 公共时序状态推进  
   调用 `series_store_.ApplyObservation(...)`，得到 `gap / persistence / flags`

4. 读取并更新该 `key` 的 runtime state  
   更新 `last_*` 字段，例如 `last_value`、`last_rho`、`last_gate_score`

5. `shadow baseline` 可用性检查  
   若当前有 gap 断裂、引用模型不可预测，或引用版本失效，则重置 `shadow_state`

6. 选择当前可服务基线  
   优先级保持与当前设计一致：
   - self formal
   - self candidate
   - configured baseline source formal
   - configured baseline source candidate

7. 若 `shadow` 已激活，则走 shadow 路径评分  
   - 使用冻结引用模型预测
   - 叠加当前 `delta`
   - 计算残差、`score_point`
   - 在 `gate_score` 成立时，在线更新 `delta`

8. 若 `shadow` 未激活但 formal/source 可服务，则走常规 formal 路径  
   - 调 formal predictor
   - 计算残差、标准化残差、`score_point`
   - 更新 `drift_state`
   - 若漂移确认并满足工程保护条件，则激活 `shadow baseline`
   - 生成 `RebuildIntent`

9. 组装 `DetectorResult`  
   写入方向、原因、分数、flags、baseline provider 等输出

10. 返回 `DetectorSubmitOutput`  
    task 层只读取 `detector_result` 和 `rebuild_intent`，不再参与内部状态机判断

这里的“相同流程，不同公式”要明确到实现层：

- value core 负责 `log1p` 变换、`sigma_ref * rho_t` 尺度、value 型 shadow 残差
- ratio core 负责 `numerator / denominator` 观测、`phi_over * d * p * (1-p)` 方差、ratio 型 shadow 偏移

因此，`detector/` 的目标是统一流程骨架，而不是抹平数值差异。

#### 8.6.1.7 task 层如何接线 detector core

`value task / ratio task` 的未来形态应明显变薄，推荐收口为：

```text
BaselineValueTask
  -> core_
  -> history_binding_
  -> rebuild_runtime_

BaselineRatioTask
  -> core_
  -> history_binding_
  -> rebuild_runtime_
```

对应的典型调用流：

```text
SubmitObservation(obs):
  EnsureOpen()
  core_.Submit(obs, &submit_output)
  if submit_output.rebuild_intent.required:
    EnqueueRebuild(submit_output.rebuild_intent)
  *out = submit_output.detector_result
```

```text
QuerySeriesSnapshotJson(key):
  core_.BuildSeriesSnapshot(key, &snapshot)
  SerializeSnapshotToJson(snapshot)
```

```text
ExecuteRebuild(request):
  FetchHistory(...)
  BuildCandidate(...)
  ValidateCandidate(...)
  TrainFullModelIfPass(...)
  if success:
    core_.ApplyFormalModel(key, apply_result)
  else:
    core_.MarkRebuildFailure(failure_result)
```

也就是说，task 层在 detector core 抽出后只剩 4 类职责：

- 对外接口与生命周期
- 历史读取与后台重建编排
- 重建队列入队与关闭时并发保护
- JSON 诊断输出

而“这个 bucket 来了以后如何更新在线状态”这一整段逻辑，不再留在 task 层。

#### 8.6.1.8 relation task 如何复用 detector core

关系任务内部的组织方式建议为：

```text
RelationTask
  -> basis_state_by_key
  -> value_cores_by_routed_feature
  -> ratio_cores_by_routed_feature
```

其中：

- `value_cores_by_routed_feature[routed_feature_id]` 表示某个 routed 标量摘要特征对应的一套 `ValueDetectorCore`
- `ratio_cores_by_routed_feature[routed_feature_id]` 表示某个 routed share 类摘要特征对应的一套 `RatioDetectorCore`
- 每个 detector core 内部再按 `key` 展开真正的序列状态

关系任务的热路径应收口为：

```text
SubmitBlock(block):
  ValidateBlock(block)
  UpdateBasisState(key)
  ExtractSummaryFeatures(block, basis_state)
  RouteToValueOrRatioObservation(...)
  ForEach routed_feature:
    detector_core.Submit(routed_obs, &submit_output)
    MergeFeatureResult(submit_output.detector_result)
    MergeRebuildIntent(submit_output.rebuild_intent)
  MaybeEnqueueOneRelationRebuild(merged_intent)
  EmitRelationTaskResult(...)
```

这里最关键的 3 条约束必须明确：

- 同一个 block 只解析一次，不为每个摘要特征重复拆解
- routed detector core 只负责单摘要特征的在线检测，不知道 `RelationObservationBlock`
- `relation task` 只能在 task 粒度上合并重建请求，不能因为某个 routed feature 触发就偷偷创建内部公开 task

因此，对 `relation task` 来说，detector core 的价值非常具体：

- 复用 `T1/T2` 已有的 formal/source 选择、漂移证据、`shadow baseline`、正式切换语义
- 避免在 `relation task` 里再写一套“摘要特征版 `T1/T2`”
- 保持内部真实序列语义为 `Series = (task, key, routed_feature)`，同时对外仍然只暴露“一个 relation task = 一个关系分布规格”

#### 8.6.1.9 迁移后的文件边界

结合当前代码骨架，推荐的迁移边界如下：

```text
task/value_task.h/.cpp
  - 保留 BaselineValueTask
  - 删除热路径状态机实现
  - 改为持有 ValueDetectorCore

task/ratio_task.h/.cpp
  - 保留 BaselineRatioTask
  - 删除热路径状态机实现
  - 改为持有 RatioDetectorCore

detector/value_detector_core.h/.cpp
  - 承接原 ValueTaskHelper
  - 承接原 SubmitObservation 的在线评分主干
  - 承接原 ApplyCandidateBuild / MarkCandidateFailure 的序列状态回填

detector/ratio_detector_core.h/.cpp
  - 承接原 RatioTaskHelper
  - 承接原 SubmitObservation 的在线评分主干
  - 承接原 ApplyCandidateBuild / MarkCandidateFailure 的序列状态回填
```

这一定义的本质是：

- `detector/` 不是薄封装层
- `detector/` 也不是平行新增实现
- `detector/` 是把现有 `T1/T2` 在线检测主干从 task 层挪出来、变成可复用内部核心的正式落点

### 8.7 正式模型元数据与候选构建结果

为避免慢路径继续停留在“只有 skeleton、没有正式训练产物”的状态，需要把内部对象补齐为：

```text
FormalModelMetadata = {
  model_version,
  train_bucket_start,
  train_bucket_end,
  holdout_count,
  calendar_id?,
  calendar_version?
}

CandidateBuildResult = {
  status,
  candidate_model?,
  metadata?,
  failure_reason
}
```

说明：

- `FormalModelMetadata` 是正式模型的一部分，不是调试临时字段
- `calendar_id / calendar_version` 是事件层契约的最小持久化信息
- `CandidateBuildResult` 由 `candidate_builder` 产出，供 `candidate_validator` 和正式切换逻辑消费
- 训练失败时返回结构化 `failure_reason`，而不是只记日志后丢弃

---

## 9. 后台重建与并发模型

### 9.1 内部后台队列

插件内部需要维护：

- `rebuild queue`
- `rebuild worker thread`

慢路径触发来源：

- 热路径中的 `shift_confirmed`
- 计划重建
- 手动触发 `RequestRebuild()`

队列颗粒度：

- `T1 / T2`：按 `(task, key)`
- `T3`：同样按 `(task, key)`，但重建使用该 task 的关系分布规格

### 9.2 热路径并发约束

建议约束如下：

- 不同 `key` 可以并发提交
- 同一 `key` 的输入必须按 `bucket_id` 非递减提交
- 同一 `key` 不建议并发调用热路径接口

理由：

- 内部状态按 `key` 维护
- 同一 `key` 的热路径状态更新天然有顺序要求

### 9.3 `shadow baseline`

热路径发现旧基线失配后：

- 先激活 `shadow baseline`
- 立即返回桥接结果
- 同时把正式重建请求入队

`shadow baseline` 的主要职责：

- 在旧正式基线失配后，先维持在线评分稳定
- 避免同步接口卡在历史读取和重训练上

分期实现边界：

- `Story 18.12` 先闭合 `shadow + candidate_validator + formal apply` 主状态机
- 对 `T1`，若 `sigma_ref` 尚未具备，则先沿用业务设计中定义的临时桥接口径；`Story 18.12A` 再补 `sigma_ref` 契约与标准化评分
- 对 `ShadowState`，工程实现不等待全局 `model registry`；先显式记录 `ref_kind / ref_source_key / ref_model_version` 并冻结 `frozen_ref_model`
- 为避免“刚确认漂移就触发重建，但新阶段 replay 长度还不足以完成当前简化 holdout 验证”，`shadow + rebuild` 的实际激活还要满足最小可验证窗口约束；当前实现收口为连续确认点数至少为 3

### 9.4 快照与诊断

诊断接口需要能够回答：

- task 当前是否 ready
- 某个 key 当前是否处于 `shadow`
- 最近一次重建是否成功
- 漂移证据是否已确认
- 正式模型版本与 basis 版本

这些信息不进入热路径返回结构，而是通过：

- `QueryTaskSnapshotJson()`
- `QuerySeriesSnapshotJson()`

对外暴露。

### 9.5 正式训练与 `candidate builder`

慢路径内部对象关系必须进一步收口为：

```text
history_reader
  -> replay_runner
  -> formal_model_trainer
  -> candidate_builder
  -> candidate_validator
  -> formal apply
```

各层职责如下：

1. `replay_runner`

- 从 `history_reader` 取得 typed observation
- 负责时间顺序回放、gap 识别与有效样本筛选
- 不负责求解器训练

2. `formal_model_trainer`

- 消费 replay 后的训练样本
- 调用 `solver_backend`
- 产出最小可服务的正式模型对象

3. `candidate_builder`

- 负责组织训练窗口
- 负责 holdout 尾段切分
- 负责产出 `CandidateBuildResult`

4. `candidate_validator`

- 比较 `candidate` 与 `incumbent`
- 决定是否允许正式切换

这样拆开的目的，是把“历史读取 / 回放 / 训练 / 验证 / 切换”从一坨 `rebuild_worker` 逻辑里拆清楚，避免后续再出现伪实现。

### 9.6 `Baseline Source` 的代码落点

`Baseline Source` 的工程语义不是“另一个输入字段”，而是热路径评分前的一次来源选择。

推荐内部收口为：

```text
BaselineSourceDecision = {
  kind,        // self | configured_source | none
  source_key?
}
```

选择时机：

- 每次热路径评分前
- 仅查询当前 task 内当前可服务的模型状态
- 不调用 `history_reader`

这里的“可服务”直接对应 [design.md] 第 `2.3` 节中的 `Serviceable(source)` 语义，即“当前能够为在线评分提供所需基线输出”，而不是强绑定某一种内部模型状态。

选择规则：

1. 若 `self` 已可服务，直接选 `self`
2. 否则按 `series_overrides[key].baseline_sources[]` 顺序查找第一个可服务来源
3. 若都不可用，则返回 `none`

其中，`Story 18.10` 为保证工程闭环，`可服务` 的临时判定定义为：

- `formal_ready == true`；或
- `candidate_state == trained`，且 `candidate_model` 对当前 bucket 可成功产出预测

这一定义服务于 `18.10` 的来源选择实现，不改变总体设计中“正式模型是长期稳定来源”的原则。待 `18.12` 完成 candidate 验证与正式切换后，来源选择的稳定优先级应收口为“正式模型优先于 candidate 占位模型”。

设计约束：

- 借用来源只影响当前 bucket 的解释基线
- 当前 `key` 的本级状态、训练累计与正式重建必须继续进行
- 不允许跨 task、跨 feature profile、跨粒度借用来源

### 9.7 `EventCalendarSpec` 的代码落点

`EventCalendarSpec` 的关键不在于“如何表达日历”，而在于“如何保证训练与预测语义一致”。

工程上需要 3 个闭环：

1. task 创建时解析并持有 `EventCalendarSpec`
2. 正式训练时把 `calendar_id / calendar_version` 写入 `FormalModelMetadata`
3. 热路径预测时比较“当前 task 持有的日历版本”和“模型记录的日历版本”

若出现以下任一情况：

- task 未配置 `EventCalendarSpec`
- 当前 task 的 `calendar_id / calendar_version` 与模型 metadata 不一致

则：

- 不报热路径错误
- 直接禁用事件块
- 退化为无事件层的同一正式模型 / `shadow baseline`

这条规则必须固化在 predictor 层，而不是散落在各 task 里做条件分支。

当前 `v1` 的补充说明：

- 由于正式模型仍是 `intercept-only`，`EventCalendarSpec` 当前只闭合“配置解析 + metadata 持久化 + 版本检查 + 快照诊断”这条契约链
- 若日历缺失或版本不一致，predictor 仍返回同一模型的基础截距预测，只是把事件块状态标记为禁用
- `QueryTaskSnapshotJson()` 暴露 task 级 `event_calendar_present / event_calendar_id / event_calendar_version / event_calendar_entry_count`
- `QuerySeriesSnapshotJson()` 暴露 task 当前日历、模型 metadata 中的 `calendar_id / calendar_version`，以及 `formal_event_status / candidate_event_status`

---

## 10. 开源依赖与复用策略

### 10.1 基本原则

本插件涉及大量计算和数据结构，必须坚持：

- 尽量复用成熟开源代码
- 不为通用问题重复造轮子
- 依赖选择优先考虑：
  - 性能
  - 许可证兼容性
  - 与现有仓库的一致性
  - 封装边界清晰

### 10.2 已有依赖优先复用

从当前仓库看，以下能力已有明确基础：

- JSON：已广泛使用 `RapidJSON`
- 并发原语：已有 `std::thread`、`std::mutex`、`std::condition_variable`
- 数据通道：已有 Arrow 相关依赖，但本插件热路径不应直接绑定 Arrow 接口

因此：

- 配置解析与诊断输出继续复用 `RapidJSON`
- 慢路径后台线程与队列，`v1` 可先基于标准库并发原语实现

### 10.3 数值计算层

对求解器、线性代数和鲁棒拟合层，建议采用“可替换 backend”设计：

- 对外不暴露具体数学库类型
- 对内通过 `solver_backend` 封装

选择原则：

1. 优先复用成熟的 dense linear algebra 开源库
2. 不手写通用矩阵分解、线性求解等基础能力
3. 若 `v1` 暂未引入新依赖，则自实现部分必须严格限制在“小规模、封装内、可替换”的薄层包装，避免演化成自制数学框架

本条是实现约束，不代表当前文档已绑定某个具体数学库。

### 10.3.1 `v1` 的正式依赖决策

结合当前仓库现状与 `design.md` 中统一块求解器的要求，`v1` 的正式决策收口如下：

- `v1` 新增且仅新增 `1` 个数值计算依赖：`Eigen 3`
- 用途严格限定在慢路径的 `solver backend`
- 主要服务对象是 `WeightedHuberRidgeBlockSolver`
- 使用范围限定为：
  - `T1 / T2` 的离线训练
  - 正式重建
  - candidate 验证中的求解过程

不允许的用法：

- 不允许把 `Eigen` 暴露到公共接口头文件
- 不允许让热路径 `SubmitObservation()` / `SubmitBlock()` 直接依赖 `Eigen`
- 不允许把 `Eigen` 作为通用容器层扩散到 `task / model / service` 普通对象中

采用 `Eigen 3` 的原因：

1. `design.md` 已明确 `v1` 统一块求解器需要 `weighted ridge + IRLS`，并要求 `Cholesky` 失败时可回退 `QR`
2. 这类问题属于标准的小规模稠密线性代数，不应手写矩阵分解与线性求解
3. 当前仓库尚无现成的 dense linear algebra 库；若引入新库，`Eigen 3` 在接入成本、性能和封装灵活性之间最均衡
4. `Eigen 3` 为头文件库，适合按当前仓库的 `thirdparts/*-config.cmake` 方式纳入

### 10.3.2 `solver backend` 的封装约束

虽然 `v1` 决定引入 `Eigen 3`，但工程上仍必须保持“算法层依赖可替换”：

- 对外不出现 `Eigen` 类型
- 对内通过 `solver_backend.h/.cpp` 一类内部封装统一提供：
  - 设计矩阵装载
  - `weighted ridge` 子问题求解
  - `Cholesky -> QR` 回退
  - 数值稳定性与失败码映射

这样做的目的不是为了抽象而抽象，而是为了保证：

- 将来若要替换为其他数学库，不需要改公共接口
- 热路径与慢路径边界不会被数学库类型污染
- 求解器行为、回退逻辑和错误语义可以在同一封装层集中维护

### 10.3.3 明确不纳入 `v1` 的新增依赖

以下依赖在 `v1` 中明确不纳入：

- 不引入 `Boost`
- 不引入 `TBB / folly / absl`
- 不引入专门的 lock-free queue 库
- 不引入额外的时序预测框架，如 `Prophet`、`statsmodels` 一类方案
- 不在基线层引入 `sketch / tdigest / KLL / Count-Min` 一类摘要数据结构库

原因统一收口为：

- 这些依赖对当前 `baseline v1` 的主路径收益不足
- 会显著增加构建、封装和维护复杂度
- 其中不少能力已在设计上明确不属于基线层职责，尤其是 `sketch`

### 10.3.4 `Eigen 3` 的 `CMake` 接入约束

虽然 `Eigen 3` 是纯头文件库，但在当前仓库中仍应通过 `thirdparts/*-config.cmake` 统一接入，而不是在某个插件的 `CMakeLists.txt` 中手工硬编码头文件路径。

正式约束如下：

- 新增文件：`thirdparts/eigen/eigen-config.cmake`
- 依赖名固定为：`eigen`
- 继续复用现有：
  - `add_thirdparts()`
  - `add_thirddepen(target eigen)`
- `eigen-config.cmake` 只负责：
  - 查找系统安装
  - 查找缓存安装目录
  - 必要时自动下载并安装头文件
  - 导出统一的 include 路径变量
- `eigen-config.cmake` 不负责：
  - 生成动态库或静态库
  - 导出链接目录
  - 导出链接参数

变量契约固定为：

- `eigen_LINK_INC`：必须指向可直接 `#include <Eigen/Core>` 的根目录，因此应为 `.../include/eigen3`
- `eigen_LINK_DIR`：固定为空字符串
- `eigen_LINK_TAR`：固定为空

查找顺序固定为：

1. 系统安装：优先检查 `/usr/include/eigen3`、`/usr/local/include/eigen3`
2. 缓存安装：`${THIRDPARTS_INSTALL_DIR}/eigen/include/eigen3`
3. 自动下载：使用 `ExternalProject_Add` 拉取 `Eigen 3` 源码，并安装到缓存目录

这样设计的目的有 3 个：

1. 让头文件库与现有 `RapidJSON` 等依赖保持统一接入方式
2. 避免 `baseline` 插件私自管理第三方头文件路径，破坏工程一致性
3. 为后续其他模块复用 `Eigen 3` 预留统一入口

工程提醒：

- `Eigen 3` 只允许在 `baseline` 插件内部的 `solver backend` 使用
- 即便依赖在全仓统一注册，也不意味着可以在其他普通模块中随意扩散使用
- 若后续出现新的数学依赖，也应优先沿用这一统一接入模式，而不是在单个模块里临时拼接
- `git worktree` 开发必须复用主工作区的第三方缓存；`THIRDPARTS_INSTALL_DIR` / `THIRDPARTS_PREFIX_DIR` 应统一解析到 Git 主工作区根目录，而不是当前 worktree 根目录
- `Eigen 3` 作为本轮新增依赖，应先在主工作区完成一次缓存安装，再进入 `.worktrees/` 分支开发；后续新增其他第三方依赖也遵循同一规则
- 如果主工作区缓存尚未预热，worktree 侧不得首次拉起 `ExternalProject` 下载 / 编译；应回到主工作区补齐缓存后再继续

### 10.4 数据结构层

对高频容器和索引结构，原则如下：

- 优先复用标准库和成熟容器
- 热点容器通过统一别名 / wrapper 封装，便于后续替换
- 不在业务层散落自制 hash table、queue、top-k 容器

具体含义：

- `unordered_map`、`vector`、`priority_queue` 等可作为首版实现基础
- 若性能 profiling 证明瓶颈明显，再在封装层替换为更高性能开源容器

### 10.5 基线算法代码注释原则

涉及基线算法的代码，必须提供清晰注释。

这里的“基线算法代码”包括但不限于：

- `T1 / T2 / T3` 的核心评分逻辑
- `shadow baseline` 与漂移证据累积器
- 正式重建、候选验证与切换逻辑
- `T3` 的摘要提取、basis / support / stable head 相关逻辑
- 求解器封装、状态机转换和关键阈值映射

注释要求：

1. 注释优先解释“为什么这样做”，而不是逐字复述代码字面含义。
2. 当代码对应 `design.md` 或本文中的某个算法概念时，注释应明确指出该概念名称，便于实现与设计对照。
3. 对状态迁移、重建触发、`shadow baseline` 接管 / 退出、`T3` basis 切换等非直观逻辑，必须在代码块前给出短而明确的说明。
4. 对输入字段、局部变量和中间量，若名字不足以自解释，必须补充注释说明其数学或工程语义。
5. 注释应保持克制，避免把简单赋值、循环和显然可读的控制流机械翻译成自然语言。

工程含义：

- 基线插件不是普通 CRUD 代码，它包含大量时序、状态、重建和分布建模逻辑。
- 若缺少清晰注释，后续调参与故障排查成本会显著上升。
- 因此，“涉及基线算法的代码要给清晰注释”是本插件实现阶段的正式原则，不是可选建议。

---

## 11. 目录与文件建议

建议新增目录：

```text
src/plugins/baseline/
```

建议的首版文件分层：

```text
src/plugins/baseline/
  baseline_plugin.h/.cpp
  ibaseline_internal.h
  config_parser.h/.cpp
  task/
    baseline_task_base.h/.cpp
    value_task.h/.cpp
    ratio_task.h/.cpp
    relation_task.h/.cpp
  model/
    task_spec.h
    series_override.h
    event_calendar_spec.h
    series_state.h
    formal_model_state.h
    formal_model.h
    formal_predictor.h/.cpp
    shadow_state.h
    drift_state.h
  rebuild/
    rebuild_request.h
    rebuild_queue.h/.cpp
    rebuild_worker.h/.cpp
    replay_runner.h/.cpp
    formal_model_trainer.h/.cpp
    candidate_builder.h/.cpp
    candidate_validator.h/.cpp
  detector/
    detector_common.h
    value_detector_core.h/.cpp
    ratio_detector_core.h/.cpp
  relation/
    relation_basis.h/.cpp
    relation_summary_extractor.h/.cpp
    relation_router.h/.cpp
```

对应新增公共接口头文件：

```text
src/framework/interfaces/
  ibaseline_service.h
  ibaseline_types.h
```

原则：

- 公共接口只放 `framework/interfaces`
- 插件内部实现细节全部留在 `src/plugins/baseline`

---

## 12. 实现拆分建议

建议按以下顺序落地：

### 12.1 Phase 1：公共接口与插件骨架

完成内容：

- `IID_BASELINE_SERVICE`
- `IBaselineService`
- `IBaselineTask`
- `IBaselineValueTask`
- `IBaselineRatioTask`
- `IBaselineRelationTask`
- `BaselinePlugin` 注册骨架

目标：

- 先让插件可被加载、可被查询、可创建空 task

### 12.2 Phase 2：Task 配置与生命周期

完成内容：

- `Create*Task(config_json, ...)`
- task 元信息
- `Close()`
- task 注册表

目标：

- 先把 task 对象生命周期与最小配置解析闭合
- `Baseline Source / EventCalendarSpec` 的低频静态配置解析延后到 Phase 6

### 12.3 Phase 3：`T1 / T2` 热路径

完成内容：

- `ValueObservation`
- `RatioObservation`
- 基础 `DetectorResult`
- 每 key 状态管理

目标：

- 先跑通标量热路径

### 12.4 Phase 4：慢路径基础设施与 replay

完成内容：

- `history_reader`
- rebuild queue
- rebuild worker
- replay runner

目标：

- 先让同步接口与异步正式重建解耦，并把历史回放链路打通

### 12.5 Phase 5：predictor、正式训练与 `candidate builder`

完成内容：

- `formal_model`
- `formal_predictor`
- `formal_model_trainer`
- `candidate_builder`

目标：

- 让慢路径真正产出可服务的 `candidate model`

### 12.6 Phase 6：`Baseline Source` 与 `EventCalendarSpec` 契约

完成内容：

- `Baseline Source` 选择
- `calendar_id / calendar_version` 比对
- predictor 层自动禁用事件块

目标：

- 把低频静态配置和热路径评分之间的契约闭合

### 12.7 Phase 7：`shadow baseline` 与正式切换

完成内容：

- `shadow baseline`
- `candidate_validator`
- 正式切换

目标：

- 闭合旧基线失配后的桥接与正式切换

### 12.7A Phase 7A：`T1` 正式尺度层与 `ShadowState` 引用模型收口

完成内容：

- `sigma_ref` 训练 / 持久化 / predictor 契约
- `T1` 标准化残差评分
- `ShadowState` 显式冻结引用模型结构（`ref_kind / ref_source_key / ref_model_version / frozen_ref_model`）
- `QuerySeriesSnapshotJson()` 暴露 `formal_predict_sigma_ref / candidate_predict_sigma_ref`

目标：

- 收口 `18.12` 的两个过渡实现点，避免 `shadow` 逻辑长期停留在临时口径

### 12.8 Phase 8：关系分布 task（算法类型 `T3`）

完成内容：

- `RelationObservationBlock`
- basis / support / stable head 内部对象
- 摘要提取与 routed detector core 接线

目标：

- 落地“算法类型 `T3` = 代码侧一个 `relation task` = 一个关系分布规格”的实现框架

### 12.9 Phase 9：诊断、压测与稳定性补强

完成内容：

- snapshot / diagnostics
- 并发约束验证
- 高频 key 场景压测
- 重建失败与恢复路径测试

---

## 13. 本文结论

统一基线插件的代码设计在 `v1` 上正式收口为：

1. 采用插件形态，而不是算子形态。
2. 插件命名为 `baseline`。
3. 主接口采用 `IBaselineService`。
4. 任务对象采用 `IBaselineTask`，不采用泛化 `IHandle`。
5. 热路径数据用强类型结构体，配置 / 诊断用 JSON。
6. 对外同步，对内双路径：同步评分 + 异步正式重建。
7. `T1 / T2` 的 task 颗粒度是标量特征规格。
8. 算法类型 `T3` 在代码侧的 task 颗粒度是关系分布规格。
9. `history_reader` 只能进入慢路径。
10. `BaselineSourceConfig` 与 `EventCalendarSpec` 都通过低频静态配置注入，不进入热路径输入。
11. 慢路径正式训练对象链路固定为 `replay_runner -> formal_model_trainer -> candidate_builder -> candidate_validator`。
12. 实现阶段必须优先复用成熟开源能力，避免为通用问题重复造轮子。
