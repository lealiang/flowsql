# Stage 2：C++ ↔ Python 桥接 + Web 管理系统

## Context

Stage 1 已完成 C++ 框架核心（IDataEntity、IDataFrame、IChannel、IOperator、DataFrame/Arrow、Pipeline、Service、PluginRegistry）。Stage 2 需要实现三大模块：
1. **模块 P**：插件系统增强 — PluginLoader 三阶段加载 + PluginRegistry 动态注册/注销，为 Bridge 和未来 C++ 动态算子提供基础
2. **模块 A**：C++ ↔ Python 桥接 — 让 Python 算子（pandas/scipy/sklearn）能通过统一的 IOperator 接口被 Pipeline 调度
3. **模块 B**：Web 管理系统 — httplib 后端 + Vue.js 前端，提供通道/算子/任务管理和 SQL 执行界面

## 核心接口体系

FlowSQL 的插件化架构建立在以下四个核心接口之上，通过 GUID 标识、PluginLoader 加载、PluginRegistry 索引，形成完整的组件生命周期和数据流通机制。

```
IPlugin（生命周期基元）
├── IChannel（数据通道）    ← Pipeline 的源和汇
├── IOperator（数据算子）   ← Pipeline 的计算节点
└── ...（可扩展）

IModule（启停控制）         ← 需要后台运行的组件额外实现

IDataEntity（数据实体）     ← 单行数据的抽象
IDataFrame（数据帧）        ← 列式数据集，Arrow RecordBatch 后端
```

### IPlugin — 插件生命周期基元

定义：`common/loader.hpp` | 标识：`IID_PLUGIN`

所有可加载组件的根接口。每个 .so 插件通过 `pluginregist()` 注册时，至少注册一个 IPlugin 实例。IChannel 和 IOperator 都继承自它，因此每个通道和算子天然具备生命周期管理能力。

| 方法 | 职责 |
|------|------|
| `Load()` | 插件初始化（三阶段加载的阶段 2），此时不应调用其他接口 |
| `Unload()` | 插件清理，同样不应调用其他接口 |
| `Option(const char*)` | 接收加载时传入的配置参数 |

### IModule — 服务启停控制

定义：`common/loader.hpp` | 标识：`IID_MODULE`

用于需要"运行态"的组件（如后台进程、网络监听）。与 IPlugin 正交——一个插件类可以同时实现两者（如 BridgePlugin），也可以只实现 IPlugin。

| 方法 | 职责 |
|------|------|
| `Start()` | 启动服务（三阶段加载的阶段 3，所有 IPlugin::Load 完成后） |
| `Stop()` | 停止服务（Unload 前逆序调用） |

PluginLoader 保证：先注册 → 再初始化 → 最后启动，Start 失败时逆序回滚已启动的模块。

### IChannel — 数据通道

定义：`framework/interfaces/ichannel.h` | 标识：`IID_CHANNEL` | 继承：IPlugin

代表数据的入口或出口。`Catelog()` + `Name()` 构成唯一标识（如 `"example.memory"`）。基类只定义生命周期、身份和元数据，**数据读写方法由子类定义**（IDataFrameChannel、IDatabaseChannel 等）。

| 方法 | 职责 |
|------|------|
| `Open()` / `Close()` / `IsOpened()` | 通道生命周期 |
| `Flush()` | 批量刷新 |
| `Catelog()` / `Name()` | 身份标识 |
| `Type()` | 通道类型（"dataframe"、"database" 等） |
| `Schema()` | 元数据描述（格式由实现决定） |

子类型：`IDataFrameChannel`（Read/Write IDataFrame）、`IDatabaseChannel`（CreateReader/CreateWriter 工厂）。算子通过 `dynamic_cast` 获取具体子类型。

### IOperator — 数据算子

定义：`framework/interfaces/ioperator.h` | 标识：`IID_OPERATOR` | 继承：IPlugin

代表一个数据处理单元，接收通道输入、产出通道输出。算子内部通过 `dynamic_cast` 获取具体通道类型，自行完成数据读写。

| 方法 | 职责 |
|------|------|
| `Work(IChannel* in, IChannel* out)` | 核心处理方法，算子自行从 in 通道读取、向 out 通道写入 |
| `Position()` | 算子位置（STORAGE 或 DATA 层） |
| `Configure(key, value)` | 运行时调参 |

Pipeline 只负责将 source channel 和 sink channel 交给算子，不关心通道类型和数据格式。不同算子根据自身职责 cast 到所需的通道子类型：

```
计算算子（Python）：  IDataFrameChannel → Work → IDataFrameChannel
存储算子（system.store）：  IDataFrameChannel → Work → IDatabaseChannel
提取算子（system.extract）：IDatabaseChannel  → Work → IDataFrameChannel
```

