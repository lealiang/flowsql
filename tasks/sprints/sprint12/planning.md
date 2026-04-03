# Sprint 12 规划

## Sprint 信息

- **Sprint 周期**：Sprint 12
- **开始日期**：2026-04-01
- **预计工作量**：8 天
- **Sprint 目标**：交付流式处理框架基础设施（路径 A），形成可执行、可测试、可观测的最小闭环。

---

## 迭代边界（冻结）

### 本迭代范围（In Scope）

1. **Story 14.0**：`StreamPlugin` 流式通道生命周期管理（`IStreamFactory` 注册与查找）
2. **Story 14.1**：`IStreamChannel`（路径 A）+ `RingStreamChannel` + `FanIn/FanOut`
3. **Story 14.2**：`IStreamOperator`（路径 A）+ 流式算子插件导出能力
4. **Story 14.3**：`StreamTask/ShardRunner/StreamRuntime` 线程池调度 + Scheduler 流式端点
5. **测试**：完成 `test_stream` 的 20 个用例（`T1~T20`）
6. **补充任务**：Web 端最小流式可用性（流式通道只读展示 + 示例 SQL 一键填充）

### 非本迭代范围（Out of Scope）

1. 多主机分布式编排（Orchestrator/Host/Executor）
2. 路径 B 接口占位（`IBlockStreamChannel/IBlockStreamOperator` 头文件）延后到下个迭代
3. 路径 B 完整实现（`IBlockStreamChannel/IBlockStreamOperator` 数据面）
4. Story 14.5（DPDK 网卡采集插件）
5. Story 14.6（NPM 网络性能分析算子）
6. 跨进程流通道、TaskPlugin 统一入口
7. 完整 Web UI 流式管理页面（流式通道增删改、流式任务生命周期可视化）

### 关键约束

1. 保持现有 6 态调度状态机（`kIdle/kQueued/kRunning/kRunningPending/kWaitingRetry/kDone`）
2. 暂不引入“输入可读回调”，空闲唤醒继续使用 `kWaitingRetry + timer` 兜底
3. `AtomicRing` 本迭代仅实现 `SPSC/SPMC`；`MPSC/MPMC` 配置返回 `ENOTSUP`

---

## Sprint 目标与成功标准

### 主要目标

1. 打通 `SELECT ... FROM stream_channel USING stream_op INTO stream/dataframe/<db_type>.<db_name>[.<table>]` 的流式执行链路
2. 在同一调度内核下支持三类执行场景：`NONE/STATELESS/KEYED`
3. 提供流式任务管理端点：`execute/stop/status/list`
4. 提供 Web 端最小流式入口：可发现流式通道，并快速填充演示 SQL

### 成功标准

- [x] 流式任务可非阻塞启动，并返回 `stream_task_id`
- [x] `RequestStop + Join` 可稳定收敛，不出现悬挂执行线程
- [x] `TaskSnapshot` 指标、错误与状态聚合一致
- [x] `STATELESS/SPMC` 按 `producer_finished && queue_empty` 正确收敛
- [x] `INTO` 绑定改为“真实通道透传”：Scheduler 不再隐式统一为 `IStreamChannel` 输出
- [x] `sink_type != stream` 的执行路径默认单写者降级（`parallelism=1`），`sink_type == stream` 保持算子声明并行度
- [x] 框架层移除 `WITH sink_table` 语义；表名仅来自 `INTO <db_type>.<db_name>.<table>` 或由普通算子自行处理两段式数据库目标
- [x] `builtin.*` 在 `INTO <db_type>.<db_name>`（两段式）时返回明确错误，要求三段式显式表名
- [x] 目录归一完成：`passthrough/concat/hstack` 收敛到 `src/framework/builtin/dataframe/`，且注册路径完成同步
- [x] `test_stream (T1~T20)` 全部通过
- [x] Web 页面支持流式通道只读展示，并可一键填充 `tcp_session_mock + builtin.tcp_service_merge_stream` 示例 SQL
- [x] 相关文档与配置同步更新（`design.md` / `product_backlog.md`）

---

## Story 列表

### Story 14.0：StreamPlugin（前置）

**优先级**：P0  
**工作量估算**：1 天  
**依赖**：无

**验收标准**：
- [x] 新增 `IStreamFactory` 接口，支持按 `type.name` 查询流式通道
- [x] 新增 `libflowsql_stream.so`，完成 `stream_channels` 配置解析与通道注册
- [x] Scheduler `FindChannel()` 支持流式分支查找

