# Stage 2：C++ ↔ Python 桥接 + Web 管理系统

## Context

Stage 1 已完成 C++ 框架核心（IDataEntity、IDataFrame、IChannel、IOperator、DataFrame/Arrow、Pipeline、Service、PluginRegistry）。Stage 2 需要实现三大模块：
1. **模块 P**：插件系统增强 — PluginLoader 三阶段加载 + PluginRegistry 动态注册/注销，为 Bridge 和未来 C++ 动态算子提供基础
2. **模块 A**：C++ ↔ Python 桥接 — 让 Python 算子（pandas/scipy/sklearn）能通过统一的 IOperator 接口被 Pipeline 调度
3. **模块 B**：Web 管理系统 — httplib 后端 + Vue.js 前端，提供通道/算子/任务管理和 SQL 执行界面

## 目录结构

```
src/
├── bridge/                             # 模块 A: C++ ↔ Python 桥接
│   ├── CMakeLists.txt
│   ├── arrow_ipc_serializer.h/.cpp     # Arrow IPC 序列化工具
│   ├── python_operator_bridge.h/.cpp   # PythonOperatorBridge（实现 IOperator）
│   ├── python_process_manager.h/.cpp   # Python Worker 进程管理
│   ├── bridge_plugin.h/.cpp            # BridgePlugin（IPlugin + IModule，生命周期管理）
│   └── plugin_register.cpp             # 桥接插件注册入口（仅注册，不启动）
│
├── python/                             # Python Worker + 算子框架
│   ├── flowsql/
│   │   ├── __init__.py
│   │   ├── worker.py                   # FastAPI Worker 主入口
│   │   ├── operator_base.py            # 算子基类
│   │   ├── operator_registry.py        # 算子注册中心
│   │   ├── arrow_codec.py              # Arrow IPC 编解码
│   │   └── config.py                   # Worker 配置
│   ├── operators/                      # 内置示例算子
│   │   ├── __init__.py
│   │   └── explore_chisquare.py        # 卡方分析示例
│   ├── requirements.txt
│   └── setup.py
│
├── web/                                # 模块 B: Web 管理系统
│   ├── CMakeLists.txt
│   ├── web_server.h/.cpp               # httplib 服务器主类
│   ├── api/
│   │   ├── auth_api.h/.cpp             # 认证 API
│   │   ├── channel_api.h/.cpp          # 通道管理 API
│   │   ├── operator_api.h/.cpp         # 算子管理 API
│   │   ├── task_api.h/.cpp             # 任务管理 API
│   │   ├── sql_api.h/.cpp              # SQL 执行 API
│   │   └── health_api.h/.cpp           # 健康检查
│   ├── middleware/
│   │   └── auth_middleware.h           # JWT 认证中间件
│   ├── db/
│   │   ├── database.h/.cpp             # SQLite 封装
│   │   └── schema.sql                  # 建表 DDL
│   └── static/                         # Vue.js 构建产物目录
│
├── web-ui/                             # Vue.js 前端源码（不参与 C++ 编译）
│   ├── package.json
│   ├── vite.config.js
│   ├── index.html
│   └── src/
│       ├── main.js / App.vue / router/
│       ├── views/  (Login, Dashboard, Channels, Operators, Tasks, SqlWorkbench)
│       └── components/ (Sidebar, DataTable, SqlEditor)
│
├── thirdparts/
│   ├── httplib/httplib-config.cmake     # 新增：cpp-httplib (header-only)
│   └── sqlite/sqlite-config.cmake      # 新增：SQLite amalgamation
│
├── tests/
│   ├── test_bridge/                    # 新增：桥接测试
│   └── test_web/                       # 新增：Web API 测试
│
└── CMakeLists.txt                      # 修改：新增 bridge/web 子目录
```

---

## 模块 P：插件系统增强

### 问题背景

现有 PluginLoader 的注册模型是静态的：接口实例在 `pluginregist()` 中以 `static` 变量创建，数量在编译时确定。这无法满足以下场景：
- **Python 桥接**：算子数量取决于 Python 侧运行时发现的算子，`pluginregist()` 时还不知道有哪些
- **C++ 动态算子**：未来可能通过配置文件或 Web 管理界面动态注册/注销 C++ 算子
- **插件间依赖**：Bridge 插件的 `pluginregist()` 中启动 Python Worker 是阻塞操作（3-5 秒），串行加载模型下会拖慢所有后续插件