PythonOperatorBridge 在 Work() 内部完成 `dynamic_cast<IDataFrameChannel*>` + Read/Write + Arrow IPC 序列化，对 Python 算子开发者完全透明。

### 数据流通路径

```
┌─────────┐              ┌───────────┐              ┌─────────┐
│ IChannel │  ─────────→  │ IOperator │  ─────────→  │ IChannel│
│ (source) │   Work(in,   │  算子内部  │   out)       │ (sink)  │
└─────────┘   out)        │ 自行读写   │              └─────────┘
                          └───────────┘
                               │
                          Pipeline 只做连接
```

---

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

## 模块 P：插件系统增强（已实现）

### 发现的问题与修复

| 编号 | 级别 | 问题 | 修复方案 |
|------|------|------|---------|
| P0-1 | 必须 | Start() 失败无回滚，已启动模块泄漏 | StartModules() 失败时逆序 Stop() 已启动模块 |
| P0-2 | 必须 | fntraverse 是 C 函数指针，无法捕获上下文 | PluginRegistry 通过 GetInterfaces() 直接遍历，消除 static hack |
| P0-3 | 必须 | 泛化 Get() 丢失类型安全 | 保留类型安全模板 `Get<T>(iid, key)` + 向后兼容 GetChannel/GetOperator |
| P0-4 | 必须 | Unload 后 ifs_ref_ 未清理，悬空指针 | Unload() 末尾 clear plugins_ref_ + ifs_ref_ |
| P1-5 | 应该 | Load() 只调用返回的那一个 IPlugin | 记录注册前后 IPlugin 数量差值，遍历所有新注册 IPlugin 调用 Load() |
| P1-6 | 应该 | PluginRegistry 多实例 vs PluginLoader 单例 | PluginRegistry 改为单例 Instance() |
| P2-7 | 约定 | PythonOperatorBridge 继承链 | 动态注册只注册 IID_OPERATOR，Load/Unload 空实现 |

### 改动 1：PluginLoader 改造（loader.hpp）

**三阶段加载**：
```
阶段 1: pluginregist()    → 注册接口（轻量）
阶段 2: IPlugin::Load()   → 插件初始化（遍历本次 .so 注册的所有 IPlugin）
阶段 3: IModule::Start()  → 启动服务（失败时逆序回滚）
```

**新增方法**：
- `GetInterfaces(iid)` — 只读访问接口列表，供 PluginRegistry::BuildIndex 使用
- `StartModules()` — 遍历 IID_MODULE 逐个 Start()，失败时逆序 Stop()
- `StopModules()` — 逆序遍历 IID_MODULE 逐个 Stop()

**Unload 改进**：先 StopModules → 再 IPlugin::Unload → freelibrary → 清理 ifs_ref_/plugins_ref_

### 改动 2：PluginRegistry 重构（plugin_registry.h/.cpp）

**核心变化**：
- 单例模式 `PluginRegistry::Instance()`
- 双层索引：`static_index_`（从 PluginLoader 扫描）+ `dynamic_index_`（shared_ptr 管理生命周期）
- 统一 `Traverse(iid, std::function<int(void*)>)` 替代分散的 TraverseChannels/TraverseOperators
- 动态注册 `Register(iid, key, shared_ptr<void>)` / `Unregister(iid, key)`
- BuildIndex 通过 `loader_->GetInterfaces()` 直接遍历，无 static 变量 hack
- 查询合并：动态优先，遍历时跳过已被动态覆盖的静态 key
- 类型安全模板 `Get<T>(iid, key)` + `Traverse<T>(iid, callback)`
- 向后兼容内联：GetChannel/GetOperator/TraverseChannels/TraverseOperators

### 改动 3：适配层

- `service.h/.cpp`：`PluginRegistry registry_` → `PluginRegistry* registry_`，通过 Instance() 获取
- `test_framework/main.cpp`：适配单例 + 新增 test_dynamic_register 验证动态注册/注销/合并遍历

### 涉及文件

| 文件 | 改动类型 |
|------|---------|
| `common/loader.hpp` | 修改：GetInterfaces + StartModules/StopModules + Load 多 IPlugin + Unload 清理 |
| `framework/core/plugin_registry.h` | 重构：单例 + 统一 Traverse + 泛型模板 + 动态注册 + 双层索引 |
| `framework/core/plugin_registry.cpp` | 重构：BuildIndex 用 GetInterfaces + Traverse 合并查询 + Register/Unregister |
| `framework/core/service.h` | 小改：registry_ 改为指针 |
| `framework/core/service.cpp` | 小改：适配指针调用 |
| `tests/test_framework/main.cpp` | 小改：适配单例 + 新增动态注册测试 |

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
- 实现 `IOperator` 接口，`Work(IChannel* in, IChannel* out)` 方法：
  1. `dynamic_cast<IDataFrameChannel*>(in)` 获取输入通道，`Read()` 读取 DataFrame
  2. `DataFrame::ToArrow()` 零拷贝获取 RecordBatch
  3. `ArrowIpcSerializer::Serialize()` 序列化
  4. HTTP POST 到 `http://{host}:{port}/work/{catelog}/{name}`，Content-Type: `application/vnd.apache.arrow.stream`
  5. 响应 body → `ArrowIpcSerializer::Deserialize()` → `DataFrame::FromArrow()` 零拷贝写入
  6. `dynamic_cast<IDataFrameChannel*>(out)` 获取输出通道，`Write()` 写入结果
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

