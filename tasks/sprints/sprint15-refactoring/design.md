# Sprint 15 Refactoring 设计方案（可编码版，R1-R5）

## 1. 文档信息

- 文档版本：v2（用于直接指导编码）
- 适用迭代：Sprint 15 Refactoring
- 更新时间：2026-04-07
- 对应计划文档：`tasks/sprints/sprint15-refactoring/planning.md`

---

## 2. 目标与边界

## 2.1 目标

在不改变外部行为的前提下，完成以下 5 个重构项并可直接落地编码：

1. R1：拆分 `SchedulerPlugin` 巨文件
2. R2：抽离 `TaskPlugin` 的 SQLite 存储层
3. R3：收敛 stream 执行前校验为执行计划对象
4. R4：统一 Scheduler 控制调用客户端（生产与测试共用）
5. R5：错误码与响应契约类型化

## 2.2 不变约束（必须遵守）

1. 对外 URI 不变化。
2. 请求/响应 JSON 字段不变化。
3. 任务状态语义不变化（`pending/running/completed/failed/stopped/cancelled/timeout`）。
4. 已有测试必须保持通过。

## 2.3 非目标

1. 不引入新功能语义。
2. 不做兼容双轨（无 v1/v2 并行实现）。
3. 不改前端业务流程。

---

## 3. 实施顺序与里程碑

按风险从低到高实施：

1. R4
2. R5
3. R1
4. R2
5. R3

每项完成后必须通过门禁：

1. `cmake --build build -j4`
2. `./build/output/test_framework`
3. `./build/output/test_task`
4. `./build/output/test_scheduler_mutation_guard`
5. `./build/output/test_scheduler_e2e`

---

## 4. R1 详细设计：拆分 `SchedulerPlugin` 巨文件

## 4.1 当前问题

文件 [scheduler_plugin.cpp](/mnt/d/working/flowSQL/src/services/scheduler/scheduler_plugin.cpp) 同时承载：

1. 路由入口
2. stream 执行流程
3. 通道管理与查找
4. 运行态 retention
5. 租约与并发保护

单文件维护成本高，冲突概率高。

## 4.2 目标文件布局

新增文件：

1. `src/services/scheduler/scheduler_routes.cpp`
2. `src/services/scheduler/scheduler_stream_executor.cpp`
3. `src/services/scheduler/scheduler_channel_admin.cpp`
4. `src/services/scheduler/scheduler_runtime_retention.cpp`

保留文件：

1. `src/services/scheduler/scheduler_plugin.cpp`（只保留生命周期、控制服务入口和公共装配）
2. `src/services/scheduler/scheduler_plugin.h`（类声明集中）

## 4.3 函数迁移映射（必须按表执行）

迁移到 `scheduler_routes.cpp`：

1. `EnumRoutes`
2. `HandleSqlClassify`
3. `HandleStreamExecuteSingle`
4. `HandleStreamExecute`
5. `HandleStreamStop`
6. `HandleStreamStatus`
7. `HandleStreamList`
8. `HandleExecute`
9. `HandleGetChannels`
10. `HandleQueryStreamChannelDefinitions`
11. `HandleQueryStreamChannels`
12. `HandleAddStreamChannel`
13. `HandleModifyStreamChannel`
14. `HandleRemoveStreamChannel`
15. `HandlePreviewDataframe`
16. `HandleRefreshOperators`

迁移到 `scheduler_stream_executor.cpp`：

1. `ResolveSourceBindings`
2. `ResolveStreamSink`
3. `ClassifySqlTaskKind`
4. `ExecuteStreamTask`
5. `ExecuteTransfer`
6. `ExecuteWithOperator`
7. `ExecuteWithOperatorChain`
8. `FindOperator`
9. `CreateOperator`
10. `QueryStreamChannelRole`

迁移到 `scheduler_channel_admin.cpp`：

1. `RegisterManagedChannel`
2. `EraseManagedChannel`
3. `ClearManagedChannels`
4. `FindManagedChannelShared`
5. `SnapshotManagedChannels`
6. `FindChannel`
7. `RegisterChannel`
8. `TryBeginStreamChannelMutation`
9. `EndStreamChannelMutation`
10. `TryAcquireStreamTaskLeases`
11. `CaptureStreamChannelVersionSnapshot`

迁移到 `scheduler_runtime_retention.cpp`：

