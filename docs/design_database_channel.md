# DatabaseChannel 多数据库通道能力设计文档

## 1. 背景与目标

### 1.1 当前状态
- IDatabaseChannel/IBatchReader/IBatchWriter 接口已定义
- ChannelAdapter 已实现 Database ↔ DataFrame 自动适配
- Scheduler 的类型感知执行路径已就绪
- 缺少具体的数据库通道实现

### 1.2 设计目标
- 支持多种数据库：SQLite、MySQL、ClickHouse（可扩展）
- 统一的接口抽象，最小化重复代码
- 懒加载连接，按需创建通道
- 通过配置驱动，无需修改代码即可接入新数据库实例

## 2. 架构设计

### 2.1 整体架构

```
┌─────────────────────────────────────────────────────────────┐
│                         Scheduler                            │
│  FindChannel("sqlite.mydb")                                  │
│         ↓                                                    │
│  IQuerier::First(IID_DATABASE_FACTORY)                       │
│         ↓                                                    │
│  factory->Get("sqlite", "mydb")                              │
└─────────────────────────────────────────────────────────────┘
                          ↓
┌─────────────────────────────────────────────────────────────┐
│              DatabasePlugin (工厂实现)                        │
│  - IPlugin: 插件生命周期管理                                  │
│  - IDatabaseFactory: 通道工厂接口                             │
│                                                              │
│  configs_: map<string, map<string, string>>                  │
│    └─ "sqlite.mydb" → {path="/data/test.db", ...}           │
│                                                              │
│  channels_: map<string, shared_ptr<DatabaseChannel>>         │
│    └─ "sqlite.mydb" → DatabaseChannel 实例                   │
│                                                              │
│  CreateDriver(type) → IDbDriver 实例                         │
└─────────────────────────────────────────────────────────────┘
                          ↓
┌─────────────────────────────────────────────────────────────┐
│              DatabaseChannel (通道实现)                       │
│  - IDatabaseChannel: 通道接口                                │
│  - 持有 unique_ptr<IDbDriver>                                │
│  - 委托所有数据库操作给 driver                                │
│                                                              │
│  Open() → driver_->Connect(params_)                          │
│  CreateReader() → driver_->CreateReader()                    │
│  CreateWriter() → driver_->CreateWriter()                    │
└─────────────────────────────────────────────────────────────┘
                          ↓
┌─────────────────────────────────────────────────────────────┐
│                IDbDriver (驱动抽象)                           │
│  - Connect/Disconnect/IsConnected                            │
│  - CreateReader/CreateWriter                                 │
│                                                              │
│  ┌──────────────┐  ┌──────────────┐  ┌──────────────┐      │
│  │SqliteDriver  │  │MysqlDriver   │  │ClickhouseDriver│     │
│  │- sqlite3* db │  │- MYSQL* conn │  │- httplib     │      │
│  │- 类型映射    │  │- 类型映射    │  │- ArrowStream │      │
│  └──────────────┘  └──────────────┘  └──────────────┘      │
└─────────────────────────────────────────────────────────────┘
```

### 2.2 分层职责

| 层次 | 组件 | 职责 |
|------|------|------|
| **工厂层** | DatabasePlugin | 配置管理、通道池管理、驱动工厂、懒加载 |
| **通道层** | DatabaseChannel | 通道身份、生命周期、委托数据库操作 |
| **驱动层** | IDbDriver 实现 | 连接管理、SQL 执行、类型映射、客户端库调用 |

## 3. 核心接口设计

### 3.1 IDatabaseFactory（工厂接口）

```cpp
// src/framework/interfaces/idatabase_factory.h
#ifndef _FLOWSQL_FRAMEWORK_INTERFACES_IDATABASE_FACTORY_H_
#define _FLOWSQL_FRAMEWORK_INTERFACES_IDATABASE_FACTORY_H_

#include <common/typedef.h>
#include <functional>

namespace flowsql {

constexpr Guid IID_DATABASE_FACTORY = {0xa9b8c7d6, 0xe5f4, 0x3210,
                                       {0xfe, 0xdc, 0xba, 0x98, 0x76, 0x54, 0x32, 0x10}};

interface IDatabaseFactory {
    virtual ~IDatabaseFactory() = default;

    // 获取或创建数据库通道实例（懒加载）
    // type: "sqlite", "mysql", "clickhouse"
    // name: 通道名称（如 "mydb"）
    // 返回: 通道指针（工厂持有所有权），失败返回 nullptr
    virtual IDatabaseChannel* Get(const char* type, const char* name) = 0;

    // [预留] 获取数据库通道（带用户上下文）
    // 未来实现权限管理时，可根据 user_context 检查权限或选择凭证
    // 当前默认实现：忽略 user_context，调用 Get(type, name)
    virtual IDatabaseChannel* GetWithContext(const char* type,
                                             const char* name,
                                             const char* user_context) {
        return Get(type, name);
    }

    // 列出所有已配置的数据库连接
    virtual void List(std::function<void(const char* type, const char* name)> callback) = 0;

    // 释放指定通道（关闭连接，从池中移除）
    virtual int Release(const char* type, const char* name) = 0;

    // 获取最近一次操作的错误信息（线程安全：内部使用 thread_local 存储）
    virtual const char* LastError() = 0;
};

}  // namespace flowsql

#endif
```

### 3.2 IDbDriver（驱动抽象）

```cpp
// src/services/database/idb_driver.h
#ifndef _FLOWSQL_SERVICES_DATABASE_IDB_DRIVER_H_
#define _FLOWSQL_SERVICES_DATABASE_IDB_DRIVER_H_

#include <framework/interfaces/idatabase_channel.h>
#include <string>
#include <unordered_map>

namespace flowsql::database {

interface IDbDriver {
    virtual ~IDbDriver() = default;

    // 连接管理
    virtual int Connect(const std::unordered_map<std::string, std::string>& params) = 0;
    virtual int Disconnect() = 0;
    virtual bool IsConnected() = 0;

    // 数据操作（创建 Reader/Writer，生命周期由调用者管理）
    virtual int CreateReader(const char* query, IBatchReader** reader) = 0;
    virtual int CreateWriter(const char* table, IBatchWriter** writer) = 0;

    // 元数据
    virtual const char* DriverName() = 0;  // "sqlite", "mysql", "clickhouse"
    virtual const char* LastError() = 0;
};

}  // namespace flowsql::database

#endif
```

### 3.3 DatabaseChannel（通道实现）

```cpp
// src/services/database/database_channel.h
#ifndef _FLOWSQL_SERVICES_DATABASE_DATABASE_CHANNEL_H_
#define _FLOWSQL_SERVICES_DATABASE_DATABASE_CHANNEL_H_

#include <framework/interfaces/idatabase_channel.h>
#include "idb_driver.h"
#include <memory>
#include <string>
#include <unordered_map>

namespace flowsql::database {

class DatabaseChannel : public IDatabaseChannel {
public:
    DatabaseChannel(const std::string& type, const std::string& name,
                    std::unique_ptr<IDbDriver> driver,
                    const std::unordered_map<std::string, std::string>& params);
    ~DatabaseChannel() override;

    // IChannel
    const char* Catelog() override { return type_.c_str(); }
    const char* Name() override { return name_.c_str(); }
    const char* Type() override { return "database"; }
    const char* Schema() override { return "[]"; }  // TODO: 后续可查询数据库元数据返回表列表

    int Open() override;
    int Close() override;
    bool IsOpened() const override { return opened_; }
    int Flush() override { return 0; }

    // IDatabaseChannel（委托给 driver_）
    int CreateReader(const char* query, IBatchReader** reader) override;
    int CreateWriter(const char* table, IBatchWriter** writer) override;
    bool IsConnected() override;

private:
    std::string type_;   // "sqlite", "mysql", "clickhouse"
    std::string name_;   // 通道名称
    std::unique_ptr<IDbDriver> driver_;
    std::unordered_map<std::string, std::string> params_;
    bool opened_ = false;
};

}  // namespace flowsql::database

#endif
```

**实现要点**：
```cpp
int DatabaseChannel::Open() {
    if (opened_) return 0;
    int ret = driver_->Connect(params_);
    if (ret == 0) opened_ = true;
    return ret;
}

int DatabaseChannel::CreateReader(const char* query, IBatchReader** reader) {
    if (!opened_) return -1;
    return driver_->CreateReader(query, reader);
}
```

### 3.4 DatabasePlugin（工厂实现）

