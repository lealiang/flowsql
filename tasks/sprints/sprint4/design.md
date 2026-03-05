# Sprint 4 技术设计文档

## 概述

Sprint 4 实现 Epic 4 的核心功能：MySQL 驱动支持、数据库连接池基础实现、SQL 高级特性。

**核心技术方案**：采用继承抽象基类（方案 A），提供 `SqlDbDriver` 封装公共逻辑，新增驱动只需实现钩子方法。

---

## 架构设计

### 整体架构

```
DatabasePlugin (工厂层)
    ├── ConnectionPool (连接池管理)
    └── IDbDriver (驱动抽象)
            ├── SqliteDriver (已实现)
            ├── MysqlDriver (本 Sprint 实现)
            └── PostgresDriver (未来扩展)

SqlDbDriver (抽象基类)
    ├── SqlBatchReader (通用读取器)
    └── SqlBatchWriter (通用写入器)
```

### 类图

```
┌─────────────────────────────────────────────────────────────┐
│                        IDbDriver                            │
│  + Connect(params): int                                     │
│  + Disconnect(): int                                        │
│  + IsConnected(): bool                                      │
│  + CreateReader(query, reader): int                         │
│  + CreateWriter(table, writer): int                         │
│  + DriverName(): const char*                                │
│  + LastError(): const char*                                 │
└─────────────────────────────────────────────────────────────┘
                            △
                            │
┌───────────────────────────┴─────────────────────────────────┐
│                      SqlDbDriver                            │
│  # ExecuteQueryImpl(sql, error): void*                      │
│  # InferSchemaImpl(result, error): Schema                   │
│  # FetchRowImpl(result, builders, error): int               │
│  # FreeResultImpl(result): void                             │
│  # ExecuteSqlImpl(sql, error): int                          │
│  # BeginTransactionImpl(error): int                         │
│  # CommitTransactionImpl(error): int                        │
│  # RollbackTransactionImpl(error): int                      │
│  + CreateReader(query, reader): int  [模板方法]            │
│  + CreateWriter(table, writer): int  [模板方法]            │
└─────────────────────────────────────────────────────────────┘
                            △
                            │
                ┌───────────┴───────────┐
                │                       │
    ┌───────────┴──────────┐  ┌────────┴──────────┐
    │   SqliteDriver       │  │   MysqlDriver     │
    │  - db_: sqlite3*     │  │  - conn_: MYSQL*  │
    └──────────────────────┘  └───────────────────┘
```

---

## 核心接口设计

### 1. SqlDbDriver 抽象基类

**职责**：封装 SQL 数据库的公共逻辑，提供模板方法。

**关键方法**：

```cpp
class SqlDbDriver : public IDbDriver {
protected:
    // ===== 子类必须实现的钩子方法 =====

    // 执行查询，返回原生结果集句柄（void* 类型擦除）
    // 返回值：成功返回结果集指针，失败返回 nullptr 并设置 error
    virtual void* ExecuteQueryImpl(const char* sql, std::string* error) = 0;

    // 从结果集推断 Arrow Schema
    // 提示：遍历列元数据，映射数据库类型到 Arrow 类型
    virtual std::shared_ptr<arrow::Schema> InferSchemaImpl(
        void* result, std::string* error) = 0;

    // 从结果集读取一行数据，填充到 Arrow builders
    // 返回值：0=成功，1=无更多数据，-1=错误
    virtual int FetchRowImpl(
        void* result,
        const std::vector<std::unique_ptr<arrow::ArrayBuilder>>& builders,
        std::string* error) = 0;

    // 释放结果集资源
    virtual void FreeResultImpl(void* result) = 0;

    // 执行 SQL 语句（用于建表、INSERT）
    virtual int ExecuteSqlImpl(const char* sql, std::string* error) = 0;

    // 事务管理
    virtual int BeginTransactionImpl(std::string* error) = 0;
    virtual int CommitTransactionImpl(std::string* error) = 0;
    virtual int RollbackTransactionImpl(std::string* error) = 0;

public:
    // ===== 基类提供的模板方法（子类无需重写）=====

    int CreateReader(const char* query, IBatchReader** reader) override {
        std::string error;

        // 1. 执行查询
        void* result = ExecuteQueryImpl(query, &error);
        if (!result) {
            last_error_ = error;
            return -1;
        }

        // 2. 推断 Schema
        auto schema = InferSchemaImpl(result, &error);
        if (!schema) {
            last_error_ = error;
            FreeResultImpl(result);
            return -1;
        }

        // 3. 创建通用 Reader
        *reader = new SqlBatchReader(this, result, schema);
        return 0;
    }

    int CreateWriter(const char* table, IBatchWriter** writer) override {
        *writer = new SqlBatchWriter(this, table);
        return 0;
    }

protected:
    std::string last_error_;
};
```