### 改动 1：PluginLoader 三阶段加载

现有 `loader.hpp` 已定义 `IModule` 接口（`Start()`/`Stop()`）但未使用。启用三阶段加载：

```
阶段 1: pluginregist()    → 注册接口（轻量，不做重活）
阶段 2: IPlugin::Load()   → 插件初始化（读配置、准备资源，但不启动服务）
阶段 3: IModule::Start()  → 启动服务（Python Worker、Web Server 等）
```

修改 `PluginLoader::Load()` 末尾，取消注释已有的 Traverse 逻辑并改为调用 `IModule::Start()`：

```cpp
int PluginLoader::Load(const char* fullpath[], int count) {
    // 阶段 1+2: 现有逻辑不变
    for (int pos = 0; pos < count; ++pos) {
        // dlopen → pluginregist → plugin->Load()
        // ...（现有代码）
    }

    // 阶段 3: 所有插件注册完成后，统一启动 IModule
    this->Traverse(IID_MODULE, [](void* imod) {
        return reinterpret_cast<IModule*>(imod)->Start();
    });
    return 0;
}
```

对应地，`Unload()` 中在调用 `IPlugin::Unload()` 之前先调用 `IModule::Stop()`：

```cpp
int PluginLoader::Unload() {
    // 先停止所有 IModule
    this->Traverse(IID_MODULE, [](void* imod) {
        return reinterpret_cast<IModule*>(imod)->Stop();
    });
    // 再卸载所有 IPlugin（现有逻辑）
    this->Traverse(IID_PLUGIN, [](void* imod) {
        return reinterpret_cast<IPlugin*>(imod)->Unload();
    });
    // ...
}
```

### 改动 2：PluginRegistry 通用动态注册/注销

重构 `PluginRegistry`，基于已有的 GUID（IID）体系做泛化，不再为每种插件类型写专用方法：

```cpp
class PluginRegistry {
public:
    // 静态插件（从 PluginLoader 扫描）
    int LoadPlugin(const std::string& path);
    void UnloadAll();

    // 动态注册/注销 — 通用接口，按 IID + key 管理
    void Register(const Guid& iid, const std::string& key, std::shared_ptr<void> instance);
    void Unregister(const Guid& iid, const std::string& key);

    // 通用查询 — 合并静态 + 动态，动态优先
    void* Get(const Guid& iid, const std::string& key);
    void Traverse(const Guid& iid, std::function<int(void*)> callback);

private:
    PluginLoader* loader_;
    bool index_built_ = false;

    // 静态插件索引（BuildIndex 从 PluginLoader 扫描构建）
    // key = IID, value = { "catelog.name" → void* }
    std::map<Guid, std::unordered_map<std::string, void*>> static_index_;

    // 动态插件（shared_ptr<void> 管理生命周期，析构时自动调用正确的 deleter）
    std::map<Guid, std::unordered_map<std::string, std::shared_ptr<void>>> dynamic_index_;

    void BuildIndex();
};
```

查询合并逻辑：

```cpp
void* PluginRegistry::Get(const Guid& iid, const std::string& key) {
    BuildIndex();

    // 动态优先
    auto dit = dynamic_index_.find(iid);
    if (dit != dynamic_index_.end()) {
        auto it = dit->second.find(key);
        if (it != dit->second.end()) return it->second.get();
    }

    // 静态
    auto sit = static_index_.find(iid);
    if (sit != static_index_.end()) {
        auto it = sit->second.find(key);
        if (it != sit->second.end()) return it->second;
    }
    return nullptr;
}
```

调用方按需 cast：