```cpp
// src/services/database/database_plugin.h
#ifndef _FLOWSQL_SERVICES_DATABASE_DATABASE_PLUGIN_H_
#define _FLOWSQL_SERVICES_DATABASE_DATABASE_PLUGIN_H_

#include <common/iplugin.h>
#include <framework/interfaces/idatabase_factory.h>
#include "database_channel.h"
#include <memory>
#include <mutex>
#include <unordered_map>

namespace flowsql::database {

class DatabasePlugin : public IPlugin, public IDatabaseFactory {
public:
    DatabasePlugin() = default;
    ~DatabasePlugin() override = default;

    // IPlugin
    int Option(const char* arg) override;
    int Load(IQuerier* querier) override;
    int Unload() override;
    int Start() override { return 0; }
    int Stop() override;

    // IDatabaseFactory
    IDatabaseChannel* Get(const char* type, const char* name) override;
    IDatabaseChannel* GetWithContext(const char* type, const char* name,
                                     const char* user_context) override;
    void List(std::function<void(const char* type, const char* name)> callback) override;
    int Release(const char* type, const char* name) override;
    const char* LastError() override;

private:
    std::unique_ptr<IDbDriver> CreateDriver(const std::string& type);

    // 通道池：key = "type.name"（如 "sqlite.mydb"）
    std::unordered_map<std::string, std::shared_ptr<DatabaseChannel>> channels_;

    // 配置表：key = "type.name", value = 连接参数
    std::unordered_map<std::string, std::unordered_map<std::string, std::string>> configs_;

    std::mutex mutex_;
    IQuerier* querier_ = nullptr;

    // 线程安全的错误信息存储
    static thread_local std::string last_error_;
};

}  // namespace flowsql::database

#endif
```

**关键实现**：

```cpp
// Option 解析配置
int DatabasePlugin::Option(const char* arg) {
    // 解析 "type=sqlite;name=mydb;path=/data/test.db"
    std::unordered_map<std::string, std::string> params;
    // ... 解析逻辑 ...

    std::string type = params["type"];
    std::string name = params["name"];
    std::string key = type + "." + name;

    std::lock_guard<std::mutex> lock(mutex_);
    configs_[key] = std::move(params);
    return 0;
}

// 懒加载获取通道（含断线重连）
IDatabaseChannel* DatabasePlugin::Get(const char* type, const char* name) {
    std::lock_guard<std::mutex> lock(mutex_);

    std::string key = std::string(type) + "." + name;

    // 1. 查找已存在的通道
    auto it = channels_.find(key);
    if (it != channels_.end()) {
        // 检查连接是否仍然有效，断开则移除并重建
        if (!it->second->IsConnected()) {
            printf("DatabasePlugin: connection lost for %s, reconnecting...\n", key.c_str());
            channels_.erase(it);
        } else {
            return it->second.get();
        }
    }

    // 2. 查找配置
    auto cfg_it = configs_.find(key);
    if (cfg_it == configs_.end()) {
        last_error_ = "database not configured: " + key;
        return nullptr;  // 未配置
    }

    // 3. 创建驱动
    auto driver = CreateDriver(type);
    if (!driver) {
        last_error_ = "unsupported database type: " + std::string(type);
        return nullptr;  // 不支持的数据库类型
    }

    // 4. 创建通道
    auto channel = std::make_shared<DatabaseChannel>(
        type, name, std::move(driver), cfg_it->second);

    // 5. 打开连接
    if (channel->Open() != 0) {
        last_error_ = "connection failed: " + key;
        return nullptr;  // 连接失败
    }

    // 6. 加入通道池
    channels_[key] = channel;
    return channel.get();
}

const char* DatabasePlugin::LastError() {
    return last_error_.c_str();
}

// 驱动工厂
std::unique_ptr<IDbDriver> DatabasePlugin::CreateDriver(const std::string& type) {
    if (type == "sqlite")     return std::make_unique<SqliteDriver>();
    if (type == "mysql")      return std::make_unique<MysqlDriver>();
    if (type == "clickhouse") return std::make_unique<ClickhouseDriver>();
    return nullptr;
}
```

## 4. SQLite 驱动实现（第一个实现）

### 4.1 SqliteDriver

```cpp
// src/services/database/drivers/sqlite_driver.h
#ifndef _FLOWSQL_SERVICES_DATABASE_DRIVERS_SQLITE_DRIVER_H_
#define _FLOWSQL_SERVICES_DATABASE_DRIVERS_SQLITE_DRIVER_H_

#include "../idb_driver.h"
#include <sqlite3.h>
#include <string>

namespace flowsql::database {

class SqliteDriver : public IDbDriver {
public:
    SqliteDriver() = default;
    ~SqliteDriver() override;

    int Connect(const std::unordered_map<std::string, std::string>& params) override;
    int Disconnect() override;
    bool IsConnected() override { return db_ != nullptr; }

    int CreateReader(const char* query, IBatchReader** reader) override;
    int CreateWriter(const char* table, IBatchWriter** writer) override;

    const char* DriverName() override { return "sqlite"; }
    const char* LastError() override { return last_error_.c_str(); }

private:
    sqlite3* db_ = nullptr;
    std::string db_path_;
    std::string last_error_;
};

}  // namespace flowsql::database

#endif
```

**Connect 实现**：
```cpp
int SqliteDriver::Connect(const std::unordered_map<std::string, std::string>& params) {
    if (db_) return 0;  // 已连接

    auto it = params.find("path");
    db_path_ = (it != params.end()) ? it->second : ":memory:";

    // 使用 FULLMUTEX 模式，保证多线程安全
    int flags = SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE | SQLITE_OPEN_FULLMUTEX;

    // 支持只读模式
    auto ro_it = params.find("readonly");
    if (ro_it != params.end() && ro_it->second == "true") {
        flags = SQLITE_OPEN_READONLY | SQLITE_OPEN_FULLMUTEX;
    }

    int rc = sqlite3_open_v2(db_path_.c_str(), &db_, flags, nullptr);
    if (rc != SQLITE_OK) {
        last_error_ = sqlite3_errmsg(db_);
        sqlite3_close(db_);
        db_ = nullptr;
        return -1;
    }

    // 开启 WAL 模式，提升并发读写性能
    sqlite3_exec(db_, "PRAGMA journal_mode=WAL;", nullptr, nullptr, nullptr);
    return 0;
}
```

**并发模型**：
- SQLite 使用 `SQLITE_OPEN_FULLMUTEX`（serialized 模式），保证同一连接的多线程安全
- WAL 模式允许并发读取，写入仍然串行
- 对于 MySQL/ClickHouse 等 client/server 数据库，连接本身是线程安全的

### 4.2 SqliteBatchReader

```cpp
class SqliteBatchReader : public IBatchReader {
public:
    SqliteBatchReader(sqlite3_stmt* stmt, std::shared_ptr<arrow::Schema> schema);
    ~SqliteBatchReader() override;

    int GetSchema(const uint8_t** buf, size_t* len) override;
    int Next(const uint8_t** buf, size_t* len) override;
    void Cancel() override { cancelled_ = true; }
    void Close() override;
    const char* GetLastError() override { return last_error_.c_str(); }
    void Release() override { delete this; }

private:
    sqlite3_stmt* stmt_ = nullptr;
    std::shared_ptr<arrow::Schema> schema_;
    std::string schema_buf_;   // Arrow Schema IPC 序列化缓存
    std::string batch_buf_;    // Arrow RecordBatch IPC 序列化缓存
    std::string last_error_;
    bool cancelled_ = false;
    int batch_size_ = 1024;    // 每批行数
};
```

**Next 实现要点**：
1. 循环调用 `sqlite3_step(stmt_)`，积累 `batch_size_` 行
2. 根据 `schema_` 中的列类型，调用 `sqlite3_column_xxx` 读取值
3. 构建 `arrow::RecordBatch`
4. 序列化为 Arrow IPC stream 格式写入 `batch_buf_`
5. 返回 `*buf = batch_buf_.data()`, `*len = batch_buf_.size()`
6. 如果 `sqlite3_step` 返回 `SQLITE_DONE` 且本批无数据，返回 1（已读完）

**类型映射**：
| SQLite Type | Arrow Type |
|-------------|------------|
| SQLITE_INTEGER | Int64 |
| SQLITE_FLOAT | Double |
| SQLITE_TEXT | Utf8 |
| SQLITE_BLOB | Binary |
| SQLITE_NULL | Null |

### 4.3 SqliteBatchWriter