> 本阶段聚焦框架搭建 + Python 算子的完整执行链路，不涉及 C++ 算子和异步任务。

### 本阶段范围

**做的事：**
- 通道：查询展示（预留上传激活接口）
- 算子：查询展示 + Python 算子上传激活
- 任务：Python 算子任务的创建、SQL 执行、结果查看（同步）
- Pipeline 单算子执行架构
- DataFrameChannel 实现（内存存储，预填测试数据）
- IDatabaseChannel 接口设计（IBatchReader/IBatchWriter 抽象）
- 内置存储算子（system.store）和提取算子（system.extract）的算子类框架（Work 签名、Configure、dynamic_cast 骨架），不含实际数据库读写逻辑

**不做的事：**
- 异步任务执行（下阶段）
- SQL 列选择（`SELECT col1, col2`）
- C++ 算子 / 网卡通道的实际实现
- DatabaseChannel 具体实现（ClickHouse 等，下阶段）
- DataEntity schema 校验（算子自行处理不匹配情况）

---

### 架构设计决策

#### 1. IChannel 重构

IChannel 基类只保留生命周期 + 身份 + 元数据，**数据读写方法下沉到子类**，算子通过 `dynamic_cast` 获取具体通道类型。

```cpp
// IChannel 基类 — 通用抽象
interface IChannel : public IPlugin {
    // 生命周期
    virtual int Open() = 0;
    virtual int Close() = 0;
    virtual bool IsOpened() = 0;
    virtual int Flush() = 0;

    // 身份
    virtual const char* Catelog() = 0;
    virtual const char* Name() = 0;

    // 类型与元数据 — 具体格式由实现决定
    virtual const char* Type() = 0;       // "dataframe", "packet", ...
    virtual const char* Schema() = 0;     // 返回描述字符串（JSON 等）
};
```

不同通道类型在子类中定义各自的数据读写方法：

```cpp
// DataFrame 通道 — 承载 DataFrame
class IDataFrameChannel : public IChannel {
public:
    virtual int Write(IDataFrame* df) = 0;
    virtual int Read(IDataFrame* df) = 0;
};

// 未来：PacketChannel — 承载原始数据包
// class IPacketChannel : public IChannel { ... };
```

DataFrameChannel（内存实现）的读写语义：
- `Read()` — **快照语义**（非破坏性），每次返回当前存储的完整 DataFrame 副本，可被多次 SQL 引用
- `Write()` — **替换语义**，覆盖当前存储内容为新的 DataFrame

##### IDatabaseChannel — 数据库通道子接口

数据库通道（ClickHouse 等列式存储）通过工厂模式创建 Reader/Writer 对象，不同数据库各自实现。
交换格式统一为 Arrow IPC，与框架内 DataFrame 天然对接。

```cpp
// 写入统计信息
struct BatchWriteStats {
    int64_t rows_written;     // 写入行数
    int64_t bytes_written;    // 写入字节数（原始数据）
    int64_t elapsed_ms;       // 耗时毫秒
};

// IBatchReader — 流式读取器
// 生命周期：CreateReader() → GetSchema() → Next()... → Close() → Release()
interface IBatchReader {
    // 获取结果集 Schema（Arrow Schema IPC 序列化）
    // 返回的指针在 Reader 生命周期内有效
    virtual int GetSchema(const uint8_t** buf, size_t* len) = 0;

    // 读取下一批数据（Arrow RecordBatch IPC buffer）
    // 返回：0=有数据, 1=已读完, <0=错误
    // 返回的指针在下次 Next() 或 Close() 前有效
    virtual int Next(const uint8_t** buf, size_t* len) = 0;

    // 取消正在进行的读取（可从其他线程调用）
    virtual void Cancel() = 0;

    // 关闭读取器，释放资源
    virtual void Close() = 0;

    // 错误信息（Next 返回 <0 时调用）
    virtual const char* GetLastError() = 0;

    // 释放读取器自身
    virtual void Release() = 0;
};

// IBatchWriter — 批量写入器
// 生命周期：CreateWriter() → Write()... → Flush() → Close() → Release()
interface IBatchWriter {
    // 写入一批数据（Arrow RecordBatch IPC buffer）
    // 实现层可内部攒批，达到阈值自动 flush
    virtual int Write(const uint8_t* buf, size_t len) = 0;

    // 强制刷新缓冲区到数据库
    virtual int Flush() = 0;

    // 关闭写入器，返回统计信息（stats 可为 nullptr）
    virtual void Close(BatchWriteStats* stats) = 0;

    // 错误信息
    virtual const char* GetLastError() = 0;

    // 释放写入器自身
    virtual void Release() = 0;
};

// IDatabaseChannel — 数据库通道
// 作为工厂创建 Reader/Writer，内部管理连接
class IDatabaseChannel : public IChannel {
public:
    // 创建读取器，执行 query 并流式返回结果
    // 调用方负责最终 Release() 读取器
    virtual int CreateReader(const char* query, IBatchReader** reader) = 0;

    // 创建写入器，指定目标表
    // 调用方负责最终 Release() 写入器
    virtual int CreateWriter(const char* table, IBatchWriter** writer) = 0;

    // 测试连接是否可用
    virtual bool IsConnected() = 0;
};
```

