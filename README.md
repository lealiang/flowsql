# FlowSQL

基于 SQL 的实时数据处理与分析共创平台

## 项目简介

FlowSQL 是一个全栈式实时数据处理与分析平台，通过扩展的 SQL 语法提供从数据采集、流式处理到结果探索的完整能力。用户无需深入了解底层执行细节，只需使用熟悉的 SQL 语句即可构建面向实时与离线场景的数据任务。

平台采用 Gateway + RouterAgency 插件架构：所有 C++ 服务共享同一个框架程序（加载不同 .so），业务插件通过 IRouterHandle 声明路由，RouterAgencyPlugin 统一收集并向 Gateway 注册，Python Worker 作为独立 FastAPI 进程运行。控制面统一走 HTTP + URI 路由，数据面通过共享内存 / Arrow IPC 实现零拷贝传输。

## 核心特性

- **插件化架构**：所有功能模块以 .so 插件形式加载，统一框架程序驱动
- **RouterAgency 路由**：业务插件实现 IRouterHandle 声明路由，RouterAgencyPlugin 收集后向 Gateway 注册，插件对 HTTP 完全无感知
- **Gateway 转发**：Trie 最长前缀匹配，KeepAlive 自动续期，服务故障自动重启
- **C++ ↔ Python 桥接**：共享内存 + Arrow IPC 零拷贝数据传输，HTTP 仅传控制指令
- **SQL 驱动**：扩展 SQL 语法统一数据采集、分析、探索操作
- **流批双模式统一**：Batch 与 Stream 均采用 SQL 驱动、统一任务管理与统一插件体系
- **三类算子统一管理**：内置算子（builtin）+ Python 算子 + C++ 插件算子统一走 `/api/operators/*`
- **Web 管理**：Vue.js 前端 + REST API，支持通道/算子/任务管理

## 快速开始

### 开发依赖服务

**MySQL**

```bash
docker compose -f config/docker-compose-mysql.yml up -d
```

连接参数：`127.0.0.1:3306`，用户 `flowsql_user`，密码 `flowSQL@user`，库 `flowsql_db`

**ClickHouse**

```bash
docker compose -f config/docker-compose-clickhouse.yml up -d
```

连接参数：HTTP `127.0.0.1:8123`，TCP `127.0.0.1:9000`，用户 `flowsql_user`，密码 `flowSQL@user`，库 `flowsql_db`

**PostgreSQL**

```bash
docker compose -f config/docker-compose-postgres.yml up -d
```

连接参数：`127.0.0.1:5432`，用户 `flowsql_user`，密码 `flowSQL@user`，库 `flowsql_db`

### 环境要求

- CMake 3.12+，C++17 编译器（GCC 7+），Linux
- Python 3.8+（Python 算子运行时）

### 系统编译依赖

- `libicu-dev`（或提供 `libicuuc` / `libicui18n` 的等价开发包）
  - 用途：Baseline 插件的业务时区换算
  - 对应 CMake 依赖：`find_package(ICU REQUIRED COMPONENTS uc i18n)`

```bash
pip3 install -e src/python/ --break-system-packages
```

### 编译

```bash
cmake -B build src && cmake --build build -j$(nproc)
```

### 运行

**单进程模式（开发调试）**

```bash
cd build/output
LD_LIBRARY_PATH=. ./flowsql --config ../../config/deploy-single.yaml
```

**多进程模式（生产部署）**

```bash
cd build/output
LD_LIBRARY_PATH=. ./flowsql --config ../../config/deploy-multi.yaml
```

启动后浏览器访问 `http://127.0.0.1:8081` 进入管理界面。

### 前端构建

```bash
cd src/frontend && npm install && npm run build
# 构建产物由 CMake 自动同步到 build/output/static/
# 或手动：rm -rf build/output/static/assets && cp -r src/frontend/dist/* build/output/static/
```

### 测试

```bash
cd build/output
./test_framework && ./test_bridge
./test_sqlite
./test_connection_pool
export FLOWSQL_SECRET_KEY="your-32-byte-secret-key-here!!"
./test_database_manager
./test_mysql        # 需运行中的 MySQL
./test_postgres     # 需运行中的 PostgreSQL
./test_clickhouse   # 需运行中的 ClickHouse
./test_router       # 路由表单元测试
./test_builtin      # Catalog/BinAddon/算子管理链路
./test_stream       # 流式通道与运行时
./test_scheduler_e2e # Stream Group DAG 端到端回归
```

## 架构

### 服务拓扑