```cpp
// Pipeline 获取算子
auto* op = static_cast<IOperator*>(registry->Get(IID_OPERATOR, "explore.chisquare"));

// Bridge 动态注册 Python 算子
auto bridge = std::make_shared<PythonOperatorBridge>(catelog, name, client);
registry->Register(IID_OPERATOR, catelog + "." + name, bridge);

// Bridge 注销
registry->Unregister(IID_OPERATOR, "explore.chisquare");

// Web 模块查询通道
auto* ch = static_cast<IChannel*>(registry->Get(IID_CHANNEL, "pcap.eth0"));

// 遍历所有算子（静态 + 动态合并）
registry->Traverse(IID_OPERATOR, [](void* iface) {
    auto* op = static_cast<IOperator*>(iface);
    printf("%s.%s\n", op->Catelog(), op->Name());
    return 0;
});
```

### 设计要点

| 关注点 | 方案 |
|--------|------|
| 类型无关 | 基于 IID + key 泛化，新增插件类型无需修改 PluginRegistry 接口 |
| 生命周期 | 静态插件由 PluginLoader 管（static 变量 + dlclose）；动态插件由 `shared_ptr<void>` 管，构造时记住原始类型 deleter，注销时自动正确析构 |
| 对 Pipeline 透明 | `Get()` 返回 `void*`，调用方 `static_cast` 到具体接口，与 PluginLoader 的 `First()` 使用方式一致 |
| 线程安全 | Stage 2 暂为单线程使用，后续可加 `std::shared_mutex` 保护 `dynamic_index_` |
| 向后兼容 | 现有静态插件（NPI、example）的注册流程完全不变，`BuildIndex` 从 PluginLoader 扫描构建 `static_index_` |

### 涉及文件

| 文件 | 改动 |
|------|------|
| `common/loader.hpp` | `Load()` 末尾启用 IModule::Start() 遍历；`Unload()` 前置 IModule::Stop() |
| `framework/core/plugin_registry.h` | 重构为通用 Register/Unregister/Get/Traverse 接口 + `static_index_`/`dynamic_index_` 双层存储 |
| `framework/core/plugin_registry.cpp` | 实现通用注册/注销 + BuildIndex + 查询/遍历合并逻辑 |

---

## 模块 A：C++ ↔ Python 桥接

### 架构

```
C++ Pipeline                                    Python Worker (FastAPI)
┌──────────────────┐   POST /work/cat/name     ┌──────────────────────┐
│ PythonOperator   │ ──── Arrow IPC stream ──→  │ OperatorRegistry     │
│ Bridge           │                            │   → operator.Work()  │
│ (IOperator impl) │ ←── Arrow IPC stream ────  │   → df_out           │
└──────────────────┘   200 OK                   └──────────────────────┘
```

### 关键组件

#### 1. ArrowIpcSerializer (`bridge/arrow_ipc_serializer.h/.cpp`)
- `Serialize(RecordBatch) → std::string`：使用 `arrow::ipc::MakeStreamWriter` + `BufferOutputStream`
- `Deserialize(string) → RecordBatch`：使用 `BufferReader` + `RecordBatchStreamReader`
- 复用 Stage 1 已有的 Arrow 依赖（`ARROW_IPC=ON` 已开启）

#### 2. PythonOperatorBridge (`bridge/python_operator_bridge.h/.cpp`)
- 实现 `IOperator` 接口，`Work()` 方法：
  1. `in->ToArrow()` 零拷贝获取 RecordBatch
  2. `ArrowIpcSerializer::Serialize()` 序列化
  3. HTTP POST 到 `http://{host}:{port}/work/{catelog}/{name}`，Content-Type: `application/vnd.apache.arrow.stream`
  4. 响应 body → `ArrowIpcSerializer::Deserialize()` → `out->FromArrow()` 零拷贝写入
- HTTP 客户端使用 cpp-httplib（与 Web 模块共用同一依赖）
- 超时配置：连接 5s，读取 30s（可通过 Configure 调整）

#### 3. PythonProcessManager (`bridge/python_process_manager.h/.cpp`)
- `Start(host, port, operators_dir, python_path)`：`fork()` + `execvp()` 启动 `python3 -m flowsql.worker`
- `WaitReady(timeout_ms)`：轮询 GET `/health` 直到 200 或超时
- `Stop()`：SIGTERM → 等 2s → SIGKILL
- `IsAlive()`：`kill(pid_, 0)` 检查