设计要点：
- **工厂模式**：IDatabaseChannel 创建 Reader/Writer 对象，不同数据库（ClickHouse、PostgreSQL 等）各自实现 IBatchReader/IBatchWriter
- **Arrow IPC 交换格式**：Reader 输出和 Writer 输入统一为 Arrow IPC buffer，ClickHouse 原生支持 `FORMAT Arrow`，几乎零转换开销
- **内存所有权**：Reader 内部持有 buffer，`Next()` 返回的指针在下次 `Next()` 或 `Close()` 前有效；Writer 的 `Write()` 传入的 buffer 由调用方持有
- **Writer 内部攒批**：实现层可内部缓冲，攒够一定量再真正发送，`Flush()` 强制提交，对调用方透明
- **本阶段只定义接口**，具体数据库实现（ClickHouse 等）留到下阶段

##### 存储算子与提取算子

Python 算子的输入通道不一定是 DataFrame，也可能需要从数据库通道取数据；输出也可能需要保存到数据库通道。
格式转换的职责交给**专用算子**，而非让通道互相适配，这更符合 Pipeline 的哲学——数据变换是算子的本职工作。

```
通道各自纯粹：
  DataFrameChannel  — 只管 DataFrame 的读写
  DatabaseChannel   — 只管数据库的读写（ClickHouse 等）

算子负责桥接：
  计算算子（Python）：DataFrame → 计算 → DataFrame
  存储算子（system.store）：DataFrame → 写入 → Database
  提取算子（system.extract）：Database → 读取 → DataFrame
```

存储算子工作流（system.store）：
```
Work(in=DataFrameChannel, out=DataFrameChannel)
  1. dynamic_cast<IDataFrameChannel*>(in)，Read() 获取 DataFrame
  2. 解析 WITH 参数：target=clickhouse.my_table, mode=append, partition_by=date, ...
  3. 从 PluginRegistry 查找目标 IDatabaseChannel
  4. CreateWriter(table)
  5. DataFrame 序列化为 Arrow IPC → Writer::Write()
  6. Writer::Close(&stats) 获取统计信息
  7. 构造统计 DataFrame: {rows_written, bytes_written, elapsed_ms}
  8. dynamic_cast<IDataFrameChannel*>(out)，Write() 写入统计结果
```

提取算子工作流（system.extract）：
```
Work(in=DatabaseChannel, out=DataFrameChannel)
  1. dynamic_cast<IDatabaseChannel*>(in)
  2. 解析 WITH 参数：query="SELECT col1,col2 FROM t WHERE date > '2026-01-01'"
  3. CreateReader(query)
  4. 循环 Next() → 反序列化为 DataFrame
  5. dynamic_cast<IDataFrameChannel*>(out)，Write() 写入结果
  6. Reader::Close()
```

写入配置（mode、partition_by、压缩方式等）通过 SQL 的 WITH 子句传递给算子，算子再传给 Writer。
这与 Spark SQL（USING + OPTIONS）、Flink SQL（WITH 子句）的设计模式一致，是经过验证的成熟方案。

设计理由：
- Channel 是通用数据管道，承载的数据类型取决于具体实现（DataFrame、原始数据包、消息等）
- 不区分"行级"和"帧级"通道，避免破坏抽象
- 算子内部根据自身职责 `dynamic_cast` 到所需的通道子类型，Pipeline 不参与类型判断
- 本阶段实现 `DataFrameChannel`（IDataFrameChannel 的内存实现）

#### 2. IDataEntity 调整

IDataEntity 不再作为数据载体，转为**元数据声明**角色——描述通道承载的数据结构，用于算子与通道之间的数据协商。

原有的 `Put(IDataEntity*)` / `Get() → IDataEntity*` 从 IChannel 基类移除，数据读写由各子类自行定义。

元数据声明由各通道的 `Schema()` 方法承担，具体格式由实现决定：
- DataFrameChannel：Arrow Schema（列名 + 类型）
- PacketChannel：协议字段结构描述
- 其他通道：各自定义