**设计要点**：
- `void*` 类型擦除：牺牲类型安全换取接口统一
- 模板方法模式：基类控制流程，子类实现细节
- 错误处理统一：通过 `std::string* error` 参数传递错误信息

---

### 2. SqlBatchReader 通用读取器

**职责**：封装 Arrow builders 创建、RecordBatch 构建、IPC 序列化。

**关键方法**：

```cpp
class SqlBatchReader : public IBatchReader {
public:
    SqlBatchReader(SqlDbDriver* driver, void* result,
                   std::shared_ptr<arrow::Schema> schema);

    int Next(const uint8_t** buf, size_t* len) override {
        if (done_ || cancelled_) return 1;

        // 1. 创建 Arrow builders（基类封装）
        int ncols = schema_->num_fields();
        std::vector<std::unique_ptr<arrow::ArrayBuilder>> builders(ncols);
        for (int i = 0; i < ncols; ++i) {
            arrow::MakeBuilder(arrow::default_memory_pool(),
                              schema_->field(i)->type(), &builders[i]);
        }

        // 2. 读取 batch_size_ 行（调用子类的 FetchRowImpl）
        int rows = 0;
        while (rows < batch_size_ && !cancelled_) {
            std::string error;
            int rc = driver_->FetchRowImpl(result_, builders, &error);
            if (rc == 1) {  // 无更多数据
                done_ = true;
                break;
            }
            if (rc == -1) {  // 错误
                last_error_ = error;
                return -1;
            }
            ++rows;
        }

        if (rows == 0) return 1;

        // 3. 构建 RecordBatch（基类封装）
        std::vector<std::shared_ptr<arrow::Array>> arrays(ncols);
        for (int i = 0; i < ncols; ++i) {
            builders[i]->Finish(&arrays[i]);
        }
        auto batch = arrow::RecordBatch::Make(schema_, rows, arrays);

        // 4. IPC 序列化（基类封装）
        auto sink = arrow::io::BufferOutputStream::Create().ValueOrDie();
        auto writer = arrow::ipc::MakeStreamWriter(sink, schema_).ValueOrDie();
        writer->WriteRecordBatch(*batch);
        writer->Close();
        auto buffer = sink->Finish().ValueOrDie();

        batch_buf_.assign(reinterpret_cast<const char*>(buffer->data()),
                         buffer->size());
        *buf = reinterpret_cast<const uint8_t*>(batch_buf_.data());
        *len = batch_buf_.size();
        return 0;
    }

private:
    SqlDbDriver* driver_;
    void* result_;
    std::shared_ptr<arrow::Schema> schema_;
    std::string batch_buf_;
    std::string last_error_;
    bool cancelled_ = false;
    bool done_ = false;
    int batch_size_ = 1024;
};
```

---

### 3. MysqlDriver 实现

**职责**：实现 MySQL 特定的钩子方法。

**关键方法**：