```cpp
class SqliteBatchWriter : public IBatchWriter {
public:
    SqliteBatchWriter(sqlite3* db, const std::string& table);
    ~SqliteBatchWriter() override;

    int Write(const uint8_t* buf, size_t len) override;
    int Flush() override;
    void Close(BatchWriteStats* stats) override;
    const char* GetLastError() override { return last_error_.c_str(); }
    void Release() override { delete this; }

private:
    int CreateTable(const arrow::Schema& schema);
    int InsertBatch(const arrow::RecordBatch& batch);

    sqlite3* db_ = nullptr;
    std::string table_;
    std::string last_error_;
    int64_t rows_written_ = 0;
    int64_t bytes_written_ = 0;
    std::chrono::steady_clock::time_point start_time_;
    bool table_created_ = false;
};
```

**Write 实现要点**：
1. 反序列化 Arrow IPC stream → `RecordBatch`
2. 首次写入时：根据 `RecordBatch->schema()` 自动 `CREATE TABLE IF NOT EXISTS`
3. 构建参数化 INSERT：`INSERT INTO table (col1, col2, ...) VALUES (?, ?, ...)`
4. 使用 `BEGIN/COMMIT` 事务包裹批量插入
5. 逐行 `sqlite3_bind_xxx` + `sqlite3_step`

**类型映射**：
| Arrow Type | SQLite Affinity |
|------------|-----------------|
| Int32/Int64/UInt32/UInt64 | INTEGER |
| Float/Double | REAL |
| Utf8 | TEXT |
| Binary | BLOB |
| Boolean | INTEGER |

## 5. Scheduler 集成

### 5.1 FindChannel 改造

```cpp
// src/services/scheduler/scheduler_plugin.cpp
IChannel* SchedulerPlugin::FindChannel(const std::string& name) {
    // 第一层：精确查找内部表
    auto it = channels_.find(name);
    if (it != channels_.end()) return it->second.get();

    // 第二层：按 catelog.name 格式拆分查找
    // ... 现有逻辑 ...

    // 第三层：通过 IQuerier 遍历静态注册的通道
    // ... 现有逻辑 ...

    // 【新增】第四层：尝试通过 IDatabaseFactory 获取数据库通道
    auto pos = name.find('.');
    if (pos != std::string::npos) {
        std::string type = name.substr(0, pos);
        std::string ch_name = name.substr(pos + 1);

        auto factory = static_cast<IDatabaseFactory*>(
            querier_->First(IID_DATABASE_FACTORY));
        if (factory) {
            auto db_ch = factory->Get(type.c_str(), ch_name.c_str());
            if (db_ch) return db_ch;
        }
    }

    // 第五层：模糊匹配内部通道表
    // ... 现有逻辑 ...

    return nullptr;
}
```

### 5.2 SQL 使用示例

```sql
-- Database → DataFrame（三段式）
SELECT * FROM sqlite.mydb.users INTO result

-- DataFrame → Database（三段式，自动建表）
SELECT * FROM test.data INTO sqlite.mydb.users

-- Database + WHERE → 算子 → DataFrame
SELECT col1, col2 FROM sqlite.mydb.users WHERE age>18 USING explore.chisquare WITH threshold=0.05

-- DataFrame + WHERE → Database
SELECT * FROM test.data WHERE key=1 INTO sqlite.mydb.users

-- 复杂 WHERE 条件
SELECT * FROM mysql.prod.orders WHERE status='pending' AND amount>1000

-- Database → Database（跨数据库）
SELECT * FROM sqlite.db1.users WHERE age>18 INTO mysql.db2.customers
```

## 6. 配置方式

### 6.1 配置格式

通过 `Option()` 传入，格式：`type=sqlite;name=mydb;path=/data/test.db`

```yaml
# config/gateway.yaml
- name: scheduler
  plugins:
    - libflowsql_scheduler.so
    - libflowsql_bridge.so
    - libflowsql_example.so
    - libflowsql_database.so:type=sqlite;name=testdb;path=/data/test.db
    - libflowsql_database.so:type=sqlite;name=memdb;path=:memory:
    - libflowsql_database.so:type=mysql;name=prod;host=127.0.0.1;port=3306;user=root;password=xxx;database=mydb
```

### 6.2 配置参数

| 参数 | SQLite | MySQL | ClickHouse |
|------|--------|-------|------------|
| type | sqlite | mysql | clickhouse |
| name | 通道名 | 通道名 | 通道名 |
| path | 数据库文件路径 | — | — |
| host | — | 主机地址 | 主机地址 |
| port | — | 3306 | 8123 |
| user | — | 用户名 | 用户名 |
| password | — | 密码 | 密码 |
| database | — | 库名 | 库名 |

## 7. 目录结构

```
src/
├── framework/interfaces/
│   ├── idatabase_factory.h       # [新增] IDatabaseFactory 工厂接口
│   └── ...
├── services/database/
│   ├── idb_driver.h              # IDbDriver 驱动抽象接口
│   ├── database_channel.h        # DatabaseChannel 通用实现
│   ├── database_channel.cpp
│   ├── database_plugin.h         # DatabasePlugin 工厂实现
│   ├── database_plugin.cpp
│   ├── plugin_register.cpp       # 插件注册（IID_PLUGIN + IID_DATABASE_FACTORY）
│   ├── drivers/
│   │   ├── sqlite_driver.h       # SQLite 驱动
│   │   ├── sqlite_driver.cpp
│   │   ├── mysql_driver.h        # [未来] MySQL 驱动
│   │   └── clickhouse_driver.h   # [未来] ClickHouse 驱动
│   └── CMakeLists.txt
└── services/scheduler/
    └── scheduler_plugin.cpp      # [修改] FindChannel 增加 IDatabaseFactory 查找
```

## 8. 实施步骤

### 总体原则

- **可测**：每个阶段都有明确的测试标准
- **可控**：每个阶段独立可编译、可验证
- **增量**：后续阶段不破坏前面阶段的成果

---

### Phase 1：基础框架搭建（2-3 天）

**目标**：建立 DatabaseChannel 的基础架构，不涉及具体数据库实现。

#### 1.1 接口定义
- 创建 `src/framework/interfaces/idatabase_factory.h`
- 定义 `IDatabaseFactory` 接口（包含预留的 `GetWithContext`）
- 定义 `IID_DATABASE_FACTORY` GUID

#### 1.2 驱动抽象
- 创建 `src/services/database/idb_driver.h`
- 定义 `IDbDriver` 接口

#### 1.3 通道实现
- 创建 `src/services/database/database_channel.h/.cpp`
- 实现 `DatabaseChannel` 类（委托给 `IDbDriver`）
- 实现 `Open/Close/CreateReader/CreateWriter` 方法

#### 1.4 工厂实现
- 创建 `src/services/database/database_plugin.h/.cpp`
- 实现 `DatabasePlugin` 类（`IPlugin + IDatabaseFactory`）
- 实现 `Option` 解析、`Get` 方法、驱动工厂
- 实现 `plugin_register.cpp`

#### 1.5 CMake 配置
- 创建 `src/services/database/CMakeLists.txt`
- 更新 `src/CMakeLists.txt` 添加 `add_subdirectory(services/database)`

#### 1.6 单元测试
- 创建 `src/tests/test_database/` 测试目标
- 测试 DatabasePlugin::Option 配置解析
- 测试 DatabasePlugin::Get 在无驱动时返回 nullptr 并设置 LastError
- 测试 DatabasePlugin::List 列出已配置的连接
- 测试 IID_PLUGIN 和 IID_DATABASE_FACTORY 注册

#### 验收标准
```bash
# 编译通过
cmake --build build

# 单元测试通过
./build/output/test_database

# 测试用例：
# 1. Option 解析 "type=sqlite;name=mydb;path=/data/test.db"
# 2. Get("sqlite", "mydb") 返回 nullptr（无 SQLite 驱动）
# 3. LastError() 返回 "unsupported database type: sqlite"
# 4. List 回调被调用
```

**输出**：
- `libflowsql_database.so` 可编译
- 插件可注册 `IID_PLUGIN` 和 `IID_DATABASE_FACTORY`
- 无运行时错误

---

### Phase 2：SQLite 驱动实现（3-4 天）

**目标**：实现第一个数据库驱动，验证整条链路。

#### 2.1 SQLite 驱动
- 创建 `src/services/database/drivers/sqlite_driver.h/.cpp`
- 实现 `SqliteDriver::Connect/Disconnect/IsConnected`
- 实现 `SqliteDriver::CreateReader/CreateWriter`

