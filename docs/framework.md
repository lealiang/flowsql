# FlowSQL 架构文档

## 核心设计原则

1. **统一框架**：所有 C++ 服务都是同一个 `flowsql` 可执行文件加载不同 `.so` 插件运行，彼此对等；Python Worker 是独立的 FastAPI 进程
2. **IPlugin 机制**：所有功能模块以 `.so` 插件形式加载，通过 `IPlugin` 接口统一生命周期管理
3. **控制面 HTTP**：服务间控制面通信全部走 HTTP + URI 路由，Gateway 负责转发
4. **数据面独立**：高吞吐数据传输走共享内存 / Arrow IPC，不经过 HTTP
5. **Interface 思维**：Plugin 通过纯虚接口（`IID_*`）向进程内其他 Plugin 暴露能力，调用方通过 `IQuerier::Traverse` 按 IID 查找，不直接依赖具体实现类

---

## 服务拓扑

```
                    ┌─────────────────────┐
                    │       Gateway       │
                    │  (flowsql_gateway)  │
                    │  路由表 / 心跳监控   │
                    │  子进程生命周期管理  │
                    └──────┬──────────────┘
                           │ posix_spawn + HTTP 转发
              ┌────────────┼────────────┐
              │            │            │
     ┌────────▼───┐  ┌─────▼──────┐  ┌─▼─────────────┐
     │    Web     │  │ Scheduler  │  │ Python Worker  │
     │ web.so     │  │ sched.so   │  │   (FastAPI)    │
     │ 前端 + API │  │ bridge.so  │  │  Python 算子   │
     └────────────┘  │ database.so│  └────────────────┘
                     │ example.so │
                     └────────────┘
```

**端口分配**：Gateway 18800 · Web 8081 · Scheduler 18803 · PyWorker 18900

---

## Gateway

### 职责
- 维护 URI 前缀路由表（prefix → service address）
- HTTP 请求转发：剥离匹配前缀后转发给目标服务
- `posix_spawn` 管理子服务进程，心跳超时自动重启
- 自身也是框架 + `gateway.so`，与其他服务对等

### 路由机制
路由匹配优先取前 2 级精确匹配，未命中回退到前 1 级。

```
请求 /scheduler/db-channels/add
  → 匹配 /scheduler → 转发 /db-channels/add 给 Scheduler
```

### Gateway 内置接口

| 接口 | 方法 | 说明 |
|------|------|------|
| `/gateway/register` | POST | 注册路由 `{ "prefix": "/scheduler", "address": "127.0.0.1:18803", "service": "scheduler" }` |
| `/gateway/unregister` | POST | 注销路由 |
| `/gateway/routes` | GET | 查询所有已注册路由 |
| `/gateway/heartbeat` | POST | 心跳上报 `{ "service": "scheduler" }` |

### 心跳机制
各服务定期 `POST /gateway/heartbeat`，超过 `heartbeat_timeout_count × heartbeat_interval_s`（默认 15s）未收到，Gateway 自动重启该服务。

---

## Scheduler

Scheduler 进程加载多个插件，通过 `IQuerier` 进行进程内插件间通信：

| 插件 | IID | 职责 |
|------|-----|------|
| `libflowsql_scheduler.so` | `IID_SCHEDULER` | SQL 解析、Pipeline 执行、任务管理、通道管理端点 |
| `libflowsql_bridge.so` | `IID_BRIDGE` | C++ ↔ Python 算子桥接，发现并注册 Python 算子 |
| `libflowsql_database.so` | `IID_DATABASE_FACTORY` | 数据库通道管理（MySQL / SQLite / ClickHouse） |
| `libflowsql_example.so` | `IID_OPERATOR` | 示例 C++ 算子 |

### SQL 执行流程

```
用户 SQL → Web → Gateway → Scheduler
  → SqlParser 解析（source / operator / dest）
  → FindChannel(source)  ← 支持三段式 type.name.table
  → ExecuteTransfer / ExecuteWithOperator
  → 返回结果 JSON
```

**三段式寻址**：`mysql.prod.test_users`
- `mysql` → 通道类型（type）
- `prod` → 通道逻辑名（name，由用户添加通道时指定）
- `test_users` → 表名（BuildQuery 替换 FROM 子句）

### 数据库通道管理

`IDatabaseFactory`（由 `database.so` 实现）提供：
- `Get(type, name)` → 懒加载连接，断线自动重连
- `AddChannel / RemoveChannel / UpdateChannel` → 运行时动态管理
- `List` → 枚举通道，密码字段脱敏

通道配置持久化到 `config/flowsql.yml`，密码 AES-256-GCM 加密（`ENC:` 前缀）。Scheduler 重启后自动从 YAML 恢复通道。

支持的数据库类型：

| 类型 | 驱动 | 连接方式 |
|------|------|---------|
| `sqlite` | SqliteDriver | 文件 / `:memory:` |
| `mysql` | MysqlDriver | 连接池（libmysqlclient） |
| `clickhouse` | ClickHouseDriver | HTTP 无状态（httplib，无连接池） |

### Scheduler 端点

| 端点 | 说明 |
|------|------|
| `POST /scheduler/execute` | 执行 SQL |
| `GET /scheduler/db-channels` | 列出数据库通道 |
| `POST /scheduler/db-channels/add` | 添加通道 |
| `POST /scheduler/db-channels/remove` | 删除通道 |
| `POST /scheduler/db-channels/update` | 更新通道 |
| `POST /scheduler/refresh-operators` | 刷新算子列表 |

---

## Python 算子

### 注册流程

