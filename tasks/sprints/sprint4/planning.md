# Sprint 4 规划

## Sprint 信息

- **Sprint 周期**：Sprint 4
- **开始日期**：2026-03-05
- **预计工作量**：11 天（大型 Sprint）
- **Sprint 目标**：实现 MySQL 驱动支持、数据库连接池基础功能、SQL 高级特性

---

## Sprint 目标

### 主要目标

1. ✅ 实现 MySQL 驱动支持，提供与 SQLite 一致的数据库操作能力
2. ✅ 实现数据库连接池基础功能，支持连接复用和超时回收
3. ✅ 支持 SQL 高级特性（JOIN/GROUP BY/ORDER BY），透传给数据库引擎

### 成功标准

- MySQL 驱动端到端测试全部通过
- 连接池功能测试通过，连接复用率 > 80%
- SQL 高级特性在 MySQL 和 SQLite 上都能正确执行

---

## Story 列表

### Story 4.1: MySQL 驱动支持

**优先级**：P0（必须完成）
**工作量估算**：6 天
**负责人**：待分配

**验收标准**：
- [x] 实现 `SqlDbDriver` 抽象基类（含 `SqlBatchReader/SqlBatchWriter`）
- [x] 实现 `MysqlDriver`（基于 libmysqlclient）
- [x] 支持预编译语句
- [x] 支持事务控制（BEGIN/COMMIT/ROLLBACK）
- [x] 端到端测试通过（连接/读取/写入/事务）

**任务分解**：

#### Task 4.1.1: 实现 SqlDbDriver 抽象基类（3 天）
- [ ] 定义 `SqlDbDriver` 类和钩子方法接口
  - `ExecuteQueryImpl(sql, error): void*`
  - `InferSchemaImpl(result, error): Schema`
  - `FetchRowImpl(result, builders, error): int`
  - `FreeResultImpl(result): void`
  - `ExecuteSqlImpl(sql, error): int`
  - `BeginTransactionImpl/CommitTransactionImpl/RollbackTransactionImpl`
- [ ] 实现 `SqlBatchReader` 类
  - 创建 Arrow builders
  - 循环调用 `FetchRowImpl` 读取行
  - 构建 RecordBatch
  - IPC 序列化
- [ ] 实现 `SqlBatchWriter` 类
  - IPC 反序列化
  - 自动建表（Arrow Schema → SQL DDL）
  - 批量 INSERT
  - 事务管理
- [ ] 单元测试（使用 Mock 驱动验证基类逻辑）

#### Task 4.1.2: 实现 MysqlDriver（2 天）
- [ ] 实现连接管理
  - `Connect(params): int`
  - `Disconnect(): int`
  - `IsConnected(): bool`
- [ ] 实现 7 个钩子方法
  - `ExecuteQueryImpl`：`mysql_stmt_init` + `mysql_stmt_prepare` + `mysql_stmt_execute`
  - `InferSchemaImpl`：`mysql_stmt_result_metadata` + MySQL 类型 → Arrow 类型映射
  - `FetchRowImpl`：`mysql_stmt_fetch` + `MYSQL_BIND` 读取列值
  - `FreeResultImpl`：`mysql_stmt_close`
  - `ExecuteSqlImpl`：`mysql_query`
  - `BeginTransactionImpl/CommitTransactionImpl/RollbackTransactionImpl`
- [ ] MySQL 类型映射表
  - `MYSQL_TYPE_LONGLONG` → `arrow::int64()`
  - `MYSQL_TYPE_DOUBLE` → `arrow::float64()`
  - `MYSQL_TYPE_STRING` → `arrow::utf8()`
  - 其他类型...

#### Task 4.1.3: 端到端测试（1 天）
- [ ] 连接管理测试
  - 测试正常连接
  - 测试连接失败（错误的主机/用户名/密码）
  - 测试断开连接
- [ ] 读取测试
  - 测试 SELECT 查询
  - 测试 WHERE 过滤
  - 测试空结果集
  - 测试大数据量（> 10000 行）