#### 4. BridgePlugin (`bridge/bridge_plugin.h/.cpp`)
- 实现 `IPlugin` + `IModule` 接口，作为桥接插件的生命周期管理器
- 持有 `PythonProcessManager` 和 `PluginRegistry*` 引用
- 三阶段加载：
  - `pluginregist()`：仅注册 BridgePlugin 到 IID_PLUGIN + IID_MODULE，立即返回
  - `Load()`：读取配置（Python 路径、Worker 端口等），不启动进程
  - `Start()`：启动 Python Worker → WaitReady → GET `/operators` → 为每个算子创建 `PythonOperatorBridge` 实例 → 通过 `PluginRegistry::RegisterOperator(shared_ptr)` 动态注册
  - `Stop()`：逐个 `UnregisterOperator` 注销动态算子 → 停止 Python Worker

```cpp
class BridgePlugin : public IPlugin, public IModule {
    PythonProcessManager process_manager_;
    PluginRegistry* registry_;  // 通过 getiquerier() 或构造注入获取
    httplib::Client* client_;
    std::vector<std::string> registered_keys_;  // 已注册的 catelog.name 列表

public:
    int Load() override {
        // 读取配置，准备参数
        return 0;
    }

    int Start() override {
        process_manager_.Start(host_, port_, operators_dir_, python_path_);
        if (process_manager_.WaitReady(5000) != 0) return -1;

        // 查询 Python 算子列表，动态注册
        for (auto& info : QueryPythonOperators()) {
            auto bridge = std::make_shared<PythonOperatorBridge>(
                info.catelog, info.name, client_);
            std::string key = std::string(info.catelog) + "." + info.name;
            registry_->Register(IID_OPERATOR, key, bridge);
            registered_keys_.push_back(key);
        }
        return 0;
    }

    int Stop() override {
        for (auto& key : registered_keys_) {
            registry_->Unregister(IID_OPERATOR, key);
        }
        registered_keys_.clear();
        process_manager_.Stop();
        return 0;
    }

    int Unload() override { return 0; }
};
```

#### 5. 插件注册入口 (`bridge/plugin_register.cpp`)
- `pluginregist()` 仅注册 BridgePlugin 实例到 IID_PLUGIN + IID_MODULE，不启动任何进程
- 重活全部延迟到 `Start()` 阶段，不阻塞其他插件加载

```cpp
BEGIN_PLUGIN_REGIST(BridgePlugin)
    ____INTERFACE(IID_PLUGIN, IPlugin)
    ____INTERFACE(IID_MODULE, IModule)
END_PLUGIN_REGIST()
```

#### 6. Python Worker (`python/flowsql/worker.py`)
- FastAPI 应用，uvicorn 运行
- `POST /work/{catelog}/{name}`：接收 Arrow IPC → pyarrow → pandas DataFrame → operator.Work(df) → Arrow IPC 返回
- `GET /operators`：返回已注册算子列表
- `GET /health`：健康检查
- 支持 JSON 调试模式（`?format=json`）

#### 7. Python 算子框架 (`python/flowsql/operator_base.py`)
- `OperatorBase` 抽象基类：`Attribute` 属性 + `Work(df_in) → df_out` 方法
- `OperatorRegistry`：扫描 operators 目录，自动发现 OperatorBase 子类并实例化注册
- `arrow_codec.py`：`decode_arrow_ipc(bytes) → DataFrame`，`encode_arrow_ipc(DataFrame) → bytes`

### 错误处理
| 场景 | 策略 |
|------|------|
| Worker 未启动 | WaitReady 超时 → 返回错误码 |
| HTTP 超时 | httplib 超时设置，返回 -1 |
| Python 算子异常 | Worker 返回 500 + 错误信息，Bridge 记录日志返回 -1 |
| Worker 崩溃 | IsAlive 检测 → 自动重启（最多 3 次） |

---

## 模块 B：Web 管理系统

### 技术选型
- 后端：cpp-httplib（header-only，与 Bridge 共用），独立可执行文件 `flowsql_web`
- 数据库：SQLite amalgamation（单文件编译，无外部依赖）
- 认证：SHA256 密码哈希 + JWT token（简单实现，无需额外库）
- 前端：Vue 3 + Vite + Vue Router + Axios + Element Plus（UI 组件库）