```cpp
class MysqlDriver : public SqlDbDriver {
public:
    int Connect(const std::unordered_map<std::string, std::string>& params) override {
        conn_ = mysql_init(nullptr);
        const char* host = params.at("host").c_str();
        const char* user = params.at("user").c_str();
        const char* password = params.at("password").c_str();
        const char* database = params.at("database").c_str();

        if (!mysql_real_connect(conn_, host, user, password, database,
                               0, nullptr, 0)) {
            last_error_ = mysql_error(conn_);
            return -1;
        }
        return 0;
    }

protected:
    void* ExecuteQueryImpl(const char* sql, std::string* error) override {
        MYSQL_STMT* stmt = mysql_stmt_init(conn_);
        if (mysql_stmt_prepare(stmt, sql, strlen(sql)) != 0) {
            *error = mysql_stmt_error(stmt);
            mysql_stmt_close(stmt);
            return nullptr;
        }
        if (mysql_stmt_execute(stmt) != 0) {
            *error = mysql_stmt_error(stmt);
            mysql_stmt_close(stmt);
            return nullptr;
        }
        return stmt;  // 返回 MYSQL_STMT*
    }

    std::shared_ptr<arrow::Schema> InferSchemaImpl(
        void* result, std::string* error) override {
        MYSQL_STMT* stmt = static_cast<MYSQL_STMT*>(result);
        MYSQL_RES* meta = mysql_stmt_result_metadata(stmt);
        if (!meta) {
            *error = "no result metadata";
            return nullptr;
        }

        unsigned int num_fields = mysql_num_fields(meta);
        std::vector<std::shared_ptr<arrow::Field>> fields;

        for (unsigned int i = 0; i < num_fields; ++i) {
            MYSQL_FIELD* field = mysql_fetch_field_direct(meta, i);

            // MySQL 类型 → Arrow 类型映射
            std::shared_ptr<arrow::DataType> arrow_type;
            switch (field->type) {
                case MYSQL_TYPE_TINY:
                case MYSQL_TYPE_SHORT:
                case MYSQL_TYPE_LONG:
                case MYSQL_TYPE_LONGLONG:
                    arrow_type = arrow::int64();
                    break;
                case MYSQL_TYPE_FLOAT:
                case MYSQL_TYPE_DOUBLE:
                    arrow_type = arrow::float64();
                    break;
                default:
                    arrow_type = arrow::utf8();
                    break;
            }

            fields.push_back(arrow::field(field->name, arrow_type));
        }

        mysql_free_result(meta);
        return arrow::schema(fields);
    }

    int FetchRowImpl(
        void* result,
        const std::vector<std::unique_ptr<arrow::ArrayBuilder>>& builders,
        std::string* error) override {
        MYSQL_STMT* stmt = static_cast<MYSQL_STMT*>(result);

        int rc = mysql_stmt_fetch(stmt);
        if (rc == MYSQL_NO_DATA) return 1;  // 无更多数据
        if (rc != 0) {
            *error = mysql_stmt_error(stmt);
            return -1;
        }

        // 通过 MYSQL_BIND 读取列值，填充到 builders
        // （具体实现见代码）

        return 0;
    }

    void FreeResultImpl(void* result) override {
        MYSQL_STMT* stmt = static_cast<MYSQL_STMT*>(result);
        mysql_stmt_close(stmt);
    }

    int ExecuteSqlImpl(const char* sql, std::string* error) override {
        if (mysql_query(conn_, sql) != 0) {
            *error = mysql_error(conn_);
            return -1;
        }
        return 0;
    }

    int BeginTransactionImpl(std::string* error) override {
        return ExecuteSqlImpl("BEGIN", error);
    }

    int CommitTransactionImpl(std::string* error) override {
        return ExecuteSqlImpl("COMMIT", error);
    }

    int RollbackTransactionImpl(std::string* error) override {
        return ExecuteSqlImpl("ROLLBACK", error);
    }

private:
    MYSQL* conn_ = nullptr;
};
```

---

### 4. ConnectionPool 连接池

**职责**：管理数据库连接的复用和生命周期。

**关键方法**：

```cpp
class ConnectionPool {
public:
    struct Config {
        int max_connections = 10;      // 最大连接数
        int idle_timeout_sec = 300;    // 空闲超时（秒）
    };

    ConnectionPool(const Config& config) : config_(config) {}

    // 获取连接（如果池中有空闲连接则复用，否则创建新连接）
    IDbDriver* Acquire(const std::string& key,
                      std::function<IDbDriver*()> factory) {
        std::unique_lock<std::mutex> lock(mutex_);

        auto& pool = pools_[key];

        // 1. 尝试从池中获取空闲连接
        while (!pool.idle.empty()) {
            auto conn = pool.idle.front();
            pool.idle.pop();

            // 检查连接是否超时
            auto now = std::chrono::steady_clock::now();
            auto idle_time = std::chrono::duration_cast<std::chrono::seconds>(
                now - conn.last_used).count();

            if (idle_time < config_.idle_timeout_sec) {
                // 连接有效，返回
                pool.active.insert(conn.driver);
                return conn.driver;
            } else {
                // 连接超时，释放
                conn.driver->Disconnect();
                delete conn.driver;
            }
        }

        // 2. 池中无空闲连接，检查是否达到最大连接数
        if (pool.active.size() >= config_.max_connections) {
            // 达到上限，等待其他连接释放
            // （简化实现：直接返回 nullptr，生产环境应使用条件变量等待）
            return nullptr;
        }

        // 3. 创建新连接
        IDbDriver* driver = factory();
        if (driver) {
            pool.active.insert(driver);
        }
        return driver;
    }

    // 释放连接（归还到池中）
    void Release(const std::string& key, IDbDriver* driver) {
        std::unique_lock<std::mutex> lock(mutex_);

        auto& pool = pools_[key];
        pool.active.erase(driver);

        Connection conn;
        conn.driver = driver;
        conn.last_used = std::chrono::steady_clock::now();
        pool.idle.push(conn);
    }

private:
    struct Connection {
        IDbDriver* driver;
        std::chrono::steady_clock::time_point last_used;
    };

    struct Pool {
        std::set<IDbDriver*> active;       // 活跃连接
        std::queue<Connection> idle;       // 空闲连接
    };

    Config config_;
    std::mutex mutex_;
    std::map<std::string, Pool> pools_;  // key: type.name
};
```

---

## SQL 高级特性设计

### SqlParser 扩展

**目标**：识别 SQL 高级特性关键字，完整 SQL 透传给数据库引擎。