- [ ] 写入测试
  - 测试 INSERT（自动建表）
  - 测试多批次写入
  - 测试不同数据类型（INT/DOUBLE/STRING）
- [ ] 事务测试
  - 测试 COMMIT
  - 测试 ROLLBACK
  - 测试事务失败回滚

**依赖**：
- libmysqlclient 8.0+
- Arrow 库（已集成）

**风险**：
- `void*` 类型擦除可能导致类型转换错误
- MySQL 客户端库在某些环境下不可用

**缓解措施**：
- 单元测试覆盖所有钩子方法
- 提供 CMake 选项 `ENABLE_MYSQL`，默认关闭
- 文档说明如何安装 libmysqlclient

---

### Story 4.3: 数据库连接池基础实现

**优先级**：P1（重要）
**工作量估算**：2.5 天
**负责人**：待分配

**验收标准**：
- [x] DatabasePlugin 层面统一连接池管理
- [x] 支持连接复用和空闲超时回收
- [x] 支持最大连接数限制
- [x] 基础功能测试通过

**任务分解**：

#### Task 4.3.1: 设计连接池接口（0.5 天）
- [ ] 定义 `ConnectionPool` 类
  - `Acquire(key, factory): IDbDriver*`
  - `Release(key, driver): void`
- [ ] 定义配置参数
  - `max_connections`：最大连接数（默认 10）
  - `idle_timeout_sec`：空闲超时（默认 300 秒）
- [ ] 设计数据结构
  - `Pool`：活跃连接集合 + 空闲连接队列
  - `Connection`：驱动指针 + 最后使用时间

#### Task 4.3.2: 实现连接池（1.5 天）
- [ ] 实现 `Acquire` 方法
  - 从空闲队列获取连接
  - 检查连接是否超时
  - 如果无空闲连接且未达上限，创建新连接
  - 如果达到上限，返回 nullptr（简化实现）
- [ ] 实现 `Release` 方法
  - 从活跃集合移除
  - 加入空闲队列
  - 记录最后使用时间
- [ ] 线程安全
  - 使用 `std::mutex` 保护共享数据
  - 使用 `std::unique_lock` 管理锁

#### Task 4.3.3: 集成到 DatabasePlugin（0.5 天）
- [ ] 修改 `DatabasePlugin::Get()` 方法
  - 使用连接池获取连接
  - 如果连接池返回 nullptr，创建新连接（向后兼容）
- [ ] 修改 `DatabasePlugin::Release()` 方法
  - 归还连接到连接池
- [ ] 配置参数解析
  - 从 `gateway.yaml` 读取连接池配置

**依赖**：
- Story 4.1（MySQL 驱动）

**风险**：
- 多线程环境下可能出现竞态条件
- 连接泄漏（未正确归还）

**缓解措施**：
- 单元测试覆盖并发场景
- 压力测试验证线程安全
- 添加连接泄漏检测（延后到 Story 4.4）

---

### Story 4.5: SQL 高级特性

**优先级**：P1（重要）
**工作量估算**：2.5 天
**负责人**：待分配

**验收标准**：
- [x] 支持 JOIN 操作（INNER/LEFT/RIGHT/FULL）
- [x] 支持 GROUP BY 和聚合函数
- [x] 支持 ORDER BY 和 LIMIT
- [x] 支持子查询
- [x] 支持 UNION/INTERSECT/EXCEPT

**任务分解**：

#### Task 4.5.1: SqlParser 识别关键字（1 天）
- [ ] 识别 JOIN 相关关键字
  - `INNER JOIN`, `LEFT JOIN`, `RIGHT JOIN`, `FULL JOIN`, `ON`
- [ ] 识别聚合相关关键字
  - `GROUP BY`, `HAVING`, `COUNT`, `SUM`, `AVG`, `MAX`, `MIN`
- [ ] 识别排序相关关键字
  - `ORDER BY`, `ASC`, `DESC`, `LIMIT`, `OFFSET`