### 数据库 Schema (`web/db/schema.sql`)
```sql
-- users: 用户认证
-- channels: 通道注册（name, type, catelog, profile JSON, status）
-- operators: 算子注册（name, catelog, position, operator_file, active）
-- data_entities: 数据实体定义（name, schema JSON, description）
-- tasks: 任务管理（sql, status, create_time, start_time, end_time）
```

### REST API
| 模块 | 端点 | 说明 |
|------|------|------|
| 认证 | POST /api/auth/login, /logout | JWT token |
| 通道 | GET/POST /api/channels, PUT/DELETE /api/channels/{name} | CRUD + 启停 |
| 算子 | GET/POST /api/operators, /activate, /deactivate | 注册 + 激活控制 |
| 任务 | GET/POST /api/tasks, /start, /stop, /pause, /resume | 生命周期管理 |
| SQL | POST /api/sql/execute | SQL 语句执行 |
| 系统 | GET /api/health, /api/stats | 健康检查 + 统计 |

### WebServer 架构 (`web/web_server.h/.cpp`)
- 持有 `httplib::Server`、`Database`、`PluginRegistry*` 引用
- 各 API 模块（auth_api、channel_api 等）注册路由到 Server
- 认证中间件：拦截非 /api/auth 和 /api/health 请求，验证 JWT
- 静态文件服务：`/` 路径映射到 `static/` 目录（Vue.js 构建产物）

### 前端页面
- Login：用户登录
- Dashboard：系统概览
- Channels：通道列表 + 创建/配置/启停
- Operators：算子列表 + 注册/激活
- Tasks：任务列表 + 创建/控制
- SqlWorkbench：SQL 编辑器 + 执行结果展示

---

## 新增第三方依赖

### cpp-httplib (`thirdparts/httplib/httplib-config.cmake`)
- Header-only，下载 v0.18.3 的 httplib.h 即可
- 设置 `httplib_LINK_INC`，无需 `httplib_LINK_TAR`
- 模块 A（HTTP Client）和模块 B（HTTP Server）共用

### SQLite (`thirdparts/sqlite/sqlite-config.cmake`)
- 下载 amalgamation，编译为静态库 libsqlite3.a
- 设置 `sqlite_LINK_INC`、`sqlite_LINK_DIR`、`sqlite_LINK_TAR`

---

## 构建集成

### src/CMakeLists.txt 修改
```cmake
# Stage 2: C++ ↔ Python 桥接 + Web 管理系统
add_subdirectory(${CMAKE_SOURCE_DIR}/bridge ${CMAKE_BINARY_DIR}/bridge)
add_subdirectory(${CMAKE_SOURCE_DIR}/web ${CMAKE_BINARY_DIR}/web)
add_subdirectory(${CMAKE_SOURCE_DIR}/tests/test_bridge ${CMAKE_BINARY_DIR}/test_bridge)
add_subdirectory(${CMAKE_SOURCE_DIR}/tests/test_web ${CMAKE_BINARY_DIR}/test_web)
```

### 构建产物
- `libflowsql_bridge.so` — 桥接插件（动态库，PluginLoader 加载）
- `flowsql_web` — Web 服务独立可执行文件（监听 8081 端口）
- `test_bridge` / `test_web` — 测试可执行文件

---

## 实施顺序

### 步骤 1：插件系统增强（模块 P）
- 修改 `common/loader.hpp`：`Load()` 末尾启用 IModule::Start() 遍历，`Unload()` 前置 IModule::Stop()
- 修改 `framework/core/plugin_registry.h/.cpp`：新增动态注册/注销方法 + 查询合并逻辑
- 测试：现有 test_framework 通过（静态插件不受影响）+ 新增动态注册/注销单元测试

### 步骤 2：新增第三方依赖
- `thirdparts/httplib/httplib-config.cmake`
- `thirdparts/sqlite/sqlite-config.cmake`
- 编译验证依赖可用

### 步骤 3：Arrow IPC 序列化工具
- `bridge/arrow_ipc_serializer.h/.cpp`
- 单元测试：RecordBatch → serialize → deserialize → 验证数据一致

