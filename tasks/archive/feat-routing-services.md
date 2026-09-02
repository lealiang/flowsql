# Feature: 路由与服务对等化

状态：`[x]` 已完成

## 业务意图

统一插件路由、跨进程 HTTP 调用和结构化错误映射，使 Gateway、Scheduler、Database、Web 等服务保持对等边界。

## Non-Goals

- 不在路由层实现业务逻辑或数据库操作。
- 不引入跨进程共享内存作为控制面协议。

## 公共契约

`IRouterHandle` 描述方法、URI、请求和响应处理；路由表负责精确匹配与分发；错误映射器把业务错误码转换为统一 HTTP 状态和结构化错误体。

## 主链路

1. 服务插件注册路由处理器。
2. Gateway 接收请求并转发到目标服务，统一处理 CORS、状态码和错误响应。

## 完成任务

- `[x]` 冻结路由接口、错误码和批次加载边界。
- `[x]` 实现 RouterAgencyPlugin 和统一分发。
- `[x]` 迁移 Gateway、Scheduler、Database、Web 路由。
- `[x]` 完成守护进程、容器编排和跨服务端到端验证。