- [ ] 识别集合操作关键字
  - `UNION`, `INTERSECT`, `EXCEPT`
- [ ] 识别子查询（括号嵌套）
- [ ] 新增方法 `HasAdvancedFeatures(sql): bool`

#### Task 4.5.2: 修改 BuildQuery 逻辑（0.5 天）
- [ ] 修改 `Scheduler::BuildQuery()` 方法
  - 如果 `HasAdvancedFeatures()` 返回 true，直接返回原 SQL
  - 否则，保持原有的简单 SQL 拼接逻辑
- [ ] 删除冗余的 SQL 拼接代码
- [ ] 更新注释和文档

#### Task 4.5.3: 端到端测试（1 天）
- [ ] MySQL 测试
  - 测试 INNER JOIN
  - 测试 LEFT JOIN
  - 测试 GROUP BY + COUNT/SUM/AVG
  - 测试 ORDER BY + LIMIT
  - 测试子查询
  - 测试 UNION
- [ ] SQLite 测试（验证向后兼容）
  - 测试简单查询（SELECT * FROM table WHERE ...）
  - 测试 JOIN 查询
  - 测试 GROUP BY 查询
- [ ] 复杂查询测试
  - 测试多表 JOIN + GROUP BY + ORDER BY
  - 测试嵌套子查询
  - 测试 UNION ALL

**依赖**：
- Story 4.1（MySQL 驱动）

**风险**：
- SQL 解析器可能过于复杂
- 某些数据库不支持某些 SQL 特性

**缓解措施**：
- 只识别关键字，不解析语法树
- 复杂 SQL 直接透传给数据库引擎
- 文档说明各数据库支持的 SQL 特性差异

---

## 依赖关系

```
Story 4.1 (MySQL 驱动)
    ├── Story 4.3 (连接池) [依赖 4.1]
    └── Story 4.5 (SQL 高级特性) [依赖 4.1]
```

**并行开发**：
- Story 4.1 的 Task 1.1（抽象基类）完成后，可以并行开发：
  - Task 1.2（MysqlDriver）
  - Task 5.1（SqlParser 识别关键字）

---

## 风险与缓解

### 风险 1：libmysqlclient 依赖

**风险描述**：MySQL 客户端库可能在某些环境下不可用。

**影响**：无法编译 MySQL 驱动。

**缓解措施**：
- 提供 CMake 选项 `ENABLE_MYSQL`，默认关闭
- 文档说明如何安装 libmysqlclient
- 提供 Docker 镜像，预装所有依赖

### 风险 2：void* 类型擦除

**风险描述**：`void*` 失去类型安全，可能导致类型转换错误。

**影响**：运行时崩溃或数据损坏。

**缓解措施**：
- 在钩子方法文档中明确 `void*` 的实际类型
- 使用 `static_cast` 而非 `reinterpret_cast`
- 单元测试覆盖所有钩子方法
- 代码审查时重点检查类型转换

### 风险 3：连接池并发安全

**风险描述**：多线程环境下连接池可能出现竞态条件。

**影响**：连接泄漏、死锁、数据损坏。

**缓解措施**：
- 使用 `std::mutex` 保护共享数据
- 单元测试覆盖并发场景
- 压力测试验证线程安全

### 风险 4：SQL 解析器复杂度

**风险描述**：识别 SQL 高级特性可能导致解析器过于复杂。

**影响**：开发周期延长，维护成本增加。

**缓解措施**：
- 只识别关键字，不解析语法树
- 复杂 SQL 直接透传给数据库引擎
- 保持解析器简单，避免重复造轮子

---

## 测试计划

### 单元测试

| 测试文件 | 测试内容 | 预计用例数 |
|---------|---------|-----------|
| `test_sql_db_driver.cpp` | SqlDbDriver 基类逻辑 | 10 |
| `test_mysql_driver.cpp` | MysqlDriver 钩子方法 | 15 |
| `test_connection_pool.cpp` | 连接池功能 | 12 |
| `test_sql_parser_advanced.cpp` | SQL 高级特性解析 | 20 |

