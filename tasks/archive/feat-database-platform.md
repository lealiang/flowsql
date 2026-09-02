# Feature: 数据库平台

状态：`[x]` 已完成

## 业务意图

建立数据库插件和驱动抽象，提供 SQLite、MySQL、PostgreSQL、ClickHouse 的批量读写、连接池和 SQL 过滤闭环。

## Non-Goals

- 不让 Scheduler 或 Web 直接依赖具体数据库驱动类。
- 不把数据库连接状态写入构建期配置。

## 公共契约

`IDatabaseFactory` 管理通道实例，`IDatabaseChannel` 提供通道元数据和 reader/writer 创建，`IDbDriver` 封装具体数据库操作。SQL 解析后的过滤条件由通道或 DataFrame 执行，并返回结构化错误。

## 主链路

1. DatabasePlugin 根据配置创建或查找数据库通道。
2. Scheduler 通过抽象接口创建 reader/writer，批量传输 Arrow 数据并执行过滤。

## 完成任务

- `[x]` 完成数据库工厂、通道和驱动接口解耦。
- `[x]` 实现 SQLite 读写、只读/WAL 和安全校验。
- `[x]` 接入 MySQL、PostgreSQL、ClickHouse 驱动。
- `[x]` 实现连接池、SQL 高级特性和 DataFrame Filter。
- `[x]` 以数据库及端到端测试验证读写、认证和错误透传。