```
浏览器
  │
  ▼
WebPlugin (8081) ── 静态文件 / API 代理入口
  │  去掉 /api 前缀
  ▼
GatewayPlugin (18800) ── Trie 最长前缀匹配转发
  ├── /api      → RouterAgencyPlugin (18802) → WebPlugin 路由
  ├── /channels → RouterAgencyPlugin (18803) → DatabasePlugin / SchedulerPlugin 路由
  ├── /tasks    → RouterAgencyPlugin (18803) → TaskPlugin 路由
  ├── /scheduler→ RouterAgencyPlugin (18803) → SchedulerPlugin 内部调度路由
  └── /operators→ RouterAgencyPlugin (18803) → CatalogPlugin 统一入口
                                             → BinAddonHostPlugin（C++ 插件生命周期）
                                             → BridgePlugin / PyWorker（Python 算子发现与执行）
```

### 请求链路

```
前端 → POST /api/channels/database/add (8081)
  → WebPlugin 去掉 /api → POST /channels/database/add → Gateway (18800)
  → Trie 匹配 /channels → RouterAgencyPlugin (18803)
  → DatabasePlugin::HandleAdd
```

### 核心接口

```
IPlugin（生命周期）
├── IRouterHandle（声明 HTTP 路由，对 HTTP 无感知）
├── IChannel（数据通道）
│   ├── IDataFrameChannel（批处理）
│   ├── IDatabaseChannel（数据库 Reader/Writer 工厂）
│   └── IStreamChannel（流式，已实现）
└── IOperator（数据算子：Work(in, out)）
```

### URI 设计约束

详见 [架构设计](docs/framework.md)。

**概要：**

| 层 | 格式 | 示例 |
|---|---|---|
| 前端对外（WebPlugin 8081） | `/api/{资源}[/{动作}]` | `/api/channels/list`, `/api/tasks/result` |
| 内部服务间（RouterAgencyPlugin） | `/{资源}[/{子类型}]/{动作}` | `/channels/database/add`, `/tasks/batch/execute`, `/tasks/stream/execute` |
| Gateway 管理（内部专用） | `/gateway/{动作}` | `/gateway/register`, `/gateway/routes` |

常见动作词汇（非穷举）：`list` / `query` / `detail` / `add` / `remove` / `modify` / `preview` / `execute` / `status` / `stop` / `cancel` / `refresh` / `reload`

## SQL 语法示例

```sql
-- 批处理算子示例（需先准备 dataframe.input 通道）
SELECT * FROM dataframe.input USING builtin.passthrough INTO dataframe.output

-- 流式单 SQL 示例（内置流式算子）
SELECT * FROM tcp_session_mock.tcp_src
USING builtin.tcp_service_merge_stream
INTO dataframe.serviceaccess

-- 流式多 SQL（Group DAG）示例
SELECT * FROM ring.src
USING builtin.passthrough_stream
INTO stream.mid;
SELECT * FROM stream.mid
USING builtin.passthrough_stream
INTO dataframe.out;
```

## SQL 任务能力矩阵（当前）

| 任务类型 | SQL 数量 | 当前支持 | 提交入口 | 关键约束 | 未来规划 |
|---|---|---|---|---|---|
| Batch | 单 SQL | ✅ 支持 | `POST /api/tasks/batch/execute`（内部：`/tasks/batch/execute`） | 可 `mode=sync/async`；使用 `sql_text` 入参；若 SQL 被判定为 stream，将拒绝并提示使用 stream API | 持续支持 |
| Batch | 多 SQL | ✅ 支持（顺序执行） | `POST /api/tasks/batch/execute`（`sql_text`） | 多 SQL 必须用分号 `;` 切分；不允许混入 stream SQL | 持续支持 |
| Stream | 单 SQL | ✅ 支持 | `POST /api/tasks/stream/execute`（内部：`/tasks/stream/execute`） | 仅异步；`execution_kind=single`；必须是 stream SQL；必须包含 `USING` 流式算子 | 持续支持 |
| Stream | 多 SQL | ✅ 支持（Group DAG） | `POST /api/tasks/stream/execute`（`execution_kind=group` + `group_mode=dag` + `sql_text`） | 仅异步；多 SQL 必须用分号 `;` 切分；至少 2 条；纯 stream 编排可直接执行 | 持续支持 |
| Mixed（batch+stream） | 多 SQL | ✅ 支持（Hybrid DAG） | `POST /api/tasks/stream/execute`（`execution_kind=group` + `group_mode=dag` + `sql_text`） | 仅异步；多 SQL 必须用分号 `;` 切分；每条 SQL 内部不得混用 stream 与非 stream source | 持续增强 |
| Stream | 同源跨任务并发消费 | ✅ 支持（late join） | `POST /api/tasks/stream/execute`（多任务并行提交） | 新任务从加入时刻开始消费，不补历史；同源共享要求 `WHERE` 签名一致 | 持续支持 |