本阶段不实现 schema 校验，算子自行处理数据不匹配的情况。

#### 3. Pipeline 架构

本阶段为**单算子执行**模型：一条 SQL 对应一个 source channel + 一个 operator + 可选 dest channel。

Pipeline 是纯粹的连接器，只负责将 source channel 和 sink channel 交给算子，**不参与数据读写**。数据的读取、转换、写入全部由算子内部完成。

```
                        SQL 解析
                           │
                           ▼
              ┌─────────────────────────┐
              │   Pipeline（单算子）     │
              │                         │
              │  source ──→ operator ──→ dest (可选)
              │  Channel    Work(in,out) Channel
              └─────────────────────────┘
                           │
                    operator 内部自行
                    从 in 读取、向 out 写入
```

执行流程：
1. SQL 解析器提取 source channel、operator、参数、dest channel
2. Pipeline 通过 PluginRegistry 查找 channel 和 operator 实例
3. 通过 `Configure()` 将 WITH 参数传递给算子
4. 若 SQL 省略 INTO，Pipeline 创建一个临时 DataFrameChannel 作为 sink
5. 调用 `operator->Work(source_channel, sink_channel)`，算子内部完成全部数据读写
6. 算子根据自身类型 `dynamic_cast` 到所需的通道子类型（IDataFrameChannel / IDatabaseChannel）
7. 若为临时 sink，Pipeline 从中 `Read()` 取出结果 DataFrame 返回调用方

多算子场景通过多条 SQL 任务串联：前一个任务 INTO result_1，后一个任务 FROM result_1。

并行执行通过多 Pipeline 实例实现（本阶段不涉及）。

#### 4. SQL 语法

```sql
SELECT * FROM <source_channel> USING <catelog.operator> [WITH key=val, ...] [INTO <result_channel>]
```

- `SELECT *`：传递所有列（本阶段不支持列选择）
- `FROM`：源数据通道名称
- `USING`：算子标识（catelog.name 格式）
- `WITH`：算子参数（可选）
- `INTO`：结果写入目标通道（可选）
  - 省略 INTO → 结果直接返回前端展示（同步）
  - 指定 INTO → 结果存入 DataFrameChannel，可供后续任务引用

示例：
```sql
-- 直接查看结果
SELECT * FROM test_data USING explore.chisquare WITH threshold=0.05

-- 结果存入通道，供后续任务使用
SELECT * FROM test_data USING explore.chisquare WITH threshold=0.05 INTO chisquare_result
SELECT * FROM chisquare_result USING explore.another_operator INTO final_result

-- 存储算子：将 DataFrame 结果写入 ClickHouse（返回写入统计）
SELECT * FROM chisquare_result USING system.store WITH target=clickhouse.my_table, mode=append, partition_by=date

-- 提取算子：从 ClickHouse 读取数据到内存 DataFrame
SELECT * FROM clickhouse.my_table USING system.extract WITH query="SELECT col1,col2 FROM t WHERE date > '2026-01-01'" INTO memory_result
```

---

### 技术选型
- 后端：cpp-httplib（header-only，与 Bridge 共用），独立可执行文件 `flowsql_web`
- 数据库：SQLite amalgamation（单文件编译，无外部依赖）
- 前端：Vue 3 + Vite + Vue Router + Axios + Element Plus（UI 组件库）
- SQL 解析：手写递归下降解析器（语法简单，无需引入 parser generator）

### 数据库 Schema (`web/db/schema.sql`)
```sql
-- channels: 通道注册（name, type, catelog, schema JSON, status）
-- operators: 算子注册（name, catelog, position, operator_file, active）
-- tasks: 任务记录（sql, status, result JSON, create_time, finish_time）
```

### REST API
| 模块 | 端点 | 说明 |
|------|------|------|
| 通道 | GET /api/channels | 查询展示（预留 POST/PUT/DELETE） |
| 算子 | GET /api/operators | 查询展示 |
| 算子 | POST /api/operators/upload | Python 算子上传 |
| 算子 | POST /api/operators/{name}/activate | 激活算子 |
| 算子 | POST /api/operators/{name}/deactivate | 停用算子 |
| 任务 | GET /api/tasks | 任务列表 |
| 任务 | POST /api/tasks | 创建任务（提交 SQL） |
| 任务 | GET /api/tasks/{id}/result | 查看执行结果 |
| 系统 | GET /api/health | 健康检查 |

### WebServer 架构 (`web/web_server.h/.cpp`)
- 持有 `httplib::Server`、`Database`、`PluginRegistry*` 引用
- 各 API 模块注册路由到 Server
- 静态文件服务：`/` 路径映射到 `static/` 目录（Vue.js 构建产物）
- 认证：本阶段不实现，预留中间件位置