```
Gateway spawn Scheduler 和 PyWorker
  ↓
BridgePlugin::Load（Scheduler 进程内）：
  轮询 GET /gateway/routes（最多 10 次，间隔 1s）
  → 找到 /pyworker 前缀对应的地址，记录 PyWorker host:port
  ↓
BridgePlugin::Start：
  直连 PyWorker，GET /operators（最多 30 次重试，间隔 1s）
  → 解析算子元数据列表
  → 为每个算子创建 PythonOperatorBridge，存入内部 registered_operators_
  （不注册到 PluginLoader，不走 IQuerier）
```

Scheduler 查找算子时两条路径：
1. `IQuerier::Traverse(IID_OPERATOR)` → C++ 算子（进程内）
2. `IBridge::FindOperator(catelog, name)` → Python 算子（Bridge 内部查找）

**Reload**：用户在 Web 触发 → `POST /scheduler/refresh-operators` → Bridge 重新调用 `DiscoverOperators()` → 更新 `registered_operators_`。

### 执行数据面

**C++ 算子**：进程内直接函数调用，零开销。

**Python 算子**：跨进程，共享内存传数据，HTTP 只传控制指令。

```
Pipeline → operator->Work(source, sink)
  （实际调用 PythonOperatorProxy → PythonOperatorBridge）
  1. 从 source IChannel 读取 Arrow RecordBatch
  2. Arrow IPC 序列化写入 /dev/shm/flowsql_<uuid>_in
     （数据量超阈值时回退到 /tmp，Python 侧无感知）
  3. POST /pyworker/work/<catelog>/<name>  { "input": "/dev/shm/..." }
  4. Python Worker:
       pa.memory_map(path) → Arrow Table（零拷贝）
       → Polars DataFrame → operator.work(df) → df_out
       → Arrow IPC 写入 /dev/shm/flowsql_<uuid>_out
       → 返回 200 { "output": "/dev/shm/..." }
  5. Bridge: memory_map 读取 _out → Arrow RecordBatch → 写入 sink IChannel
  6. SharedMemoryGuard 析构，自动 unlink _in / _out
```

Pipeline 统一面向 `IOperator` 接口，不感知算子是 C++ 还是 Python。

**共享内存生命周期**：所有权归 Bridge（创建 _in，读取 _out，清理两者）；PyWorker 无状态，只读写不清理。Scheduler 启动时扫描 `/dev/shm/flowsql_*` 清理上次残留。

---

## 部署配置

**`config/gateway.yaml`**（启动入口）：

```yaml
gateway:
  host: 127.0.0.1
  port: 18800
  heartbeat_interval_s: 5
  heartbeat_timeout_count: 3

services:
  - name: web
    plugins: [libflowsql_web.so]
    port: 8081

  - name: scheduler
    plugins:
      - libflowsql_scheduler.so
      - libflowsql_bridge.so
      - libflowsql_example.so
      - name: libflowsql_database.so
        option: "config_file=config/flowsql.yml"
    port: 18803

  - name: pyworker
    type: python
    command: "python3 -m flowsql.worker"
    port: 18900
```

**`config/flowsql.yml`**（运行时通道配置，由 Web 动态管理）：

```yaml
channels:
  database_channels:
    - type: mysql
      name: prod
      host: 127.0.0.1
      port: 3306
      user: flowsql_user
      password: "ENC:..."
      database: flowsql_db
```

---

## 项目结构

```
flowSQL/
├── src/
│   ├── common/             # 公共头文件（define.h、loader.hpp、toolkit.hpp 等）
│   ├── framework/          # 框架核心（IPlugin、PluginLoader、SqlParser、Pipeline 等）
│   │   └── interfaces/     # 跨插件接口（IDatabaseFactory、IDatabaseChannel 等）
│   ├── services/
│   │   ├── gateway/        # libflowsql_gateway.so
│   │   ├── scheduler/      # libflowsql_scheduler.so
│   │   ├── database/       # libflowsql_database.so（含 MySQL/SQLite/ClickHouse 驱动）
│   │   ├── bridge/         # libflowsql_bridge.so
│   │   └── web/            # libflowsql_web.so + 前端静态文件
│   ├── plugins/
│   │   └── example/        # libflowsql_example.so
│   ├── python/             # Python Worker（FastAPI + 算子运行时）
│   ├── frontend/           # Vue.js 前端
│   ├── app/                # flowsql 可执行文件入口（main.cpp）
│   └── tests/
│       ├── test_framework/
│       ├── test_database/  # test_sqlite / test_mysql / test_clickhouse / test_database / test_database_manager
│       ├── test_bridge/
│       └── test_npi/
├── config/
│   ├── gateway.yaml        # 服务编排配置
│   └── flowsql.yml         # 运行时通道配置（自动生成）
├── build/output/           # 编译产物
├── .thirdparts_installed/  # 第三方依赖安装缓存
└── docs/
```

---

## 构建与运行

```bash
# 构建
cmake -B build src && cmake --build build -j$(nproc)

# 启动（从项目根目录）
cd build/output && LD_LIBRARY_PATH=. ./flowsql --config ../../config/gateway.yaml

# 调试 Scheduler（VSCode launch.json: "flowsql:scheduler (direct)"）
./flowsql --role scheduler --gateway 127.0.0.1:18800 --port 18803 \
  --plugins "libflowsql_scheduler.so,libflowsql_bridge.so,libflowsql_example.so,libflowsql_database.so:config_file=config/flowsql.yml"

# 测试
cd build/output
./test_database        # 插件层 E2E（需 MySQL 环境）
./test_database_manager
./test_sqlite
./test_clickhouse      # ClickHouse 不可达时自动 SKIP
```
