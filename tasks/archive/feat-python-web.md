# Feature: Python 算子与 Web 管理

状态：`[x]` 已完成

## 业务意图

打通 C++ 与 Python 算子执行链路，并提供算子、任务和通道的 Web 管理入口。

## Non-Goals

- 不把 Python 运行时嵌入 C++ 进程。
- 不用 HTTP 承载高吞吐数据面；数据载荷使用 Arrow IPC。

## 公共契约

`PythonOperatorBridge` 负责 channel → Arrow IPC → Worker HTTP → Arrow IPC → channel；`PythonProcessManager` 负责 Worker 的启动、就绪、存活和停止。Web 层提供 health、channels、operators、tasks 及结果查询能力。

## 主链路

1. Bridge 启动 Worker，发现并注册 Python 算子。
2. Web 提交 SQL 任务，Scheduler 调用 Bridge，Worker 返回处理后的 batch。

## 完成任务

- `[x]` 实现 Arrow 编解码和 C++ Bridge。
- `[x]` 实现 Python Worker、算子基类和注册表。
- `[x]` 实现 Web API、上传/激活和任务管理。
- `[x]` 以桥接测试和端到端测试验证完整执行链路。
