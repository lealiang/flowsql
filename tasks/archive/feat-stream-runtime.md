# Feature: 流式运行时

状态：`[x]` 已完成

## 业务意图

提供流式通道、流式算子和通用执行容器，支持跨进程管理、共享 source、具名 sink、并发 Ring 和 Group DAG 编排。

## Non-Goals

- 不在流式数据面实现历史持久化回放；历史补算单独作为待规划 Feature。
- 不把具体业务分析逻辑固化进 StreamRuntime。

## 公共契约

`IStreamChannel` 负责 `Open/Put/PollNext/Cancel/Close`，`IStreamOperator` 负责配置、处理、tick 和 flush；`StreamRuntime` 输出 `TaskSnapshot` 并管理 `Stop/Join`。共享 source 以消费者隔离、late join 和背压指标为契约。

## 主链路

1. StreamPlugin 注册通道和算子，Scheduler 创建 source、operator、sink 和运行任务。
2. Runtime 按 Ring/DAG 拓扑消费数据，处理停止、取消、失败和共享消费者状态。

## 完成任务

- `[x]` 冻结 StreamChannel、StreamOperator、Ring 和 block stream 契约。
- `[x]` 实现 StreamRuntime、ShardRunner、任务状态和跨进程入口。
- `[x]` 完成 MPSC/MPMC、具名 sink、Group/Hybrid DAG 和共享 source。
- `[x]` 完成背压、生命周期、并发和端到端测试。