#### 2.2 SQLite BatchReader
- 实现 `SqliteBatchReader` 类
- 实现 `GetSchema`（序列化 Arrow Schema）
- 实现 `Next`（sqlite3_step → Arrow RecordBatch → IPC 序列化）
- 实现类型映射（SQLite → Arrow）

#### 2.3 SQLite BatchWriter
- 实现 `SqliteBatchWriter` 类
- 实现 `Write`（Arrow IPC 反序列化 → RecordBatch）
- 实现自动建表（`CREATE TABLE IF NOT EXISTS`）
- 实现批量插入（`BEGIN/COMMIT` 事务）

#### 2.4 驱动工厂集成
- 在 `DatabasePlugin::CreateDriver` 中添加 SQLite 分支
- 更新 CMakeLists.txt 链接 SQLite

#### 验收标准
```bash
# 编译通过
cmake --build build

# 单元测试：SQLite 驱动（使用 :memory: 模式，无文件系统副作用）
./build/output/test_database

# 测试用例：
# 1. Connect(:memory:) / Disconnect
# 2. CreateReader 执行 SELECT，验证 Arrow IPC 序列化
# 3. CreateWriter 写入数据，验证自动建表
# 4. 类型映射正确性（INTEGER/REAL/TEXT/BLOB）
# 5. 错误路径：查询语法错误、连接失败（错误路径）
```

**输出**：
- SQLite 驱动可独立测试
- 可以读写 SQLite 数据库
- Arrow IPC 序列化/反序列化正确

---

### Phase 3：Scheduler 集成（2-3 天）

**目标**：将 DatabaseChannel 集成到 Scheduler，支持基本的 Database ↔ DataFrame 操作。

#### 3.1 ParseChannelReference
- 在 `scheduler_plugin.cpp` 中实现 `ParseChannelReference` 函数
- 支持三段式解析（`type.instance.table`）
- 支持二段式解析（`catelog.name`）

#### 3.2 FindChannel 改造
- 修改 `Scheduler::FindChannel`
- 增加第四层查找：通过 `IDatabaseFactory::Get` 获取数据库通道

#### 3.3 SqlStatement 预留 where_clause 字段
- 在 `SqlStatement` 中添加空的 `where_clause` 字段（解析逻辑在 Phase 4 实现）
- 确保 Phase 3 代码可以引用该字段而不报编译错误

#### 3.4 BuildQuery 改造
- 修改 `BuildQuery` 函数签名，接受 `table_name` 参数
- 从 `stmt.where_clause` 读取 WHERE 条件（Phase 3 阶段该字段始终为空，Phase 4 填充）

#### 3.5 ExecuteTransfer 改造
- 修改 `ExecuteTransfer` 签名，增加 `source_table` 和 `dest_table` 参数
- 调用 `BuildQuery(source_table, stmt)` 构建数据库查询
- 调用 `ChannelAdapter::WriteFromDataFrame(src, dst, dest_table.c_str())`

#### 3.6 ExecuteWithOperator 改造
- 修改 `ExecuteWithOperator` 签名，增加 `source_table` 和 `dest_table` 参数
- 适配 Database source/sink

#### 3.7 HandleExecute 改造
- 调用 `ParseChannelReference` 解析 source 和 dest
- 传递 `table_name` 给 `ExecuteTransfer/ExecuteWithOperator`

#### 验收标准
```bash
# 编译通过
cmake --build build

# 启动 Gateway + Scheduler
./build/output/gateway --config config/gateway.yaml

# 集成测试（通过 Web API）：
curl -X POST http://localhost:18801/execute \
  -d '{"sql": "SELECT * FROM test.data INTO sqlite.mydb.users"}'

# 验证：
# 1. DataFrame → SQLite 写入成功
# 2. SQLite 自动建表
# 3. 数据正确写入

curl -X POST http://localhost:18801/execute \
  -d '{"sql": "SELECT * FROM sqlite.mydb.users INTO result"}'

# 验证：
# 1. SQLite → DataFrame 读取成功
# 2. 数据与原始一致
```

**输出**：
- Database ↔ DataFrame 双向传输正常
- 三段式通道名解析正确
- ChannelAdapter 自动适配工作

---

### Phase 4：SQL 解析器扩展（1-2 天）

**目标**：支持 WHERE 子句。可与 Phase 2/3 并行开发。

#### 4.1 SqlParser 扩展
- 在 `SqlStatement` 中增加 `where_clause` 字段
- 实现 WHERE 子句解析（读取 WHERE 到下一个关键字之间的内容）
- 实现 `ValidateWhereClause`（检查危险关键字）

#### 4.2 BuildQuery 使用 WHERE
- 修改 `BuildQuery`，从 `stmt.where_clause` 读取 WHERE 条件
- 拼接到 SQL 查询中

#### 验收标准
```bash
# 单元测试：SqlParser
./build/output/test_sql_parser

# 测试用例：
# 1. 解析 "SELECT * FROM sqlite.mydb.users WHERE age>18"
# 2. where_clause = "age>18"
# 3. 拒绝 "WHERE age>18 DROP TABLE users"

# 集成测试：
curl -X POST http://localhost:18801/execute \
  -d '{"sql": "SELECT * FROM sqlite.mydb.users WHERE age>18 INTO result"}'

# 验证：
# 1. WHERE 下推到数据库
# 2. 只返回过滤后的数据
```

**输出**：
- WHERE 子句正确解析
- WHERE 下推到数据库执行
- SQL 注入防护生效

---

### Phase 5：DataFrame 过滤（2-3 天）

**目标**：支持 DataFrame source 的 WHERE 过滤。可与 Phase 2/3/4 并行开发。

**实现方案**：列式过滤（类似 `df[df[key]>1]` 的语义）
1. 解析条件：提取列名、操作符、值
2. 取出整列数据（`GetColumn(column_name)`）
3. 对整列做比较运算，生成布尔掩码（mask）
4. 用 mask 过滤所有行，生成新的 DataFrame

> 后续如果数据量大到需要 SIMD 加速，可以改用 Arrow Compute kernel（`ToArrow()` → `arrow::compute::Filter`）。

#### 5.1 IDataFrame::Filter 接口
- 在 `idataframe.h` 中增加 `Filter` 方法声明

#### 5.2 DataFrame::Filter 实现
- 在 `dataframe.cpp` 中实现 `Filter` 方法
- 解析简单条件（列名、操作符、值）
- 使用 Arrow Compute 的 Filter kernel
- 支持操作符：`=`, `!=`, `>`, `<`, `>=`, `<=`

#### 5.3 Scheduler 层 WHERE 过滤集成
- 在 `ExecuteTransfer` 和 `ExecuteWithOperator` 中，对 DataFrame source + WHERE 场景调用 `Filter()`
- ChannelAdapter 保持不变，不增加 where_clause 参数

#### 验收标准
```bash
# 单元测试：DataFrame::Filter
./build/output/test_dataframe_filter

# 测试用例：
# 1. Filter("key=1") 正确过滤
# 2. Filter("age>18") 正确过滤
# 3. Filter("invalid_column=1") 返回错误

# 集成测试：
curl -X POST http://localhost:18801/execute \
  -d '{"sql": "SELECT * FROM test.data WHERE key=1 INTO sqlite.mydb.users"}'

# 验证：
# 1. DataFrame 过滤生效
# 2. 只写入过滤后的数据
```

**输出**：
- DataFrame 原生过滤工作
- DataFrame → Database + WHERE 正常

---

### Phase 6：安全基线（1 天）

**目标**：实施基础安全措施。

#### 6.1 环境变量支持
- 在 `DatabasePlugin::Option` 中支持 `${VAR}` 语法
- 从环境变量读取密码

#### 6.2 SQL 注入防护
- 在 `SqlParser::ValidateWhereClause` 中检查危险关键字
- 拒绝包含 DROP/DELETE/UPDATE/INSERT 的 WHERE 子句

#### 6.3 只读模式
- 在 `DatabaseChannel` 中增加 `readonly_` 标志
- 从配置读取 `readonly=true`
- `CreateWriter` 检查只读标志

#### 验收标准
```bash
# 测试：环境变量
export DB_PASSWORD="secret123"
# 配置：password=${DB_PASSWORD}
# 验证：连接成功

# 测试：SQL 注入防护
curl -X POST http://localhost:18801/execute \
  -d '{"sql": "SELECT * FROM sqlite.mydb.users WHERE age>18 DROP TABLE users"}'
# 验证：返回错误，拒绝执行

# 测试：只读模式
# 配置：readonly=true
curl -X POST http://localhost:18801/execute \
  -d '{"sql": "SELECT * FROM test.data INTO sqlite.mydb.users"}'
# 验证：返回错误，拒绝写入
```