**任务分解**：
- [x] T1：新增 `src/framework/interfaces/istream_factory.h`
- [x] T2：实现 `src/services/stream/stream_plugin.h/cpp`
- [x] T3：新增 `src/services/stream/plugin_register.cpp` 与 CMake 接入
- [x] T4：修改部署配置，加载 `libflowsql_stream.so`

---

### Story 14.1：IStreamChannel（路径 A）

**优先级**：P0  
**工作量估算**：2 天  
**依赖**：Story 14.0

**验收标准**：
- [x] 定义 `StreamBatch/PollEvent/IStreamChannel` 契约
- [x] 完成 `RingStreamChannel`（`Open/Put/PollNext/Cancel/Close`）
- [x] 完成 `FanInStreamChannel` 与 `FanOutStreamChannel`（`ROUND_ROBIN` + `ROUTE_BY_PARTITION_ID`）
- [x] `ring_mode` 的 `mpsc/mpmc` 返回 `ENOTSUP`

**任务分解**：
- [x] T5：新增 `src/framework/interfaces/istream_channel.h`
- [x] T6：实现 `src/framework/core/ring_stream_channel.h/cpp`
- [x] T7：实现 `src/framework/core/fan_in_stream_channel.h/cpp`
- [x] T8：实现 `src/framework/core/fan_out_stream_channel.h/cpp`
- [x] T9：修改 `src/framework/interfaces/ichannel.h`（`kStream/kBlockStream`）

---

### Story 14.2：IStreamOperator（路径 A）

**优先级**：P0  
**工作量估算**：2 天  
**依赖**：Story 14.1

**验收标准**：
- [x] 定义 `IStreamOperator` 生命周期（`Configure/Init/OnSchemaReady/Process/Tick/Flush`）
- [x] `BinAddonHostPlugin` 支持 `flowsql_stream_operator_*` 导出符号与 ABI 校验
- [x] 提供内置示例流式算子用于链路验证
- [x] 内置算子目录规范统一：DataFrame 内置算子在 `src/framework/builtin/dataframe/`，流式内置算子在 `src/framework/builtin/stream/`

**任务分解**：
- [x] T10：新增 `src/framework/interfaces/istream_operator.h`
- [x] T11：新增 `src/framework/builtin/stream/passthrough_stream_operator.h/cpp` 与 `count_window_stream_operator.h/cpp`，并在 `catalog_plugin` 注册
- [x] T11.1（强制）：将现有 `passthrough/concat/hstack` 归一到 `src/framework/builtin/dataframe/`，`catalog_plugin` 调整 include 与注册路径（不改行为）
- [x] T11.2（强制）：更新 `src/framework/CMakeLists.txt`，确保 `flowsql_common` 编译 `framework/builtin/dataframe/*` 与 `framework/builtin/stream/*`；避免在 `flowsql_catalog` 重复编译这些实现源码
- [x] T12：扩展 `src/services/binaddon/binaddon_host_plugin.h/cpp`

---

### Story 14.3：StreamRuntime + Scheduler 流式调度

**优先级**：P0  
**工作量估算**：3 天  
**依赖**：Story 14.1、Story 14.2

**验收标准**：
- [x] 实现 `StreamTask/ShardRunner` 聚合模型（`Stop/Join/Snapshot`）
- [x] 实现 `StreamRuntime`（`ready_queue + timer_queue + worker/timer threads`）
- [x] 三种场景统一执行：`NONE/STATELESS/KEYED`
- [x] 实现 `INTO` 真实 sink 绑定：`stream/dataframe/database/other` 透传真实 `IChannel`
- [x] 完成流式算子 `Init(with_params_json, StreamSinkContext)` 改造，算子显式校验 `sink_type`
- [x] 非 stream sink 单写者约束生效（并行算子自动降级，不报错）
- [x] `builtin.*` 两段式数据库目标失败策略生效（三段式成功）
- [x] 取消 `WITH sink_table` 框架兜底语义，并补齐回归测试
- [x] 新增流式任务管理 API：`/tasks/stream/execute|stop|status|list`