说明：
- Stream 当前仅支持异步执行。
- Stream 多 SQL 的 `group` 当前仅支持 `group_mode=dag`。
- Stream 支持同一 source 的跨任务并发消费（late join）；任务间 stop/cancel/fail 相互隔离。
- `POST /api/tasks/stream/status` 与 `POST /api/tasks/stream/list` 已提供共享消费观测字段：`shared_hub_id`、`shared_source_keys`、`subscriber_count`、`subscriber_stats`（含 `lag`）。
- 流批一体已支持：单任务内可执行 batch + stream 混合 DAG（Hybrid DAG）。
- 当前边界：同一条 SQL 的 source 不允许同时包含 stream 与非 stream；否则提交阶段报错。

## SQL 任务类型判定规则（当前）

SQL 任务类型由 Source 通道类型决定（看 `FROM`，不看 `INTO`）：

- Source 全部是 stream 通道：判定为 `stream` 任务。
- Source 全部是非 stream 通道（如 dataframe/database）：判定为 `batch` 任务。
- 同一条 SQL 的 Source 同时包含 stream 与非 stream：当前不支持，直接报错。
- 多 SQL group 中允许不同语句分别判定为 `batch/stream`，整体按 `mixed` 编排执行。

补充说明：

- `INTO` 目标通道类型不参与任务类型判定。
- `USING` 本身不决定任务类型，但 `stream` 执行入口要求包含流式算子。
- 任务类型与执行入口必须匹配：`stream` 走 `/api/tasks/stream/execute`，`batch` 走 `/api/tasks/batch/execute`。

示例：

```sql
-- batch：Source 是 dataframe
SELECT * FROM dataframe.VNAT INTO ring.spsc_stream;

-- stream：Source 是 stream
SELECT * FROM ring.spsc_stream
USING builtin.passthrough_stream
INTO dataframe.VNAT_COPY;
```

## 流式任务执行契约

### API 入口

- 对外统一：`POST /api/tasks/stream/execute`
- 内部调度：`POST /scheduler/stream/execute`

### 请求字段

- `execution_kind`：`single` 或 `group`
- `group_mode`：仅当 `execution_kind=group` 时必填，目前固定 `dag`
- `sql_text`：SQL 文本
  - `single`：必须是 1 条 SQL（不允许多语句）
  - `group`：必须是多条 SQL，并且使用分号 `;` 作为语句分隔符
  - 仅换行不作为切分符；字符串/注释中的分号不会被误切分

### 请求示例

单条流式任务（single）：

```json
{
  "execution_kind": "single",
  "sql_text": "SELECT * FROM tcp_session_mock.tcp_src USING builtin.tcp_service_merge_stream INTO dataframe.serviceaccess"
}
```

多条流式 DAG 任务（group）：

```json
{
  "execution_kind": "group",
  "group_mode": "dag",
  "timeout_s": 120,
  "share_set_ready_timeout_s": 30,
  "sql_text": "SELECT * FROM ring.src USING builtin.passthrough_stream INTO stream.mid;SELECT * FROM stream.mid USING builtin.passthrough_stream INTO dataframe.out;"
}
```

### 状态与控制

- `POST /api/tasks/stream/status`
- `POST /api/tasks/stream/list`
- `POST /api/tasks/stream/stop`

说明：`stop/status` 使用 `task_id` 查询，不接受 `runtime_task_id`。

### Stream 通道管理

- `POST /api/channels/stream/query`
- `POST /api/channels/stream/definitions/query`
- `POST /api/channels/stream/add`
- `POST /api/channels/stream/modify`
- `POST /api/channels/stream/reset`
- `POST /api/channels/stream/remove`

说明：`reset` 仅重置运行态（同配置重建通道），不改动通道配置。

### 常见错误码

| 错误码 | 含义 | 常见原因 |
|---|---|---|
| `STREAM_GROUP_SQL_TEXT_INVALID` | `sql_text` 不合法 | `single` 传了多语句；`group` 语句不足 2 条；存在空语句（`;;`） |
| `STREAM_GROUP_TIMEOUT` | group 运行超时 | `timeout_s` 到期仍有节点未收敛 |
| `STREAM_GROUP_SHARE_SET_READY_TIMEOUT` | share set ready 屏障超时 | 同源广播组未在限定时间内全部就绪 |
| `STREAM_GROUP_SOURCE_MISMATCH` | 同源校验不一致 | `source_share_set` 成员解析源集合不一致（返回 `missing_keys/extra_keys`） |
| `STREAM_GROUP_SINK_CAPABILITY_MISMATCH` | sink 并发写能力不足 | 多节点写同一 stream sink，但通道能力不满足 |
| `SHARED_SOURCE_WHERE_MISMATCH` | 共享 source 的 `WHERE` 签名不一致 | 同一 source 上新任务的 `WHERE` 与已运行共享 hub 不一致 |