**输出**：
- 密码不在配置文件中明文存储
- SQL 注入攻击被拦截
- 只读模式生效

---

### Phase 7：端到端测试与文档（1-2 天）

**目标**：完整测试所有场景，更新文档。

#### 7.1 测试场景覆盖
- Database → DataFrame（无 WHERE）
- Database + WHERE → DataFrame
- DataFrame → Database（无 WHERE）
- DataFrame + WHERE → Database
- DataFrame + WHERE → DataFrame
- Database → Database（跨数据库）
- Database + WHERE → 算子 → DataFrame
- DataFrame + WHERE → 算子 → DataFrame

#### 7.2 错误路径测试
- 连接失败场景（错误的路径/主机）
- 查询语法错误场景
- WHERE 条件列不存在
- SQL 注入防护验证
- 只读模式写入拒绝
- 连接断开后重连

#### 7.3 性能测试
- 批量写入 10 万行数据
- 批量读取 10 万行数据
- WHERE 过滤性能对比（全表 vs 索引）

#### 7.4 文档更新
- 更新 `docs/stage3.md`（标记 P1 完成）
- 更新 `docs/commands.md`（记录实施命令）
- 编写用户手册（SQL 语法、配置示例）

#### 验收标准
```bash
# 所有测试通过
./build/output/test_framework
./build/output/test_database

# 性能测试
./scripts/benchmark_database_channel.sh

# 文档审查
# 1. stage3.md P1 标记为完成
# 2. commands.md 记录完整
# 3. 用户手册清晰
```

**输出**：
- 所有测试通过
- 性能达标
- 文档完整

---

## 9. 阶段依赖关系

```
Phase 1: 基础框架
    ↓
    ├── Phase 2: SQLite 驱动 ← 可独立测试
    │     ↓
    │     Phase 3: Scheduler 集成 ← 依赖 Phase 1 + 2
    │
    ├── Phase 4: WHERE 解析 ← 可与 Phase 2/3 并行
    │
    └── Phase 5: DataFrame 过滤 ← 可与 Phase 2/3/4 并行
          ↓
Phase 6: 安全基线 ← 可与 Phase 5 并行
    ↓
Phase 7: 端到端测试 ← 依赖所有 Phase
```

## 10. 风险控制

### 风险 1：Arrow IPC 序列化复杂

**缓解措施**：
- Phase 2 先实现简单类型（INT64, DOUBLE, UTF8）
- 复杂类型（TIMESTAMP, BINARY）后续迭代

### 风险 2：WHERE 子句解析复杂

**缓解措施**：
- Phase 4 先支持简单条件（单列比较）
- 复杂条件（AND/OR、括号）Phase 2 扩展

### 风险 3：性能不达标

**缓解措施**：
- Phase 2 实现时使用 Arrow Compute kernel（高性能）
- Phase 7 性能测试，发现瓶颈后优化

### 风险 4：多数据库兼容性

**缓解措施**：
- Phase 2 只实现 SQLite（最简单）
- MySQL/ClickHouse 作为 Phase 2 的后续迭代

## 11. 验收标准总结

| 阶段 | 编译 | 单元测试 | 集成测试 | 可并行 |
|------|------|----------|----------|--------|
| Phase 1 | ✓ | ✓ | - | - |
| Phase 2 | ✓ | ✓ | - | - |
| Phase 3 | ✓ | ✓ | ✓ | - |
| Phase 4 | ✓ | ✓ | - | Phase 2/3 |
| Phase 5 | ✓ | ✓ | - | Phase 2/3/4 |
| Phase 6 | ✓ | ✓ | ✓ | Phase 5 |
| Phase 7 | ✓ | ✓ | ✓ | - |

## 12. 验证计划

（详见各 Phase 的验收标准）

## 13. 关键设计决策

### 13.1 为什么需要 IDatabaseFactory？

DatabasePlugin 是工厂角色，不是通道本身。Scheduler 需要数据库通道时，不应该直接遍历静态注册的通道（编译时确定），而应该通过工厂接口动态获取（运行时根据配置创建）。

### 13.2 为什么需要 IDbDriver 抽象层？

- **IDatabaseChannel** 是面向 Scheduler/ChannelAdapter 的接口（Arrow IPC 格式）
- **IDbDriver** 是面向具体数据库的接口（连接管理 + 原生操作）
- 两者职责不同，DatabaseChannel 处理通用逻辑，IDbDriver 处理数据库差异

### 13.3 懒加载 vs 预连接

当前设计采用**懒加载**：
- 配置解析后只存入 `configs_`
- 实际连接在首次 `Get()` 调用时才建立
- 避免启动时连接所有数据库
- 连接失败不影响插件加载

### 13.4 为什么一个 .so 包含所有驱动？

- 共享 DatabaseChannel/DatabasePlugin 代码
- 新增驱动只需加 `drivers/xxx_driver.h/.cpp`
- 部署简单，一个 .so 搞定所有数据库

### 13.5 WHERE 子句处理方案

#### 问题背景

用户需要在数据源端过滤数据：
```sql
SELECT col1, col2 FROM sqlite.mydb.users WHERE age>18 USING explore.chisquare WITH threshold=0.05
SELECT * FROM test.data WHERE key=1 INTO sqlite.mydb.users
```

当前 SQL 解析器不支持 WHERE 子句，临时通过 `WITH where="key=1"` 传递，不符合 SQL 标准。

#### 设计方案：WHERE 下推 + DataFrame 原生过滤

**核心思想**：
- Database source：WHERE 下推到数据库，利用索引和过滤能力
- DataFrame source：DataFrame 原生过滤方法

---

#### 1. 扩展 SQL 解析器

```cpp
// src/framework/core/sql_parser.h
struct SqlStatement {
    std::string source;
    std::vector<std::string> columns;
    std::string where_clause;  // [新增] WHERE 子句（不含 WHERE 关键字）
    std::string op_catelog;
    std::string op_name;
    std::unordered_map<std::string, std::string> with_params;
    std::string dest;
    std::string error;
};

// 语法扩展：
// SELECT [* | col1, col2, ...] FROM <source> [WHERE <condition>]
// [USING <catelog.name>] [WITH key=val,...] [INTO <dest>]
```

**解析顺序**：`SELECT → FROM → WHERE → USING → WITH → INTO`

**解析示例**：
```cpp
// 输入：SELECT col1, col2 FROM sqlite.mydb.users WHERE age>18 USING op WITH threshold=0.05
// 输出：
stmt.source = "sqlite.mydb.users"
stmt.columns = ["col1", "col2"]
stmt.where_clause = "age>18"  // 不含 WHERE 关键字
stmt.op_catelog = "explore"
stmt.op_name = "chisquare"
stmt.with_params = {"threshold": "0.05"}  // WHERE 不再放在 with_params 中
```

---

#### 2. 三段式通道名规范

**Database 通道：强制三段式**
```
<type>.<instance>.<table>
```

**DataFrame 通道：保持二段式**
```
<catelog>.<name>
```

**示例**：
```sql
-- Database 通道（三段式）
SELECT * FROM sqlite.mydb.users WHERE age>18
SELECT * FROM mysql.prod.orders WHERE status='pending'

-- DataFrame 通道（二段式）
SELECT * FROM test.data WHERE key=1
```

**配置**：
```yaml
# 不需要指定表名，表名在 SQL 中显式指定
- libflowsql_database.so:type=sqlite;name=mydb;path=/data/mydb.db
- libflowsql_database.so:type=mysql;name=prod;host=127.0.0.1;database=mydb
```

---

#### 3. ParseChannelReference 函数

