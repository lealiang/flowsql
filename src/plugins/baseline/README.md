# Baseline 插件说明

本文是 Baseline 插件的长期入口文档，面向调用方、调度层和后续维护者。阶段设计文档记录实现过程和取舍；本文只记录当前对外能力、基本使用方式和必须遵守的接口契约。

## 1. 能力概览

Baseline 插件通过 `IBaselineService` 向同进程内其他插件暴露能力。调用方通过 `IID_BASELINE_SERVICE` 获取服务接口，再创建具体 task。

当前 task 类型：

| 类型 | 接口 | 主要用途 |
| --- | --- | --- |
| Value | `IBaselineValueTask` | 对单值序列建立在线 rolling baseline，支持历史 bootstrap、在线提交、预测、快照和导出 |
| Ratio | `IBaselineRatioTask` | 对比例 / 份额类序列建立在线 rolling baseline，接口形态与 Value 对齐 |
| Relation | `IBaselineRelationTask` | 对关系分布 block 做 routed summary rolling、stream basis、relation fusion 和 source / routed snapshot |

核心能力：

1. 历史 bootstrap：从历史样本训练 artifact / seed，用于冷启动和恢复。
2. 在线 rolling：对新 bucket 提交观测，输出 baseline band、score、trust、maturity 和诊断字段。
3. Relation routed summary：将关系分布拆成有界 routed summary，复用 Value / Ratio rolling core。
4. Relation stream basis：无历史或历史不足时，在线积累并刷新 basis。
5. Relation fusion：将 routed summary evidence 合成为 source 级 relation risk 和 pattern 解释。
6. Snapshot / export：支持 task、series、routed summary、bootstrap artifact / seed 的 JSON 观测和导出。

## 2. 基本使用

### 2.1 获取服务

Baseline 是插件能力，不通过 HTTP 直接暴露。调用方应通过框架的 interface 查询机制获取：

```cpp
auto* service = static_cast<flowsql::IBaselineService*>(
    querier->First(flowsql::IID_BASELINE_SERVICE));
```

具体查询 API 以当前框架接口为准。调用方不应直接依赖 `flowsql::baseline::*` 下的实现类。

### 2.2 创建 task

通过 JSON 配置创建 task：

```cpp
auto [status, task] = service->CreateValueTask(
    config_json,
    flowsql::BaselineSerializationFormat::kJson);

if (status != flowsql::BaselineStatus::kOk || !task) {
    // 处理配置解析或创建失败。
}
```

配置模板参考：

- `src/plugins/baseline/config/baseline-config-template.yaml`

目前 public 序列化格式以 JSON 为主。接口保留 `BaselineSerializationFormat` 参数，调用方不应假设未来只存在 JSON。

### 2.3 Value / Ratio 主流程

典型流程：

1. `CreateValueTask()` / `CreateRatioTask()` 创建 task。
2. 可选：`Bootstrap()` 或 `LoadBootstrapArtifact()` 预热历史模型。
3. 调用 `SubmitObservation()` 提交在线 bucket。
4. 调用 `PredictRolling()` / `PredictBootstrap()` 做只读预测。
5. 调用 `QueryTaskSnapshot()` / `QuerySeriesSnapshot()` 观测运行时状态。
6. 调用 `ExportBootstrapArtifact()` / `ExportBootstrapSeed()` 导出恢复数据。
7. 调用 `Close()` 关闭 task。

`PredictRolling()` 是只读预测接口，不应触发状态初始化或在线学习。在线学习只通过 `SubmitObservation()` 推进。

### 2.4 Relation 主流程

典型流程：

1. `CreateRelationTask()` 创建 Relation task。
2. 可选：`Bootstrap()` 或 `LoadBootstrapArtifact()` 加载历史 relation basis 和 routed summary seed。
3. 调用 `SubmitObservation()` 提交在线 relation block。
4. 调用 `PredictRoutedSummary()` 预测某个 routed summary。
5. 调用 `QuerySeriesSnapshot(source_series_key)` 观测 source 级状态和 relation fusion。
6. 调用 `QueryRoutedSummarySnapshot(query)` 观测某个 routed summary 的底层 rolling 状态。
7. 调用 `ExportBootstrapArtifact()` / `ExportBootstrapSeed()` / `QueryBootstrapBasis()` 导出或观测 bootstrap 结果。
8. 调用 `Close()` 关闭 task。