1. `ReleaseStreamTaskLeases`
2. `SweepFinishedTaskLeases`
3. `MarkRuntimeTerminal`
4. `TouchRuntimeAccess`
5. `SweepRuntimeRetainedObjects`
6. `QueryStreamTaskSnapshotByRuntimeId`
7. `RequestStopStreamTaskByRuntimeId`
8. `QueryGroupShareSetSnapshots`
9. `QueryGroupNodeResolvedSources`
10. `CleanupGroupRuntimeResources`

## 4.4 编码步骤

第 1 步：创建新 `.cpp` 文件并仅“搬迁函数定义”，不改逻辑。  
第 2 步：修复静态 helper 的可见性（需要跨文件复用的 helper 下沉到 `scheduler_internal_utils.h/.cpp`）。  
第 3 步：保持 `SchedulerPlugin` 头文件声明不变。  
第 4 步：编译与回归测试。  
第 5 步：删除旧文件中的重复定义。

## 4.5 风险与规避

1. 风险：同名静态函数在多 TU 下行为漂移。  
规避：抽公共 helper，禁止复制粘贴。

2. 风险：链接失败（遗漏成员函数定义）。  
规避：迁移按映射表逐项勾销。

## 4.6 验收

1. `scheduler_plugin.cpp` 行数显著下降。
2. 路由行为一致（`test_scheduler_e2e` 通过）。
3. 并发与租约守护测试通过（`test_scheduler_mutation_guard`）。

---

## 5. R2 详细设计：抽离 `TaskPlugin` 的 SQLite 存储层

## 5.1 当前问题

[task_plugin.cpp](/mnt/d/working/flowSQL/src/services/task/task_plugin.cpp) 同时包含：

1. 路由解析
2. 任务编排
3. SQLite 细节实现
4. retention 清理

导致存储细节与业务编排耦合过深。

## 5.2 新模块设计

新增：

1. `src/services/task/task_store_sqlite.h`
2. `src/services/task/task_store_sqlite.cpp`

建议类：

```cpp
class TaskStoreSqlite {
 public:
    struct OpenOptions {
        std::string db_dir;
        std::string db_path;
        int retention_days = 7;
        int retention_max_count = 1000;
    };

    int Open(const OpenOptions& options);
    int Close();
    int CleanupOrphans();

    int CreateTask(const TaskCreateParams& params, std::string* task_id_out);
    int UpdateStatus(const TaskStatusUpdate& update);
    int GetTask(const std::string& task_id, TaskRecord* out);
    int ListTasks(const TaskListQuery& query, std::vector<TaskRecord>* items, int64_t* total);
    int DeleteTask(const std::string& task_id);
    int WriteDiagnostic(const TaskDiagnosticRecord& record);
    int WriteTaskEvent(const TaskEventRecord& record);
    int RunRetentionCleanup();

 private:
    sqlite3* db_ = nullptr;
    std::mutex db_mu_;
};
```

> 说明：参数结构体可放在 `task_store_sqlite.h`，避免长参数列表。

## 5.3 `TaskPlugin` 改造边界

`TaskPlugin` 保留：

1. 路由入口（`Handle*`）
2. worker/timeout 线程与队列
3. 调度调用（Scheduler 控制接口）
4. 任务状态机驱动

`TaskPlugin` 删除或下沉：

1. `EnsureDb/EnsureSchema/CleanupOrphans`
2. `GetTaskNoLock/DeleteTaskNoLock/UpdateStatusNoLock`
3. 直接 `sqlite3_*` 语句构造与执行

## 5.4 迁移步骤

第 1 步：在 `TaskStoreSqlite` 复制现有 DB 逻辑，保持 SQL 语义一致。  
第 2 步：`TaskPlugin` 新增 `std::unique_ptr<TaskStoreSqlite> store_` 并切换调用。  
第 3 步：删除 `TaskPlugin` 内重复 DB 逻辑。  
第 4 步：保留 `TaskPlugin` 现有对外行为，验证回归。

## 5.5 并发规则

1. `db_mu_` 仅在 store 内管理。  
2. `TaskPlugin::mu_` 与 store 的 `db_mu_` 不交叉持有。  
3. 线程退出时先停 worker/timeout，再关闭 store。

## 5.6 验收

1. `test_task` 全通过。  
2. `cancel/delete/update/retention` 行为一致。  
3. `task_plugin.cpp` 中 `sqlite3_` 直接调用显著减少。

---

## 6. R3 详细设计：执行计划对象收敛

## 6.1 当前问题

`ExecuteStreamTask` 中存在多段重复的：

1. source/sink 解析
2. lease 申请与失败处理
3. capability 校验
4. 早返回清理

逻辑分散导致 TOCTOU 与回滚遗漏风险增加。

## 6.2 新对象

新增（建议）：