```cpp
// src/services/scheduler/scheduler_plugin.cpp

struct ChannelReference {
    std::string channel_name;  // 通道名（用于 FindChannel）
    std::string table_name;    // 表名（仅 Database 通道有效）
};

// 已知的数据库类型前缀（用于区分三段式数据库通道和二段式 DataFrame 通道）
static const std::unordered_set<std::string> kDatabaseTypes = {
    "sqlite", "mysql", "clickhouse", "postgres", "oracle"
};

static ChannelReference ParseChannelReference(const std::string& ref) {
    ChannelReference result;

    auto first_dot = ref.find('.');
    if (first_dot == std::string::npos) {
        // 无点号：非法格式，原样返回
        result.channel_name = ref;
        result.table_name = "";
        return result;
    }

    auto second_dot = ref.find('.', first_dot + 1);

    if (second_dot != std::string::npos) {
        // 有两个点号：检查第一段是否为已知数据库类型
        std::string first_segment = ref.substr(0, first_dot);

        if (kDatabaseTypes.count(first_segment)) {
            // 三段式：type.instance.table（Database 通道）
            std::string instance = ref.substr(first_dot + 1, second_dot - first_dot - 1);
            std::string table = ref.substr(second_dot + 1);

            result.channel_name = first_segment + "." + instance;  // "sqlite.mydb"
            result.table_name = table;                              // "users"
        } else {
            // 第一段不是数据库类型，视为二段式（DataFrame 通道名中包含多个点）
            result.channel_name = ref;
            result.table_name = "";
        }
    } else {
        // 二段式：catelog.name（DataFrame 通道）
        result.channel_name = ref;                     // "test.data"
        result.table_name = "";                        // 空
    }

    return result;
}
```

**设计说明**：
- 通过 `kDatabaseTypes` 集合区分数据库通道和 DataFrame 通道，避免歧义
- DataFrame 通道要求二段式命名（`catelog.name`），不应使用数据库类型名作为 catelog
- 新增数据库类型时，需要同步更新 `kDatabaseTypes` 集合

---

#### 4. DataFrame 原生过滤

**IDataFrame 接口扩展**：
```cpp
// src/framework/interfaces/idataframe.h
interface IDataFrame {
    // ... 现有方法 ...

    // [新增] 原生过滤方法
    // condition: 简单条件表达式，如 "key=1", "age>18", "status='active'"
    // 返回: 0=成功, <0=失败（条件解析错误或列不存在）
    virtual int Filter(const char* condition) = 0;
};
```

**DataFrame 实现要点**：
```cpp
// src/framework/core/dataframe.cpp
int DataFrame::Filter(const char* condition) {
    // 1. 解析条件：提取列名、操作符、值
    //    支持操作符：=, !=, >, <, >=, <=
    //    示例："age>18" → column="age", op=">", value="18"

    // 2. 列式过滤（类似 df[df[key]>1] 语义）
    //    - 取出整列数据：GetColumn(column_name)
    //    - 对整列做比较运算，生成布尔掩码（mask）
    //    - 用 mask 过滤所有行

    // 3. 用过滤后的行重建 DataFrame
    //    - 保留 schema 不变
    //    - 只保留 mask 为 true 的行

    return 0;
}
```

**支持的条件格式**：
- 简单比较：`key=1`, `age>18`, `status='active'`
- 暂不支持：复杂逻辑（AND/OR）、函数调用、括号嵌套（Phase 2 扩展）

---

#### 5. Scheduler 层 WHERE 过滤

**设计原则**：WHERE 过滤在 Scheduler 层完成，ChannelAdapter 保持纯粹的格式转换职责。

- Database source：WHERE 拼入 SQL 查询，下推到数据库执行
- DataFrame source：Scheduler 读取 DataFrame 后调用 `Filter()`，再交给 ChannelAdapter

```cpp
// ChannelAdapter 保持不变，不增加 where_clause 参数
class ChannelAdapter {
public:
    static int WriteFromDataFrame(IDataFrameChannel* df_in,
                                  IDatabaseChannel* db_out,
                                  const char* table);

    static int ReadToDataFrame(IDatabaseChannel* db, const char* query,
                               IDataFrameChannel* df_out);
    static int CopyDataFrame(IDataFrameChannel* src, IDataFrameChannel* dst);
};
```

---

#### 6. Scheduler 执行逻辑改造

**HandleExecute 改造**：
```cpp
int SchedulerPlugin::HandleExecute(const SqlStatement& stmt, ...) {
    // 解析 source 引用
    auto src_ref = ParseChannelReference(stmt.source);
    IChannel* source = FindChannel(src_ref.channel_name);

    // 解析 dest 引用
    auto dst_ref = ParseChannelReference(stmt.dest);
    IChannel* sink = FindChannel(dst_ref.channel_name);

    // 执行
    if (stmt.HasOperator()) {
        return ExecuteWithOperator(source, sink, operator,
                                   source_type, sink_type,
                                   stmt, src_ref.table_name, dst_ref.table_name);
    } else {
        return ExecuteTransfer(source, sink,
                               source_type, sink_type,
                               stmt, src_ref.table_name, dst_ref.table_name);
    }
}
```

**BuildQuery 改造**：
```cpp
// [修改] 接受 table_name 参数，不再从 source_name 提取
static std::string BuildQuery(const std::string& table_name,
                               const SqlStatement& stmt) {
    std::string query = "SELECT ";

    // 列选择
    if (stmt.columns.empty()) {
        query += "*";
    } else {
        for (size_t i = 0; i < stmt.columns.size(); ++i) {
            if (i > 0) query += ", ";
            query += stmt.columns[i];
        }
    }

    query += " FROM " + table_name;

    // [修改] 从 stmt.where_clause 读取（不再从 with_params["where"]）
    if (!stmt.where_clause.empty()) {
        query += " WHERE " + stmt.where_clause;
    }

    return query;
}
```

**ExecuteTransfer 改造**：
```cpp
// [修改] 增加 source_table 和 dest_table 参数
int SchedulerPlugin::ExecuteTransfer(IChannel* source, IChannel* sink,
                                      const std::string& source_type,
                                      const std::string& sink_type,
                                      const SqlStatement& stmt,
                                      const std::string& source_table,
                                      const std::string& dest_table) {
    if (source_type == "dataframe" && sink_type == "dataframe") {
        auto* src = dynamic_cast<IDataFrameChannel*>(source);
        auto* dst = dynamic_cast<IDataFrameChannel*>(sink);
        if (!src || !dst) return -1;

        // [新增] DataFrame→DataFrame + WHERE：先 Filter 再 Copy
        if (!stmt.where_clause.empty()) {
            DataFrame tmp;
            if (src->Read(&tmp) != 0) return -1;
            if (tmp.Filter(stmt.where_clause.c_str()) != 0) return -1;
            // 写入临时通道再 Copy
            auto filtered = std::make_shared<DataFrameChannel>("_filter", "tmp");
            filtered->Open();
            filtered->Write(&tmp);
            return ChannelAdapter::CopyDataFrame(filtered.get(), dst);
        }

        return ChannelAdapter::CopyDataFrame(src, dst);
    }

    if (source_type == "dataframe" && sink_type == "database") {
        auto* src = dynamic_cast<IDataFrameChannel*>(source);
        auto* dst = dynamic_cast<IDatabaseChannel*>(sink);
        if (!src || !dst) return -1;

        // [新增] DataFrame source + WHERE：Scheduler 层先 Filter
        if (!stmt.where_clause.empty()) {
            DataFrame tmp;
            if (src->Read(&tmp) != 0) return -1;
            if (tmp.Filter(stmt.where_clause.c_str()) != 0) return -1;
            auto filtered = std::make_shared<DataFrameChannel>("_filter", "tmp");
            filtered->Open();
            filtered->Write(&tmp);
            return ChannelAdapter::WriteFromDataFrame(filtered.get(), dst, dest_table.c_str());
        }

        return ChannelAdapter::WriteFromDataFrame(src, dst, dest_table.c_str());
    }

    if (source_type == "database" && sink_type == "dataframe") {
        auto* src = dynamic_cast<IDatabaseChannel*>(source);
        auto* dst = dynamic_cast<IDataFrameChannel*>(sink);
        if (!src || !dst) return -1;

        // Database source：WHERE 下推到 SQL 查询
        std::string query = BuildQuery(source_table, stmt);
        return ChannelAdapter::ReadToDataFrame(src, query.c_str(), dst);
    }

    if (source_type == "database" && sink_type == "database") {
        auto* src = dynamic_cast<IDatabaseChannel*>(source);
        auto* dst = dynamic_cast<IDatabaseChannel*>(sink);
        if (!src || !dst) return -1;

        auto tmp = std::make_shared<DataFrameChannel>("_adapter", "tmp");
        tmp->Open();

        // Database source：WHERE 下推到 SQL 查询
        std::string query = BuildQuery(source_table, stmt);
        int rc = ChannelAdapter::ReadToDataFrame(src, query.c_str(), tmp.get());
        if (rc != 0) return rc;

        return ChannelAdapter::WriteFromDataFrame(tmp.get(), dst, dest_table.c_str());
    }

    return -1;
}
```