### 步骤 4：Python Worker + 算子框架
- `python/flowsql/` 全部文件
- `python/operators/explore_chisquare.py` 示例算子
- `python/requirements.txt`（fastapi, uvicorn, pyarrow, pandas）
- 手动启动 Worker 验证 /health 和 /operators 端点

### 步骤 5：BridgePlugin + PythonOperatorBridge + 进程管理
- `bridge/bridge_plugin.h/.cpp`（IPlugin + IModule 生命周期管理）
- `bridge/python_operator_bridge.h/.cpp`
- `bridge/python_process_manager.h/.cpp`
- `bridge/plugin_register.cpp`（仅注册 IID_PLUGIN + IID_MODULE）
- `bridge/CMakeLists.txt`
- 测试：加载桥接插件 → Start() 自动启动 Python Worker → 动态注册算子 → 调用验证 → Stop() 注销

### 步骤 6：SQLite 数据库层
- `web/db/database.h/.cpp`
- `web/db/schema.sql`
- 测试：建表、CRUD 操作

### 步骤 7：Web 后端 API
- `web/web_server.h/.cpp`
- `web/api/` 各 API 模块
- `web/middleware/auth_middleware.h`
- `web/CMakeLists.txt`
- 测试：curl 验证各 API 端点

### 步骤 8：Vue.js 前端
- `web-ui/` 全部文件
- `npm run build` → 产物复制到 `web/static/`
- 测试：浏览器访问验证

### 步骤 9：集成测试
- `tests/test_bridge/` — 桥接端到端测试
- `tests/test_web/` — Web API 测试
- 修改 `src/CMakeLists.txt` 集成所有新模块

---

## 验证方案

```bash
# 1. 编译
cd src/build && rm -rf * && cmake .. && make -j$(nproc)

# 2. 安装 Python 依赖
pip install -r src/python/requirements.txt

# 3. 桥接测试
./output/test_bridge
# 验证：Arrow IPC 序列化/反序列化 + Python Worker 启动 + 算子调用

# 4. Web 测试
./output/test_web
# 验证：SQLite CRUD + API 端点

# 5. 端到端验证
./output/flowsql_web &
# 浏览器访问 http://localhost:8081
# 登录 → 查看通道/算子列表 → 执行 SQL

# 6. 现有测试不受影响
./output/test_npi
./output/test_framework
```

---

## 关键文件清单

### 需修改的现有文件
- `src/common/loader.hpp` — Load() 启用 IModule::Start() 三阶段加载，Unload() 前置 IModule::Stop()
- `src/framework/core/plugin_registry.h/.cpp` — 重构为通用 Register/Unregister/Get/Traverse 接口 + IID 双层索引
- `src/CMakeLists.txt` — 新增 bridge/web 子目录

### 需新建的文件（约 32 个）
- `src/bridge/` — 8 个 C++ 文件 + CMakeLists.txt（含 bridge_plugin.h/.cpp）
- `src/python/flowsql/` — 6 个 Python 文件
- `src/python/operators/` — 2 个 Python 文件
- `src/python/requirements.txt` + `setup.py`
- `src/web/` — 约 15 个 C++ 文件 + CMakeLists.txt + schema.sql
- `src/web-ui/` — Vue.js 项目文件
- `src/thirdparts/httplib/` + `sqlite/` — 2 个 cmake 文件
- `src/tests/test_bridge/` + `test_web/` — 各 2 个文件

### 复用的现有文件
| 文件 | 复用内容 |
|------|---------|
| `framework/interfaces/ioperator.h` | IOperator 接口，Bridge 实现此接口 |
| `framework/interfaces/idataframe.h` | IDataFrame 接口，ToArrow/FromArrow |
| `framework/core/dataframe.h/.cpp` | DataFrame 实现 |
| `framework/core/plugin_registry.h/.cpp` | 插件注册中心（需重构：通用 IID 动态注册） |
| `common/loader.hpp` | PluginLoader（需修改：启用三阶段加载） |
| `plugins/example/plugin_register.cpp` | 插件注册模式参考 |
| `thirdparts/arrow/arrow-config.cmake` | Arrow 依赖配置参考 |
| `subjects.cmake` | add_thirddepen 宏 |