### 前端页面
- Dashboard：系统概览（通道/算子/任务统计）
- Channels：通道列表展示
- Operators：算子列表 + 上传/激活控制
- Tasks：任务列表 + SQL 提交 + 结果查看

### 待下阶段实现
- DatabaseChannel 具体实现（ClickHouse 列式存储对接）
- 存储算子 / 提取算子的实际实现（依赖 DatabaseChannel）
- 异步任务执行（WebSocket 推送进度）
- 用户认证（JWT）
- 通道上传激活
- 多算子 Pipeline / 并行执行
- SQL 列选择

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

### 步骤 1：插件系统增强（模块 P）— ✅ 已完成
- PluginLoader 三阶段加载 + PluginRegistry 动态注册/注销

### 步骤 2：C++ ↔ Python 桥接（模块 A）— ✅ 已完成
- ArrowIpcSerializer、PythonOperatorBridge、PythonProcessManager、BridgePlugin
- FastAPI Worker、OperatorBase 装饰器注册、OperatorRegistry 自动发现
- httplib 第三方依赖、test_bridge 测试

### 步骤 3：IChannel 重构 + IOperator 接口调整 — ✅ 已完成
- IChannel 基类移除 Put/Get(IDataEntity*)，保留生命周期 + 身份 + Type() + Schema()
- IOperator::Work 签名改为 `Work(IChannel* in, IChannel* out)`，算子内部自行读写通道
- 新增 IDataFrameChannel 子接口（Write/Read IDataFrame）
- 新增 IDatabaseChannel 子接口（仅接口定义，本阶段不实现）
- 实现 DataFrameChannel（内存存储，快照读/替换写语义，线程安全）
- 适配现有 MemoryChannel（example 插件）→ 改为 IDataFrameChannel 实现
- 适配 PythonOperatorBridge：Work() 内部 dynamic_cast + Read/Write
- 适配 PassthroughOperator：同上
- 删除 framework/macros.h（未使用的死代码）
- test_framework 7 项测试全部通过

### 步骤 4：SQL 解析器 + Pipeline 重构 — ✅ 已完成
- Pipeline 简化为纯连接器：PipelineBuilder.SetSource/SetOperator/SetSink，Run() 只调用 `operator->Work(source, sink)`
- SQL 解析器（递归下降，解析 SELECT...FROM...USING...WITH...INTO 语法）
- 支持大小写不敏感关键字、WITH 中引号值、错误提示
- 实际实现：SQL 解析器和 Pipeline 重构合并在步骤 3 中一起完成

### 步骤 5：SQLite 数据库层 — ✅ 已完成
- `thirdparts/sqlite/sqlite-config.cmake`（amalgamation 下载编译，SQLITE_THREADSAFE=1）
- `web/db/database.h/.cpp`（SQLite 封装：Open/Close/Execute/Query/Insert + 参数化查询 ExecuteParams/InsertParams）
- `web/db/schema.sql`（channels、operators、tasks 建表）
- 使用 DELETE 日志模式（WAL 在 WSL2 Windows 挂载文件系统上不兼容）
- 数据库路径使用 `/tmp/flowsql.db`（避免 Windows 挂载盘的 I/O 问题）

### 步骤 6：Web 后端 API — ✅ 已完成
- `web/web_server.h/.cpp`（httplib Server + 路由注册，所有 Handler 集中在 WebServer 类中）
- 实际实现未拆分 api/ 子目录，所有路由处理函数直接在 web_server.cpp 中实现（代码量可控，无需过度拆分）
- 全部 API 端点已实现并通过测试：
  - GET /api/health — 健康检查
  - GET /api/channels — 通道列表（从 PluginRegistry 同步到 SQLite）
  - GET /api/operators — 算子列表
  - POST /api/operators/upload — Python 算子上传（含路径穿越防护）
  - POST /api/operators/{name}/activate|deactivate — 算子激活/停用
  - GET /api/tasks — 任务列表
  - POST /api/tasks — 创建任务（SQL 解析 → Pipeline 执行 → 结果存储）
  - GET /api/tasks/{id}/result — 查看执行结果
- INTO 子句支持动态创建 DataFrameChannel 并注册到 PluginRegistry
- 所有 SQL 操作使用参数化查询（防 SQL 注入）
- JSON 响应使用 rapidjson 构造（防特殊字符破坏格式）
- CORS 支持（Access-Control-Allow-Origin: *）
- `web/main.cpp`（flowsql_web 入口：加载插件 → 预填 20 行测试数据 → 初始化 WebServer → 监听 8081）
- `web/CMakeLists.txt`（编译为 flowsql_web 可执行文件，链接 framework + arrow + httplib + rapidjson + sqlite）