**ExecuteWithOperator 改造**（类似）：
```cpp
int SchedulerPlugin::ExecuteWithOperator(IChannel* source, IChannel* sink,
                                          IOperator* op,
                                          const std::string& source_type,
                                          const std::string& sink_type,
                                          const SqlStatement& stmt,
                                          const std::string& source_table,
                                          const std::string& dest_table) {
    IChannel* actual_source = source;
    IChannel* actual_sink = sink;
    std::shared_ptr<DataFrameChannel> tmp_in, tmp_out;

    if (source_type == "database") {
        auto* db_src = dynamic_cast<IDatabaseChannel*>(source);
        if (!db_src) return -1;

        tmp_in = std::make_shared<DataFrameChannel>("_adapter", "in");
        tmp_in->Open();

        // Database source：WHERE 下推到 SQL 查询
        std::string query = BuildQuery(source_table, stmt);
        int rc = ChannelAdapter::ReadToDataFrame(db_src, query.c_str(), tmp_in.get());
        if (rc != 0) return rc;

        actual_source = tmp_in.get();
    } else if (source_type == "dataframe" && !stmt.where_clause.empty()) {
        // DataFrame source + WHERE：Scheduler 层先 Filter
        auto* df_src = dynamic_cast<IDataFrameChannel*>(source);
        if (!df_src) return -1;

        tmp_in = std::make_shared<DataFrameChannel>("_adapter", "in");
        tmp_in->Open();

        DataFrame tmp;
        if (df_src->Read(&tmp) != 0) return -1;
        if (tmp.Filter(stmt.where_clause.c_str()) != 0) return -1;
        tmp_in->Write(&tmp);

        actual_source = tmp_in.get();
    }

    if (sink_type == "database") {
        tmp_out = std::make_shared<DataFrameChannel>("_adapter", "out");
        tmp_out->Open();
        actual_sink = tmp_out.get();
    }

    // 配置算子参数
    for (const auto& [key, val] : stmt.with_params) {
        op->Configure(key.c_str(), val.c_str());
    }

    auto pipeline = PipelineBuilder()
                        .SetSource(actual_source)
                        .SetOperator(op)
                        .SetSink(actual_sink)
                        .Build();
    pipeline->Run();

    if (pipeline->State() == PipelineState::FAILED) {
        return -1;
    }

    if (sink_type == "database" && tmp_out) {
        auto* db_sink = dynamic_cast<IDatabaseChannel*>(sink);
        if (!db_sink) return -1;

        // 使用 dest_table
        int rc = ChannelAdapter::WriteFromDataFrame(tmp_out.get(), db_sink, dest_table.c_str());
        if (rc != 0) return rc;
    }

    return 0;
}
```

---

#### 7. 完整执行流程

**场景 1：Database + WHERE → DataFrame**
```sql
SELECT col1, col2 FROM sqlite.mydb.users WHERE age>18 INTO result
```

```
SqlParser::Parse
  ↓
stmt.source = "sqlite.mydb.users"
stmt.where_clause = "age>18"
  ↓
ParseChannelReference("sqlite.mydb.users")
  → channel_name = "sqlite.mydb", table_name = "users"
  ↓
FindChannel("sqlite.mydb") → IDatabaseFactory::Get("sqlite", "mydb")
  ↓
BuildQuery("users", stmt) → "SELECT col1, col2 FROM users WHERE age>18"
  ↓
ChannelAdapter::ReadToDataFrame(db_channel, query, df_channel)
  ↓
SqliteDriver 执行 WHERE 过滤 → 返回过滤后数据
```

**场景 2：DataFrame + WHERE → Database**
```sql
SELECT * FROM test.data WHERE key=1 INTO sqlite.mydb.users
```

```
ParseChannelReference("test.data")
  → channel_name = "test.data", table_name = ""
  ↓
ParseChannelReference("sqlite.mydb.users")
  → channel_name = "sqlite.mydb", table_name = "users"
  ↓
ExecuteTransfer(dataframe → database)
  ↓
src->Read(&tmp)                          ← Scheduler 层读取 DataFrame
  ↓
tmp.Filter("key=1")                      ← Scheduler 层过滤
  ↓
filtered->Write(&tmp)                    ← 写入临时 DataFrameChannel
  ↓
ChannelAdapter::WriteFromDataFrame(filtered, dst, "users")
  ↓
WriteToDatabase(filtered_data, "users")  ← ChannelAdapter 纯格式转换
```

**场景 3：Database + WHERE → 算子 → DataFrame**
```sql
SELECT col1, col2 FROM mysql.prod.orders WHERE status='pending'
USING explore.chisquare WITH threshold=0.05
```

```
BuildQuery("orders", stmt) → "SELECT col1, col2 FROM orders WHERE status='pending'"
  ↓
ReadToDataFrame(db, query, tmp_in) → tmp_in 包含过滤后数据
  ↓
operator->Configure("threshold", "0.05")
  ↓
Pipeline(tmp_in → operator → sink)
```

---

#### 8. 边界情况处理

**1. DataFrame → DataFrame + WHERE（支持）**
```sql
SELECT * FROM test.data1 WHERE key=1 INTO test.data2
```
- Scheduler 读取 DataFrame 后调用 `Filter("key=1")`，再 CopyDataFrame 到目标
- WHERE 过滤在 Scheduler 层完成，ChannelAdapter 不感知 WHERE 语义

**2. WHERE 子句包含特殊字符**
```sql
SELECT * FROM sqlite.mydb.users WHERE name='O''Reilly'
```
- SqlParser 需要正确处理字符串中的引号转义
- 直接传递给数据库，由数据库处理

**3. WHERE 子句语法错误**
```sql
SELECT * FROM sqlite.mydb.users WHERE age>  -- 语法错误
```
- SqlParser 可能无法检测所有语法错误
- 数据库执行时返回错误，通过 `IBatchReader::GetLastError()` 返回给用户

**4. DataFrame Filter 条件解析失败**
```sql
SELECT * FROM test.data WHERE invalid_column=1 INTO sqlite.mydb.users
```
- DataFrame::Filter 返回错误码
- Scheduler（ExecuteTransfer/ExecuteWithOperator）检测错误并返回

---

#### 9. 实施计划

1. Phase 3：在 SqlStatement 中预留 `where_clause` 字段，实现 `ParseChannelReference`
2. Phase 4：扩展 `SqlParser`，实现 WHERE 子句解析和 `ValidateWhereClause`
3. Phase 5：实现 `IDataFrame::Filter()` 方法
4. Phase 3/5：修改 `ExecuteTransfer` 和 `ExecuteWithOperator`，Scheduler 层处理 WHERE
5. Phase 4：修改 `BuildQuery`，从 `stmt.where_clause` 读取
6. Phase 7：编写测试用例验证 WHERE 下推和 DataFrame 过滤

## 14. 未来扩展

### 14.1 MySQL 驱动

- 引入 `libmysqlclient` 依赖
- 实现 `MysqlDriver` + `MysqlBatchReader` + `MysqlBatchWriter`
- 类型映射：`MYSQL_TYPE_LONG` → Arrow Int32, `MYSQL_TYPE_VARCHAR` → Arrow Utf8

### 14.2 ClickHouse 驱动

- 利用 HTTP + ArrowStream 原生支持
- 实现最简单，零类型转换
- `SELECT ... FORMAT ArrowStream` 直接返回 Arrow IPC

### 14.3 连接池

当前每个通道 = 一个连接。未来可在 DatabasePlugin 中实现连接池：
- 同一配置的多个通道共享连接池
- 支持最大连接数、空闲超时等配置

### 14.4 事务支持

扩展 IDatabaseChannel 接口：
```cpp
virtual int BeginTransaction() = 0;
virtual int Commit() = 0;
virtual int Rollback() = 0;
```

### 14.5 预编译语句缓存

在 IDbDriver 中缓存常用查询的预编译语句，提升性能。

### 14.6 更多数据库

- PostgreSQL
- Oracle
- SQL Server
- MongoDB（需要扩展 NoSQL 语义）

## 15. 权限管理预留设计

### 15.1 设计原则

当前阶段不实施权限管理，但接口设计需要为未来扩展预留空间，确保：
- 不破坏现有代码
- 可以无缝添加权限层
- 向后兼容

### 15.2 接口预留

#### IDatabaseFactory 扩展预留

`GetWithContext` 方法已在 §3.1 的正式接口定义中包含（带默认实现），无需额外修改。
未来实现权限管理时，只需 override `GetWithContext` 方法即可。

#### IAuthService 接口定义（预留）