1. `src/services/scheduler/stream_execution_plan.h`
2. `src/services/scheduler/stream_execution_plan.cpp`

核心结构：

```cpp
struct StreamExecutionPlan {
    std::string runtime_task_id;
    SqlStatement stmt;
    scheduler::SchedulerPlugin::SourceResolveResult source;
    scheduler::SchedulerPlugin::SinkBinding sink;
    std::vector<std::string> source_keys;
    std::vector<std::string> sink_keys;
    std::unordered_map<std::string, uint64_t> version_snapshot;
    ParallelStrategy strategy = ParallelStrategy::NONE;
    int parallelism = 1;
    StreamChannelCapabilities source_caps;
    StreamChannelCapabilities sink_caps;
};

class LeaseToken {
 public:
    LeaseToken() = default;
    LeaseToken(scheduler::SchedulerPlugin* owner, std::string runtime_task_id);
    ~LeaseToken();
    void Commit();    // 提交后不自动释放
    void Reset();     // 手动释放
 private:
    scheduler::SchedulerPlugin* owner_ = nullptr;
    std::string runtime_task_id_;
    bool committed_ = false;
};
```

## 6.3 执行流程（统一管线）

1. `BuildPlan`：解析 source/sink + 计算 strategy/parallelism。  
2. `ValidatePlan`：能力校验（source/sink/fanin/fanout）。  
3. `AcquireLease`：版本快照校验 + 租约申请，返回 `LeaseToken`。  
4. `InstantiateTask`：构建 `StreamTask` 与 shard。  
5. `Schedule`：写入 `stream_tasks_` 并调度。  
6. `LeaseToken::Commit`：任务提交成功后交由运行态释放。

## 6.4 代码改造点

1. `ExecuteStreamTask` 仅保留编排，移除大段细节分支。  
2. 现有 `ResolveSourceBindings/ResolveStreamSink` 复用到 `BuildPlan`。  
3. 现有 `TryAcquireStreamTaskLeases` 复用到 `AcquireLease`。

## 6.5 并发与锁序

固定锁序：

1. 无锁阶段完成 SQL 解析与候选计划
2. `stream_channel_refs_mu_`（租约）
3. `stream_tasks_mu_`（任务注册）

禁止反向持锁。

## 6.6 验收

1. `ExecuteStreamTask` 复杂度显著下降。  
2. `test_scheduler_mutation_guard` 中 TOCTOU/lease 用例通过。  
3. 异常路径无租约泄漏。

---

## 7. R4 详细设计：统一 Scheduler 控制调用客户端

## 7.1 目标

让生产代码和测试代码使用同一套 Scheduler 控制调用封装，避免重复维护。

## 7.2 新模块

新增（建议）：

1. `src/framework/core/scheduler_control_client.h`
2. `src/framework/core/scheduler_control_client.cpp`

接口草案：

```cpp
class SchedulerControlClient {
 public:
    explicit SchedulerControlClient(IQuerier* querier = nullptr);
    void ResetQuerier(IQuerier* querier);

    int32_t ClassifySql(const std::string& req, std::string* rsp);
    int32_t ExecuteBatch(const std::string& req, std::string* rsp);
    int32_t ExecuteStream(const std::string& req, std::string* rsp);
    int32_t StopStream(const std::string& req, std::string* rsp);
    int32_t QueryStreamStatus(const std::string& req, std::string* rsp);

 private:
    ISchedulerControlService* Acquire(std::string* err_rsp);
    IQuerier* querier_ = nullptr;
};
```

## 7.3 应用改造

`TaskPlugin`：

1. 删除 `GetSchedulerControlService` 与 5 个 `Scheduler*` 包装函数。  
2. 新增成员 `SchedulerControlClient scheduler_client_;`。  
3. 在 `Load()` 中 `scheduler_client_.ResetQuerier(querier_)`。  
4. 所有调度调用改为 `scheduler_client_.*`。

`test_task`：

1. 复用 `SchedulerControlClient` 进行控制调用。  
2. 将本地 `RouterBackedSchedulerControlService::Dispatch` 逻辑收敛到公共测试适配器（避免重复 URI 分发表）。

## 7.4 验收

1. 生产与测试共用调用封装。  
2. `task_plugin.cpp` 内不再出现重复控制调用模板。  
3. `test_task` 通过。

---

## 8. R5 详细设计：错误码与响应契约类型化

## 8.1 当前问题

`error_code/error_stage` 大量字符串散落在：

1. `scheduler_plugin.cpp`
2. `scheduler_stream_group.cpp`
3. `task_plugin.cpp`

