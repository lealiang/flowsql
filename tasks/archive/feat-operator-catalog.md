# Feature: 通道与算子目录

状态：`[x]` 已完成

## 业务意图

建立通道、算子元信息和激活状态的统一目录，消除 Web、Bridge、Scheduler 之间的重复状态。

## Non-Goals

- 不让目录承担算子执行或调度职责。
- 不为不同算子类型维护多套查询入口和状态真相。

## 公共契约

`IChannelRegistry` 和算子目录接口提供注册、查询、遍历及激活状态读写。目录记录至少区分 `name`、`type`、`source`、`description`、`position` 和运行态 `active`。

## 主链路

1. 内置或 Python 算子完成注册，CatalogPlugin 持久化元信息且不覆盖用户状态。
2. Web 和 Scheduler 通过 Catalog 查询目录，Scheduler 以 `active` 状态作为执行准入。

## 完成任务

- `[x]` 实现 CatalogPlugin、通道注册接口和统一查询。
- `[x]` 将 Python 算子同步到 Catalog。
- `[x]` 切换 Web/Scheduler 的读写路径并移除重复入口。
- `[x]` 完成目录、状态和跨模块集成测试。