**关键字识别**：
- JOIN 相关：`INNER JOIN`, `LEFT JOIN`, `RIGHT JOIN`, `FULL JOIN`, `ON`
- 聚合相关：`GROUP BY`, `HAVING`, `COUNT`, `SUM`, `AVG`, `MAX`, `MIN`
- 排序相关：`ORDER BY`, `ASC`, `DESC`, `LIMIT`, `OFFSET`
- 集合操作：`UNION`, `INTERSECT`, `EXCEPT`
- 子查询：识别括号嵌套

**实现方式**：
```cpp
class SqlParser {
    // 新增方法：检查 SQL 是否包含高级特性
    bool HasAdvancedFeatures(const char* sql) {
        // 简单检查：是否包含关键字
        return strstr(sql, "JOIN") || strstr(sql, "GROUP BY") ||
               strstr(sql, "ORDER BY") || strstr(sql, "UNION");
    }

    // 修改 BuildQuery：如果包含高级特性，直接返回原 SQL
    std::string BuildQuery(const SqlStatement& stmt) {
        if (HasAdvancedFeatures(stmt.source.c_str())) {
            // 高级特性：直接返回原 SQL
            return stmt.source;
        } else {
            // 简单查询：拼接 SELECT + WHERE
            std::string sql = "SELECT ";
            if (stmt.columns.empty()) {
                sql += "*";
            } else {
                for (size_t i = 0; i < stmt.columns.size(); ++i) {
                    if (i > 0) sql += ", ";
                    sql += stmt.columns[i];
                }
            }
            sql += " FROM " + ExtractTableName(stmt.source);
            if (!stmt.where_clause.empty()) {
                sql += " WHERE " + stmt.where_clause;
            }
            return sql;
        }
    }
};
```

---

## 依赖关系

### 第三方库

| 库 | 版本 | 用途 |
|----|------|------|
| libmysqlclient | 8.0+ | MySQL 客户端库 |
| Arrow | 已集成 | 数据交换格式 |

### CMake 配置

```cmake
# 可选依赖：MySQL
option(ENABLE_MYSQL "Enable MySQL driver support" OFF)

if(ENABLE_MYSQL)
    find_package(MySQL REQUIRED)
    add_definitions(-DENABLE_MYSQL)
    target_link_libraries(flowsql_database PRIVATE MySQL::MySQL)
endif()
```

---

## 风险与缓解

### 风险 1：void* 类型擦除

**风险描述**：`void*` 失去类型安全，可能导致类型转换错误。

**缓解措施**：
- 在钩子方法文档中明确 `void*` 的实际类型
- 使用 `static_cast` 而非 `reinterpret_cast`
- 单元测试覆盖所有钩子方法
- 代码审查时重点检查类型转换

### 风险 2：连接池并发安全

**风险描述**：多线程环境下连接池可能出现竞态条件。

**缓解措施**：
- 使用 `std::mutex` 保护共享数据
- 单元测试覆盖并发场景
- 压力测试验证线程安全

### 风险 3：SQL 解析器复杂度

**风险描述**：识别 SQL 高级特性可能导致解析器过于复杂。

**缓解措施**：
- 只识别关键字，不解析语法树
- 复杂 SQL 直接透传给数据库引擎
- 保持解析器简单，避免重复造轮子

---

## 性能考虑

### 连接池性能

**优化点**：
- 连接复用减少连接建立开销
- 空闲超时回收避免资源浪费
- 最大连接数限制防止资源耗尽

**性能指标**：
- 连接获取延迟：< 1ms（池中有空闲连接）
- 连接复用率：> 80%（高并发场景）

### Arrow 序列化性能

**优化点**：
- 批量读取（batch_size = 1024）减少序列化次数
- IPC 零拷贝传输

**性能指标**：
- 序列化吞吐量：> 100MB/s
- 内存占用：< 2x 数据大小

---

## 测试策略

### 单元测试

- `test_sql_db_driver.cpp`：测试 SqlDbDriver 基类
- `test_mysql_driver.cpp`：测试 MysqlDriver 实现
- `test_connection_pool.cpp`：测试连接池功能
- `test_sql_parser_advanced.cpp`：测试 SQL 高级特性解析

### 集成测试

- `test_mysql_e2e.cpp`：MySQL 端到端测试
- `test_connection_pool_e2e.cpp`：连接池端到端测试

### 性能测试

- `benchmark_connection_pool.cpp`：连接池性能测试
- `benchmark_mysql_driver.cpp`：MySQL 驱动性能测试

---

## 参考资料

- [MySQL C API Documentation](https://dev.mysql.com/doc/c-api/8.0/en/)
- [Apache Arrow IPC Format](https://arrow.apache.org/docs/format/Columnar.html#ipc-streaming-format)
- [Design Patterns: Template Method](https://refactoring.guru/design-patterns/template-method)