Relation 的 `RelationRollingObservation.metrics` 必须与 task config 中的 `metrics` 数组同序对齐。`RelationBootstrapMetric.metric` 是可选名称校验字段；若非空，必须等于同下标的 task metric。

## 3. 接口契约

### 3.1 Task 所有权

Task 由 `std::shared_ptr<IBaseline*Task>` 持有。`Close()` 会关闭 task 并从 registry 移除，但调用方已经持有的 `shared_ptr` 不会立即失效。

调用方应将 `Close()` 视为 task 调用序列中的状态迁移点：

1. 排在 `Close()` 之前的调用先完成。
2. 排在 `Close()` 之后的调用应看到 closed 状态并拒绝。
3. `Close()` 后不应继续把该 task 暴露给新的业务调用链。

### 3.2 同 task 非并发调用契约

Baseline task 实例按外部串行化状态机理解。调用方 / 上游调度必须保证：

1. 同一个 task 的 public API 调用不会重叠执行。
2. 如果同一个 task 的连续调用发生在不同物理线程，上游调度必须在前一次调用结束与后一次调用开始之间建立 happens-before，保证 task 内部非 atomic 状态的可见性。
3. 同一个 task 不要求固定物理线程，Baseline 也不做线程身份检查。
4. 不同 task 可以并行调用。

当前实现可能仍保留内部锁作为过渡，但调用方不得依赖这些锁来直接并发访问同一个 task。后续锁优化会按上述契约收敛 task 内部 mutex。

### 3.3 Immutable identity getter

以下 getter 是 task identity 能力，在 task 对象生命周期内必须保持跨线程读取能力：

1. `Id()`
2. `Name()`
3. `Kind()`

这些字段构造后不可变，getter 不应依赖 task runtime mutex。除这 3 个 immutable identity getter 外，其他 task public API 默认都遵守“同 task 不重叠执行”的契约。

### 3.4 Service、registry 与生命周期边界

`IBaselineService`、`TaskRegistry`、plugin lifecycle 和 runtime config 是 task 契约之外的并发边界：

1. `TaskRegistry` 负责 task 表和 task id 分配，内部保留短临界区同步。
2. Runtime config 使用 immutable snapshot 方式整体替换。
3. Plugin lifecycle 是否可与 service API 并发，由框架生命周期契约决定；若框架不保证串行，Baseline 需要单独的 plugin lifecycle 保护。

### 3.5 Snapshot 与导出

Snapshot 用于观测和调试，不应被调用方当成热路径状态传递格式。进程内阶段交接应使用结构体，JSON 只服务于边界导出、审计、调试和恢复。

当前 public snapshot / export 的稳定入口：

| 能力 | 接口 |
| --- | --- |
| task 配置导出 | `ExportConfig()` |
| task 快照 | `QueryTaskSnapshot()` |
| series 快照 | `QuerySeriesSnapshot()` |
| bootstrap artifact 导出 / 导入 | `ExportBootstrapArtifact()` / `LoadBootstrapArtifact()` |
| bootstrap seed 导出 | `ExportBootstrapSeed()` |
| Relation routed summary 快照 | `QueryRoutedSummarySnapshot()` |
| Relation bootstrap basis 查询 | `QueryBootstrapBasis()` |

## 4. 维护要求

新增或修改 Baseline public 能力时，需要同步检查：

1. `src/framework/interfaces/ibaseline_service.h`
2. `src/framework/interfaces/ibaseline_types.h`
3. `src/plugins/baseline/config/baseline-config-template.yaml`
4. `src/tests/test_baseline/*`
5. 本 README

新增配置项必须同步更新 C++ 默认值、YAML 模板、strict schema 和配置测试。新增 public 字段必须保持 append-only 兼容策略，除非阶段设计明确允许破坏性迁移。