### 步骤 7：Vue.js 前端 ✅
- `web-ui/` 项目脚手架（Vue 3 + Vite + Element Plus）
- Dashboard、Channels、Operators、Tasks 四个页面
- `npm run build` → 产物复制到 `web/static/`
- API 封装适配后端数组格式（channels/operators/tasks 直接返回数组）
- 数据格式转换（schema_json 解析、active 字段布尔转换、任务结果行列转换）
- 测试脚本 `test_frontend.sh` 验证全部功能通过

### 步骤 8：集成测试
- `tests/test_web/` — Web API 测试
- 端到端验证：启动 flowsql_web → 上传算子 → 提交 SQL → 查看结果
- 现有测试不受影响（test_npi、test_framework、test_bridge）

---

## 验证方案

### 单元测试

```bash
# 1. 编译
cd src/build && rm -rf * && cmake .. && make -j$(nproc)

# 2. 现有测试适配新接口后全部通过
./output/test_npi          # 不受影响
./output/test_framework    # 适配新 IChannel/IOperator 接口
./output/test_bridge       # 适配新 Work(IChannel*, IChannel*) 签名

# 3. Web 测试
./output/test_web
# 验证：SQLite CRUD + API 端点 + SQL 解析 + Pipeline 执行
```

### 端到端验收场景：Python 算子完整执行链路

```bash
# 启动（自动加载 bridge 插件、启动 Python Worker、预填测试数据）
./output/flowsql_web &
```

验收步骤：
1. `GET /api/channels` → 看到预填的 `test_data` 通道（含示例网络流量数据）
2. `GET /api/operators` → 看到 `explore.chisquare` 算子（Python Worker 自动注册）
3. `POST /api/tasks`，提交 SQL：
   ```sql
   SELECT * FROM test_data USING explore.chisquare WITH threshold=0.05
   ```
4. `GET /api/tasks/{id}/result` → 返回卡方分析结果 DataFrame（JSON 格式）
5. 浏览器访问 `http://localhost:8081`，在 SQL 工作台提交同样的 SQL，前端展示结果表格
6. `POST /api/tasks`，提交带 INTO 的 SQL（验证 DataFrameChannel Write→Read 串联）：
   ```sql
   SELECT * FROM test_data USING explore.chisquare WITH threshold=0.05 INTO chisquare_result
   ```
7. `POST /api/tasks`，提交引用上一步结果的 SQL：
   ```sql
   SELECT * FROM chisquare_result USING explore.summary
   ```
8. `GET /api/tasks/{id}/result` → 返回对卡方结果的二次汇总，验证多 SQL 串联链路

### 端到端验收场景 2：Python 算子上传激活

1. 准备一个新的 Python 算子文件（如 `explore_summary.py`，实现简单的统计汇总）
2. `POST /api/operators/upload` 上传 .py 文件 → 存入 operators 目录
3. `POST /api/operators/explore.summary/activate` → BridgePlugin 重启 Worker → 新算子动态注册
4. `GET /api/operators` → 看到新增的 `explore.summary` 算子
5. `POST /api/tasks`，提交 SQL：
   ```sql
   SELECT * FROM test_data USING explore.summary
   ```
6. `GET /api/tasks/{id}/result` → 返回汇总结果

### flowsql_web 启动序列

```
main()
  1. PluginRegistry::Instance()
  2. LoadPlugins() — 加载 libflowsql_bridge.so 等插件
     → pluginregist() 注册 BridgePlugin
     → IPlugin::Load() 读取配置
     → IModule::Start() 启动 Python Worker → 动态注册 Python 算子
  3. 创建预填 DataFrameChannel（test_data），注册到 PluginRegistry
  4. 初始化 SQLite 数据库（建表）
  5. 启动 httplib Server 监听 8081 端口
```

### 预填测试数据

flowsql_web 启动时硬编码创建 `test_data` DataFrameChannel，Schema 和内容：

| 列名 | 类型 | 说明 |
|------|------|------|
| src_ip | STRING | 源 IP |
| dst_ip | STRING | 目的 IP |
| src_port | UINT32 | 源端口 |
| dst_port | UINT32 | 目的端口 |
| protocol | STRING | 协议名 |
| bytes_sent | UINT64 | 发送字节数 |
| bytes_recv | UINT64 | 接收字节数 |

预填约 20-50 行模拟网络流量数据，足够 chisquare 算子产出有意义的结果。

---

## 关键文件清单

### 需修改的现有文件
- ✅ `src/framework/interfaces/ichannel.h` — IChannel 重构：移除 Put/Get(IDataEntity*)，新增 Type()/Schema()
- ✅ `src/framework/interfaces/ioperator.h` — Work 签名从 `Work(IDataFrame*, IDataFrame*)` 改为 `Work(IChannel*, IChannel*)`
- `src/framework/interfaces/idata_entity.h` — 角色调整为元数据声明（或废弃）（本阶段未改动，Schema() 已由各通道实现承担）
- ✅ `src/framework/core/pipeline.h/.cpp` — 重构：简化为纯连接器，支持 SQL 驱动的单算子执行
- ✅ `src/bridge/python_operator_bridge.h/.cpp` — 适配新 Work 签名，内部 dynamic_cast + Read/Write
- ✅ `src/plugins/example/` — 适配新 IChannel + IOperator 接口
- ✅ `src/tests/test_framework/main.cpp` — 适配接口变更 + 新增 DataFrameChannel 测试
- ✅ `src/CMakeLists.txt` — 新增 web 子目录