存在拼写漂移风险与口径不一致风险。

## 8.2 新模块

新增：

1. `src/framework/core/error_contract.h`
2. `src/framework/core/error_contract.cpp`

接口草案：

```cpp
enum class ErrorCodeId {
    kUnknown = 0,
    kStreamChannelMutating,
    kStreamSourceInUse,
    kStreamChannelVersionChanged,
    kStreamGroupTimeout,
    kBatchSqlTextInvalid,
    kStreamSqlUseStreamApi,
};

enum class ErrorStageId {
    kUnknown = 0,
    kParse,
    kSourceResolve,
    kSinkResolve,
    kLease,
    kExecute,
    kStatus,
    kStop,
};

const char* ToErrorCode(ErrorCodeId id);
const char* ToErrorStage(ErrorStageId id);
```

## 8.3 `json_error_builder` 扩展

在保持旧接口可用的前提下增加强类型重载：

```cpp
std::string BuildExecutionErrorJson(const std::string& error,
                                    ErrorCodeId code,
                                    ErrorStageId stage);
```

旧字符串版本暂保留，迁移完成后禁止新增字符串直写。

## 8.4 迁移策略

第 1 步：新增 `error_contract` 与 builder 重载。  
第 2 步：先迁移 `scheduler_stream_group.cpp`，再迁移 `scheduler_plugin.cpp`，最后迁移 `task_plugin.cpp`。  
第 3 步：关键路径完成后补契约测试。  
第 4 步：清理重复字符串常量。

## 8.5 验收

1. 执行错误统一包含 `error/error_code/error_stage`。  
2. 关键错误码稳定。  
3. 契约测试通过。

---

## 9. 测试设计（可直接执行）

## 9.1 测试文件改动

新增或扩展：

1. `src/tests/test_framework/main.cpp`  
2. `src/tests/test_task/test_task.cpp`  
3. `src/tests/test_scheduler_e2e/test_scheduler_mutation_guard.cpp`

可选新增（推荐）：

1. `src/tests/test_framework/test_scheduler_control_client.cpp`
2. `src/tests/test_framework/test_error_contract.cpp`
3. `src/tests/test_scheduler_e2e/test_stream_execution_plan.cpp`

## 9.2 用例矩阵

R4 用例：

1. Querier 缺失 `ISchedulerControlService` 返回 `UNAVAILABLE` 与标准错误 JSON。  
2. 各控制方法透传返回值与返回体不变。

R5 用例：

1. `BuildExecutionErrorJson` 类型化重载输出正确 `error_code/error_stage`。  
2. 旧接口与新接口输出字段兼容。

R1 用例：

1. 路由枚举数量与 URI 集合不变。  
2. 执行路径 e2e 结果不变。

R2 用例：

1. 并发 `cancel/delete/status` 一致性。  
2. retention 按 `days/max_count` 生效。  
3. 进程重启孤儿任务清理保持一致。

R3 用例：

1. 计划构建失败时租约不残留。  
2. 版本冲突场景返回 `STREAM_CHANNEL_VERSION_CHANGED`。  
3. sink/source 能力不匹配错误字段完整。

---

## 10. 开发任务清单（可直接拆分）

1. R4-1：落地 `SchedulerControlClient`，接入 `TaskPlugin`。  
2. R4-2：测试侧调用封装收敛。  
3. R5-1：落地 `error_contract` 与 `json_error_builder` 重载。  
4. R5-2：迁移 `scheduler_stream_group` 错误码。  
5. R5-3：迁移 `scheduler_plugin` 与 `task_plugin` 错误码。  
6. R1-1：拆分 `routes`。  
7. R1-2：拆分 `channel_admin`。  
8. R1-3：拆分 `runtime_retention`。  
9. R1-4：拆分 `stream_executor`。  
10. R2-1：落地 `TaskStoreSqlite` 与参数结构体。  
11. R2-2：`TaskPlugin` 切换到 store。  
12. R2-3：清理遗留 `sqlite3_*` 直接调用。  
13. R3-1：落地 `StreamExecutionPlan` 与 `LeaseToken`。  
14. R3-2：`ExecuteStreamTask` 切编排模式。  
15. R3-3：补充 TOCTOU 与泄漏回归测试。

---

## 11. 完成定义（Definition of Done）

1. R1-R5 全部完成并在 planning 文档中勾选。  
2. 所有门禁测试通过。  
3. 不引入新 URI，不改变外部行为。  
4. 新增模块有对应单测或契约测试。  
5. 代码可读性提升有量化结果（核心巨文件行数下降、重复代码下降）。