**任务分解**：
- 说明：`T17.1~T17.4` 为旧方案已完成项；`T17.5+` 为 2026-04-02 修订后增量待办。
- [x] T13：新增 `src/services/scheduler/stream_task.h/cpp`
- [x] T14：新增 `src/services/scheduler/stream_runtime.h/cpp`
- [x] T15：修改 `src/services/scheduler/scheduler_plugin.h/cpp`
- [x] T16：实现 `ExecuteStreamTask()` 与并行拓扑装配（`BuildInputPorts`）
- [x] T17：实现状态机迁移与 `kWaitingRetry` 定时重试链路
- [x] T17.1：新增 `src/framework/core/stream_channel_adapter.h/cpp`（`IStreamChannel` 适配写出到 dataframe/database）
- [x] T17.2：在 `ExecuteStreamTask()` 增加 `BindSinkMode()` 与 `INTO dataframe.*` 单写者限制
- [x] T17.3：补充适配链路测试（`INTO dataframe.*` / `INTO <db_type>.<db_name>[.<table>]`）
- [x] T17.4：补充 `INTO <db_type>.<db_name>[.<table>]` 表名规则测试（第三段优先、`WITH sink_table` 兜底、缺失报错；`test_scheduler_e2e` T43 覆盖）
- [x] T17.5（修订）：`IStreamOperator::Init` 改为接收 `StreamSinkContext`，并更新内置流式算子签名与实现
- [x] T17.6（修订）：Scheduler 将 `BindSinkMode()` 重构为真实 sink 绑定（`ResolveStreamSink()`），移除框架层 `WITH sink_table` 处理
- [x] T17.7（修订）：builtin 流式算子下沉 adapter 逻辑；`INTO <db_type>.<db_name>` 返回显式错误
- [x] T17.8（修订）：新增普通算子两段式数据库目标用例（应收到 `IDatabaseChannel` 并可写入），更新/替换旧 T43 断言
- [x] T17.9（修订）：补齐文档与错误码文案，确保 `sink_type` 不支持时错误信息可定位

---

### Story 14.4（补充）：Web 端最小流式可用性

**优先级**：P1  
**工作量估算**：0.5 天  
**依赖**：Story 14.0、Story 14.3

**验收标准**：
- [x] `Channels` 页面新增 `Stream 通道` 分组，展示 `type/name/status`（只读）
- [x] `Tasks` 页面支持一键填充演示 SQL（`SELECT * FROM tcp_session_mock.tcp_src USING builtin.tcp_service_merge_stream INTO dataframe.serviceaccess`）
- [x] 不引入流式通道的新增/编辑/删除表单（保持本 Sprint 边界）

**任务分解**：
- [x] T21：Web 层新增流式通道查询接口 `/api/channels/stream/list`（代理 Scheduler `stream factory` 查询）
- [x] T22：前端 API 新增 `listStreamChannels()`，并在 `Channels.vue` 新增 `Stream 通道` 只读列表
- [x] T23：`Tasks.vue` 新增“流式示例 SQL”快捷填充按钮
- [x] T24：新增 `test_scheduler_e2e` Web 代理接口冒烟校验（仅校验 HTTP 200 + JSON 格式）

---

## 测试与验证

**测试文件**：`src/tests/test_stream/test_stream.cpp`

- [x] T18：实现 T1~T5（通道与基础场景）
- [x] T19：实现 T6~T10（并行与生命周期）
- [x] T20：实现 T11~T20（异常、状态机、adapter 与 sink 绑定）

**验证命令**：

```bash
cmake --build build -j4
ctest --test-dir build -R test_stream --output-on-failure
```

---

## 实施顺序

```text
Day 1: Story 14.0
Day 2-3: Story 14.1
Day 4-5: Story 14.2
Day 6-7: Story 14.3 + 测试收敛（T1~T20）
Day 8: Story 14.4（Web 最小入口）
```

---

## 风险与缓解

| 风险 | 可能性 | 缓解措施 |
|------|--------|---------|
| `STATELESS/SPMC` 终止协议实现不一致导致消费者悬挂 | 中 | 固化 `producer_finished && queue_empty` 协议，并加专项测试（T7） |
| 状态机迁移遗漏导致漏调度或重复调度 | 中 | 保持 6 态 CAS 约束，覆盖 T14/T15 |
| Stop/Cancel 路径与失败路径冲突 | 中 | `SetFailedOnce` first-failure-wins；`RequestStop` 不覆盖终态 |
| `INTO` 非 stream sink 多 shard 并发写导致顺序与一致性复杂化 | 中 | Scheduler 对 `sink_type != stream` 强制单写者（`parallelism=1`），并覆盖 T20 |

---

## 交付物清单

1. 流式接口：`istream_channel.h`、`istream_operator.h`、`istream_factory.h`
2. 流式核心：`ring_stream_channel.*`、`fan_in_stream_channel.*`、`fan_out_stream_channel.*`
3. 适配层：`stream_channel_adapter.*`（公共适配工具，供算子选择使用）
4. 调度核心：`stream_task.*`、`stream_runtime.*`、`scheduler_plugin.*`
5. 服务插件：`stream_plugin.*`
6. 测试：`test_stream`
7. Web：`web_server.*`、`frontend/src/views/Channels.vue`、`frontend/src/views/Tasks.vue`
8. 文档：`tasks/sprints/sprint12/design.md`、`tasks/product_backlog.md`、本计划文件