### 需新建的文件
- ✅ `src/framework/interfaces/idataframe_channel.h` — IDataFrameChannel 子接口
- ✅ `src/framework/interfaces/idatabase_channel.h` — IDatabaseChannel 子接口 + IBatchReader/IBatchWriter
- ✅ `src/framework/core/dataframe_channel.h/.cpp` — DataFrameChannel 内存实现
- ✅ `src/framework/core/sql_parser.h/.cpp` — SQL 解析器（递归下降）
- ✅ `src/web/` — Web 后端（web_server、db/）
- ✅ `src/web-ui/` — Vue.js 前端（已实现）
- ✅ `src/thirdparts/sqlite/sqlite-config.cmake` — SQLite 依赖
- `src/tests/test_web/` — Web API 测试（待实现）

### 已完成的文件（模块 P + 模块 A + 步骤 3-6）
| 文件 | 说明 |
|------|------|
| `common/loader.hpp` | PluginLoader 三阶段加载（已改造） |
| `framework/core/plugin_registry.h/.cpp` | 单例 + 动态注册/注销（已重构） |
| `bridge/` | C++ ↔ Python 桥接全部文件（已实现） |
| `python/` | Python Worker + 算子框架（已实现） |
| `thirdparts/httplib/` | cpp-httplib 依赖（已添加） |
| `framework/interfaces/ichannel.h` | IChannel 重构：移除 Put/Get，新增 Type()/Schema()，返回 const char* |
| `framework/interfaces/ioperator.h` | Work 签名改为 Work(IChannel*, IChannel*) |
| `framework/interfaces/idataframe_channel.h` | IDataFrameChannel 子接口（新建） |
| `framework/interfaces/idatabase_channel.h` | IDatabaseChannel + IBatchReader/IBatchWriter 接口（新建） |
| `framework/core/dataframe_channel.h/.cpp` | DataFrameChannel 内存实现（新建） |
| `framework/core/sql_parser.h/.cpp` | SQL 递归下降解析器（新建） |
| `framework/core/pipeline.h/.cpp` | Pipeline 简化为纯连接器 + PipelineBuilder |
| `plugins/example/memory_channel.h/.cpp` | 适配 IDataFrameChannel |
| `plugins/example/passthrough_operator.h/.cpp` | 适配新 Work 签名 |
| `plugins/example/plugin_register.cpp` | 新增 IDataFrameChannel 注册 |
| `bridge/python_operator_bridge.h/.cpp` | 适配新 Work 签名 + dynamic_cast |
| `thirdparts/sqlite/sqlite-config.cmake` | SQLite amalgamation 依赖（新建） |
| `web/db/database.h/.cpp` | SQLite 封装 + 参数化查询（新建） |
| `web/db/schema.sql` | 建表 DDL（新建） |
| `web/web_server.h/.cpp` | httplib Web 服务器 + 全部 API（新建） |
| `web/main.cpp` | flowsql_web 入口（新建） |
| `web/CMakeLists.txt` | Web 构建配置（新建） |
| `tests/test_framework/main.cpp` | 适配新接口 + 新增 DataFrameChannel 测试 |
| `CMakeLists.txt` | 新增 web 子目录 |
| `framework/macros.h` | 已删除（未使用的死代码） |
| `web-ui/src/main.js` | Vue 3 入口文件 + Element Plus 全局注册（新建） |
| `web-ui/src/App.vue` | 根组件 + 布局（新建） |
| `web-ui/src/router/index.js` | Vue Router 配置（新建） |
| `web-ui/src/api/index.js` | Axios API 封装（新建） |
| `web-ui/src/components/Sidebar.vue` | 侧边栏导航组件（新建） |
| `web-ui/src/views/Dashboard.vue` | 仪表盘页面（新建） |
| `web-ui/src/views/Channels.vue` | 通道列表页面（新建） |
| `web-ui/src/views/Operators.vue` | 算子管理页面（新建） |
| `web-ui/src/views/Tasks.vue` | SQL 工作台页面（新建） |
| `web-ui/vite.config.js` | Vite 配置 + 开发代理（新建） |
| `web-ui/package.json` | 前端依赖配置（新建） |
| `web-ui/index.html` | HTML 入口（新建） |
| `test_frontend.sh` | 前端功能测试脚本（新建） |
| `docs/frontend_verification.md` | 前端验证指南（新建） |
