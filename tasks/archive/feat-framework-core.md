# Feature: C++ 框架核心能力

状态：`[x]` 已完成

## 业务意图

提供插件式 C++ 进程框架、统一数据接口和批处理 Pipeline，作为其他插件的运行基础。

## Non-Goals

- 不包含 Python Worker、数据库驱动和 Web 管理功能。
- 不把具体业务算子固化进框架核心。

## 公共契约

核心接口包括 `IDataEntity`、`IDataFrame`、`IChannel` 和 `IOperator`。`DataFrame` 支持行列访问及 JSON/Arrow IPC 边界转换；`Pipeline` 负责 source → operator → sink 的批量执行和状态迁移。

## 主链路

1. 插件加载器发现并装配 channel/operator。
2. Pipeline 从 source 读取 batch，调用 operator，再将结果写入 sink。

## 完成任务

- `[x]` 冻结数据、通道和算子接口。
- `[x]` 实现 DataFrame 与 Arrow IPC 互操作。
- `[x]` 实现 Pipeline、状态机和插件加载。
- `[x]` 以 MemoryChannel、PassthroughOperator 和集成测试验证主链路。