## 算子扩展能力

### Python 算子扩展

Python 算子以 `.py` 文件形式扩展，运行在独立 Python Worker 进程中，通过 BridgePlugin 与 C++ 调度面协作。

开发方式：

1. 继承 `OperatorBase`。
2. 使用 `@register_operator(category, name, description, position)` 注册元数据。
3. 实现 `work(df_in)`，输入输出均为 DataFrame（默认 Polars）。

管理能力（统一 API）：

- 上传：`POST /api/operators/upload`（`type=python`，支持 `multipart file` 或 `content`）
- 激活：`POST /api/operators/activate`（`{"type":"python","name":"category.name"}`）
- 去激活：`POST /api/operators/deactivate`
- 详情/编辑/删除：`/api/operators/detail`、`/api/operators/update`、`/api/operators/delete`

### C++ 算子插件扩展（.so）

C++ 算子以“插件文件（`.so`）”为管理单元。一个插件可包含多个算子，统一由 BinAddonHostPlugin 托管生命周期。

插件必须导出 4 个符号（`extern "C"`）：

1. `flowsql_abi_version`
2. `flowsql_operator_count`
3. `flowsql_create_operator`
4. `flowsql_destroy_operator`

管理能力（统一 API）：

- 上传：`POST /api/operators/upload`（`type=cpp`）
- 激活/去激活：`POST /api/operators/activate|deactivate`（按 `plugin_id`）
- 列表/详情：`GET /api/operators/list?type=cpp`、`POST /api/operators/detail`
- 删除：`POST /api/operators/delete`

关键行为：

- 上传后生成稳定 `plugin_id`（基于文件指纹）并持久化。
- 激活时执行 ABI、导出符号、算子冲突检查；失败原因可在详情中查看。
- 运行时由调度器按 `category.name` 调用已激活的插件算子。

可参考开发样例：[C++ 算子插件 Sample](samples/cpp_operator/README.md)

## 项目结构

```
flowSQL/
├── build/output/           # 编译产物（.so、可执行文件、static/）
├── config/
│   ├── deploy-single.yaml  # 单进程部署配置（开发调试）
│   ├── deploy-multi.yaml   # 多进程部署配置（生产）
│   └── flowsql.yml         # 数据库通道持久化配置
├── src/
│   ├── app/                # 主程序入口
│   ├── common/             # 公共头文件（define.h、loader.hpp、error_code.h）
│   ├── framework/          # 框架核心（IPlugin、Pipeline、IRouterHandle 等）
│   ├── services/
│   │   ├── gateway/        # GatewayPlugin（Trie 路由转发）
│   │   ├── router/         # RouterAgencyPlugin（路由收集 + HTTP 分发）
│   │   ├── web/            # WebPlugin（静态文件 + API 代理）
│   │   ├── scheduler/      # SchedulerPlugin（SQL 执行 + 通道管理）
│   │   ├── task/           # TaskPlugin（任务管理与执行入口）
│   │   ├── database/       # DatabasePlugin（MySQL/SQLite/PostgreSQL/ClickHouse）
│   │   ├── stream/         # StreamPlugin（流通道实例管理与持久化）
│   │   ├── builtin/        # BuiltinPlugin（内置通道/算子注册）
│   │   ├── catalog/        # CatalogPlugin（通道目录 + 算子目录 + /operators/*）
│   │   ├── binaddon/       # BinAddonHostPlugin（C++ 算子插件管理）
│   │   └── bridge/         # BridgePlugin（C++ ↔ Python 桥接）
│   ├── plugins/
│   │   ├── baseline/       # Baseline 插件（在线基线、bootstrap、relation rolling/fusion）
│   │   └── npi/            # NPI 协议识别
│   ├── python/             # Python Worker（FastAPI）
│   ├── frontend/           # Vue.js 前端
│   └── tests/
├── docs/                   # 设计文档
├── samples/                # 开发者样例工程（如 C++ 算子插件）
└── tasks/                  # Sprint 任务管理
```

## 文档

- [项目愿景](docs/vision.md)
- [架构设计](docs/framework.md)
- [Baseline 插件说明](src/plugins/baseline/README.md)
- [C++ 算子插件 Sample](samples/cpp_operator/README.md)

## 许可证

MIT License