**总计**：57 个单元测试用例

### 集成测试

| 测试文件 | 测试内容 | 预计用例数 |
|---------|---------|-----------|
| `test_mysql_e2e.cpp` | MySQL 端到端测试 | 8 |
| `test_connection_pool_e2e.cpp` | 连接池端到端测试 | 5 |
| `test_sql_advanced_e2e.cpp` | SQL 高级特性端到端测试 | 10 |

**总计**：23 个集成测试用例

### 性能测试

| 测试文件 | 测试内容 | 性能指标 |
|---------|---------|---------|
| `benchmark_connection_pool.cpp` | 连接池性能 | 连接获取延迟 < 1ms |
| `benchmark_mysql_driver.cpp` | MySQL 驱动性能 | 吞吐量 > 100MB/s |

---

## 验收标准

### Story 4.1 验收

- [ ] 所有单元测试通过（`test_sql_db_driver` + `test_mysql_driver`）
- [ ] 所有集成测试通过（`test_mysql_e2e`）
- [ ] 代码审查通过（无 P0/P1 问题）
- [ ] 文档更新（README + design.md）

### Story 4.3 验收

- [ ] 所有单元测试通过（`test_connection_pool`）
- [ ] 所有集成测试通过（`test_connection_pool_e2e`）
- [ ] 性能测试通过（连接复用率 > 80%）
- [ ] 代码审查通过

### Story 4.5 验收

- [ ] 所有单元测试通过（`test_sql_parser_advanced`）
- [ ] 所有集成测试通过（`test_sql_advanced_e2e`）
- [ ] MySQL 和 SQLite 都能正确执行 SQL 高级特性
- [ ] 代码审查通过

---

## 交付物

### 代码

- `src/services/database/sql_db_driver.h/.cpp` — SqlDbDriver 抽象基类
- `src/services/database/drivers/mysql_driver.h/.cpp` — MysqlDriver 实现
- `src/services/database/connection_pool.h/.cpp` — ConnectionPool 实现
- `src/framework/core/sql_parser.h/.cpp` — SQL 高级特性支持
- `src/services/scheduler/scheduler_plugin.cpp` — BuildQuery 修改

### 测试

- `src/tests/test_database/test_sql_db_driver.cpp`
- `src/tests/test_database/test_mysql_driver.cpp`
- `src/tests/test_database/test_connection_pool.cpp`
- `src/tests/test_database/test_sql_parser_advanced.cpp`
- `src/tests/test_database/test_mysql_e2e.cpp`
- `src/tests/test_database/test_connection_pool_e2e.cpp`
- `src/tests/test_database/test_sql_advanced_e2e.cpp`

### 文档

- `tasks/sprints/sprint4/design.md` — 技术设计文档（已完成）
- `tasks/sprints/sprint4/planning.md` — Sprint 规划（本文档）
- `README.md` — 更新 MySQL 驱动使用说明
- `docs/commands.md` — 记录命令历史

---

## 时间线

| 日期 | 里程碑 | 交付物 |
|------|--------|--------|
| Day 1-3 | 实现 SqlDbDriver 抽象基类 | sql_db_driver.h/.cpp |
| Day 2-3 | 并行：SqlParser 识别关键字 | sql_parser.h/.cpp |
| Day 4-5 | 实现 MysqlDriver | mysql_driver.h/.cpp |
| Day 6 | MySQL 驱动端到端测试 | test_mysql_e2e.cpp |
| Day 7-8 | 实现连接池 | connection_pool.h/.cpp |
| Day 9 | 连接池集成和测试 | test_connection_pool_e2e.cpp |
| Day 10 | SQL 高级特性端到端测试 | test_sql_advanced_e2e.cpp |
| Day 11 | 代码审查和文档更新 | README.md + review.md |

---

## 下一步

1. 确认 Sprint 规划
2. 分配任务负责人
3. 开始实施 Story 4.1 Task 1.1