```cpp
// src/framework/interfaces/iauth_service.h
#ifndef _FLOWSQL_FRAMEWORK_INTERFACES_IAUTH_SERVICE_H_
#define _FLOWSQL_FRAMEWORK_INTERFACES_IAUTH_SERVICE_H_

#include <common/typedef.h>
#include <functional>

namespace flowsql {

constexpr Guid IID_AUTH_SERVICE = {0xb1c2d3e4, 0xf5a6, 0x7890,
                                   {0xab, 0xcd, 0xef, 0x12, 0x34, 0x56, 0x78, 0x90}};

// 权限操作类型
enum class PermissionType {
    READ = 0,
    WRITE = 1,
    EXECUTE = 2,
    ADMIN = 3
};

// 认证服务接口（预留，当前不实现）
interface IAuthService {
    virtual ~IAuthService() = default;

    // 用户认证
    virtual bool Authenticate(const char* username, const char* password) = 0;

    // 检查用户是否有权访问指定资源
    // resource: 资源标识（如 "sqlite.mydb.users"）
    // permission: 权限类型
    virtual bool CheckPermission(const char* username,
                                 const char* resource,
                                 PermissionType permission) = 0;

    // 获取用户角色
    virtual const char* GetUserRole(const char* username) = 0;

    // 审计日志
    virtual void AuditLog(const char* username,
                          const char* operation,
                          const char* resource,
                          bool success) = 0;
};

}  // namespace flowsql

#endif
```

#### Scheduler 接口预留

```cpp
// src/services/scheduler/scheduler_plugin.h
class SchedulerPlugin : public IPlugin {
public:
    // [当前] 执行 SQL（无用户上下文）
    int HandleExecute(const SqlStatement& stmt, std::string& result);

    // [预留] 执行 SQL（带用户上下文）
    // 未来实现时，可以在执行前检查权限
    // 当前实现：忽略 user_context，调用 HandleExecute(stmt, result)
    int HandleExecuteWithContext(const SqlStatement& stmt,
                                 const char* user_context,
                                 std::string& result) {
        // 默认实现：忽略用户上下文
        return HandleExecute(stmt, result);
    }

private:
    IQuerier* querier_ = nullptr;
    IAuthService* auth_service_ = nullptr;  // [预留] 权限服务（当前为 nullptr）
};
```

### 15.3 配置预留

```yaml
# config/gateway.yaml

# [当前] 数据库配置（使用环境变量存储密码）
databases:
  - type: sqlite
    name: mydb
    path: /data/mydb.db

  - type: mysql
    name: prod
    host: 127.0.0.1
    port: 3306
    user: ${DB_USER}
    password: ${DB_PASSWORD}
    database: mydb

# [预留] 权限配置（当前不启用）
# auth:
#   enabled: false  # 当前禁用
#   provider: builtin  # builtin | ldap | oauth2
#
#   users:
#     - name: analyst
#       role: readonly
#       allowed_databases:
#         - sqlite.mydb.users
#         - mysql.prod.orders
#       denied_operations:
#         - DELETE
#         - DROP
#
#     - name: admin
#       role: admin
#       allowed_databases: "*"
#
#   credential_mapping:
#     - flowsql_user: analyst
#       database: mysql.prod
#       db_user: analyst_db_user
#       db_password: ${ANALYST_PASSWORD}
```

### 15.4 未来扩展路径

#### Phase 1（当前）：无权限管理

```cpp
// DatabasePlugin::Get 实现
IDatabaseChannel* DatabasePlugin::Get(const char* type, const char* name) {
    // 直接返回通道，无权限检查
    return GetOrCreateChannel(type, name);
}

// Scheduler::HandleExecute 实现
int SchedulerPlugin::HandleExecute(const SqlStatement& stmt, std::string& result) {
    // 直接执行，无权限检查
    return ExecuteInternal(stmt, result);
}
```

#### Phase 2：添加权限管理（无需修改现有代码）

```cpp
// DatabasePlugin::GetWithContext 实现
IDatabaseChannel* DatabasePlugin::GetWithContext(const char* type,
                                                 const char* name,
                                                 const char* user_context) {
    // 1. 检查权限
    if (auth_service_ && !auth_service_->CheckPermission(user_context,
                                                          MakeResourceId(type, name),
                                                          PermissionType::READ)) {
        return nullptr;  // 权限拒绝
    }

    // 2. 根据用户上下文选择凭证
    if (credential_service_) {
        auto cred = credential_service_->GetCredential(user_context, type, name);
        return GetOrCreateChannelWithCredential(type, name, user_context, cred);
    }

    // 3. 降级到默认行为
    return Get(type, name);
}

// Scheduler::HandleExecuteWithContext 实现
int SchedulerPlugin::HandleExecuteWithContext(const SqlStatement& stmt,
                                              const char* user_context,
                                              std::string& result) {
    // 1. 权限检查
    if (auth_service_) {
        if (!auth_service_->CheckPermission(user_context, stmt.source, PermissionType::READ)) {
            return -1;  // 权限拒绝
        }
        if (!stmt.dest.empty()) {
            if (!auth_service_->CheckPermission(user_context, stmt.dest, PermissionType::WRITE)) {
                return -1;
            }
        }
    }

    // 2. 审计日志
    if (auth_service_) {
        auth_service_->AuditLog(user_context, "EXECUTE", stmt.source, true);
    }

    // 3. 执行
    return ExecuteInternal(stmt, result);
}
```

#### Phase 3：客户端调用改造

```cpp
// Gateway 层传递用户上下文
int GatewayPlugin::HandleRequest(const HttpRequest& req) {
    // 1. 从 HTTP Header 或 Token 中提取用户信息
    std::string user_context = ExtractUserContext(req);

    // 2. 调用带上下文的接口
    auto scheduler = GetScheduler();
    return scheduler->HandleExecuteWithContext(stmt, user_context.c_str(), result);
}
```

### 15.5 安全基线（Phase 1 必须实施）

即使不实施完整权限管理，也需要基础安全措施：

#### 1. 密码保护

```yaml
# 使用环境变量，不在配置文件中明文存储
password: ${DB_PASSWORD}
```

#### 2. SQL 注入防护（临时方案）

> **风险提示**：当前采用关键字黑名单方案，存在绕过风险（大小写混合、注释插入等）。
> 中期应升级为白名单验证（只允许比较运算符、AND/OR、括号、字面量），
> 长期应使用参数化查询。

```cpp
// SqlParser 增加 WHERE 子句验证
bool SqlParser::ValidateWhereClause(const std::string& where_clause) {
    // 检查危险关键字（临时方案，后续升级为白名单）
    static const std::vector<std::string> dangerous_keywords = {
        "DROP", "DELETE", "UPDATE", "INSERT", "TRUNCATE", "ALTER", "CREATE",
        "--", "/*", "*/", "EXEC", "EXECUTE"
    };

    std::string upper_clause = ToUpper(where_clause);
    for (const auto& keyword : dangerous_keywords) {
        if (upper_clause.find(keyword) != std::string::npos) {
            return false;  // 拒绝危险 SQL
        }
    }

    return true;
}
```

#### 3. 只读模式配置

```yaml
# 数据库配置增加只读标志
databases:
  - type: mysql
    name: prod
    readonly: true  # 只允许 SELECT 操作
    host: 127.0.0.1
    user: readonly_user
    password: ${RO_PASSWORD}
```

```cpp
// DatabaseChannel 增加只读检查
class DatabaseChannel : public IDatabaseChannel {
public:
    int CreateWriter(const char* table, IBatchWriter** writer) override {
        if (readonly_) {
            return -1;  // 只读模式，拒绝写入
        }
        return driver_->CreateWriter(table, writer);
    }

private:
    bool readonly_ = false;  // 从配置读取
};
```

### 15.6 接口兼容性保证

**向后兼容承诺**：
- `Get(type, name)` 接口永远保留，用于无权限场景
- `GetWithContext(type, name, user_context)` 默认实现调用 `Get()`
- 现有代码无需修改即可继续工作
- 新代码可以选择使用带上下文的接口

**扩展点**：
- `IAuthService` 接口已定义，可插拔实现
- `IDatabaseFactory::GetWithContext` 预留用户上下文参数
- `SchedulerPlugin::HandleExecuteWithContext` 预留权限检查点
- 配置文件预留 `auth` 配置段

**实施路径**：
1. Phase 1：实现安全基线（密码保护、SQL 验证、只读模式）
2. Phase 2：实现 `IAuthService` 和权限检查逻辑
3. Phase 3：实现动态凭证和用户级连接池
