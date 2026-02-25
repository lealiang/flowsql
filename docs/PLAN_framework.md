# flowSQL 插件式进程框架设计

## 背景与目标

基于现有 `PluginLoader` 和 `MDriver` 架构，设计一个统一的**插件式进程框架**，用于支持：
- **Channel (通道)** - 数据输入输出抽象
- **Operator (算子)** - 数据处理抽象
- **DataEntity (数据实体)** - 数据表示抽象
- **Web管理界面** - 插件上传、激活、管理的Web前后端系统

---

## 一、现有架构分析

### 现有组件

| 组件 | 文件 | 功能 |
|------|------|------|
| `PluginLoader` | `loader.hpp` | 轻量级插件加载，IPlugin接口 |
| `MDriver` | `modularity.h` | 模块加载器，IModule接口 |
| `Launcher` | `launcher.hpp` | 进程启动器 |

### 现有接口

```cpp
// 插件接口
interface IPlugin {
    int Load();
    int Unload();
    int Option(const char*);
}

// 模块接口 (继承IPlugin)
class IModule : public IPlugin {
    int Start();
    int Stop();
}
```

---

## 二、框架设计

### 2.1 整体架构

```
┌─────────────────────────────────────────────────────────────────┐
│                     flowSQL 进程框架                             │
├─────────────────────────────────────────────────────────────────┤
│  ┌─────────────┐  ┌─────────────┐  ┌─────────────┐             │
│  │   Channel   │  │   Operator  │  │  DataEntity │             │
│  │  插件层     │  │   插件层    │  │   插件层    │             │
│  └──────┬──────┘  └──────┬──────┘  └──────┬──────┘             │
│         │                │                │                     │
│  ┌──────▼──────┐  ┌──────▼──────┐  ┌──────▼──────┐             │
│  │ IChannel    │  │ IOperator   │  │ IDataEntity │             │
│  │ 接口抽象    │  │  接口抽象   │  │   接口抽象   │             │
│  └─────────────┘  └─────────────┘  └─────────────┘             │
│                                                                 │
│  ┌─────────────────────────────────────────────────────────────┐│
│  │              PluginRegistry (插件注册中心)                  ││
│  │  - 插件加载/卸载                                             ││
│  │  - 接口查询                                                  ││
│  │  - 生命周期管理                                              ││
│  └─────────────────────────────────────────────────────────────┘│
│                                                                 │
│  ┌─────────────────────────────────────────────────────────────┐│
│  │              PipelineBuilder (流水线构建器)                 ││
│  │  - 连接 Channel → Operator → Channel                       ││
│  │  - 数据流编排                                                ││
│  └─────────────────────────────────────────────────────────────┘│
└─────────────────────────────────────────────────────────────────┘
```

### 2.2 新增接口定义

#### IChannel 接口 (数据通道)

```cpp
// {A1B2C3D4-E5F6-7890-ABCD-EF1234567890}
const Guid IID_CHANNEL = {...};

interface IChannel : public IPlugin {
    // 通道类型
    virtual ChannelType Type() const = 0;

    // 打开/关闭通道
    virtual int Open() = 0;
    virtual int Close() = 0;

    // 读取/写入数据
    virtual int Read(IDataEntity* entity) = 0;
    virtual int Write(IDataEntity* entity) = 0;

    // 通道状态
    virtual bool IsOpened() const = 0;
    virtual int Flush() = 0;
};
```

#### IOperator 接口 (算子)

```cpp
// {B2C3D4E5-F6A7-8901-BCDE-F12345678901}
const Guid IID_OPERATOR = {...};

interface IOperator : public IPlugin {
    // 算子名称
    virtual const char* Name() const = 0;

    // 处理数据
    virtual int Process(IDataEntity* in, IDataEntity* out) = 0;

    // 批处理
    virtual int ProcessBatch(std::vector<IDataEntity*>& in,
                           std::vector<IDataEntity*>& out) = 0;

    // 配置
    virtual int Configure(const char* key, const char* value) = 0;
};
```

#### IDataEntity 接口 (数据实体)

```cpp
// {C3D4E5F6-A7B8-9012-CDEF-123456789012}
const Guid IID_DATA_ENTITY = {...};

interface IDataEntity {
    // 实体类型
    virtual EntityType Type() const = 0;

    // 字段访问
    virtual bool HasField(const char* name) const = 0;
    virtual Variant GetField(const char* name) const = 0;
    virtual int SetField(const char* name, const Variant& value) = 0;

    // 序列化
    virtual int Serialize(std::string& output) const = 0;
    virtual int Deserialize(const std::string& input) = 0;

    // 克隆
    virtual IDataEntity* Clone() const = 0;
};
```

---

## 三、目录结构设计

```
src/
├── common/                        # 公共基础库 (现有)
│   ├── algo/                     # 算法组件
│   ├── network/                  # 网络组件
│   ├── ipc/                      # 进程间通信
│   ├── share_memory/             # 共享内存
│   ├── threadsafe/               # 线程安全
│   ├── modularity/               # 模块化接口 (现有: loader.hpp, modularity.h)
│   ├── reflection/               # 反射
│   └── *.hpp                    # 工具类
│
├── plugins/                      # 插件实现 (现有)
│   └── protocol/npi/            # NPI协议插件
│
├── framework/                    # 新增：进程框架
│   ├── CMakeLists.txt
│   ├── plugin_registry.hpp       # 插件注册中心
│   ├── pipeline_builder.hpp     # 流水线构建器
│   ├── channel/
│   │   ├── ichannel.hpp         # 通道接口
│   │   └── channel_factory.hpp  # 通道工厂
│   ├── operator/
│   │   ├── ioperator.hpp        # 算子接口
│   │   └── operator_factory.hpp  # 算子工厂
│   └── data_entity/
│       ├── ientity.hpp          # 数据实体接口
│       ├── entity_factory.hpp    # 实体工厂
│       └── variant.hpp          # 变体类型
│
├── web/                          # Web管理平台 (新增)
│   ├── CMakeLists.txt
│   ├── server.cpp                # httplib服务器入口
│   ├── auth/                     # 认证模块
│   ├── api/                      # REST API
│   ├── service/                  # 业务服务
│   ├── db/                       # 数据库
│   └── ui/                       # 前端静态文件
│
└── tests/                        # 测试
```

---

## 四、核心组件实现

### 4.1 PluginRegistry (插件注册中心)

```cpp
// plugin_registry.hpp
class PluginRegistry {
public:
    // 单例
    static PluginRegistry* Instance();

    // 插件管理
    int LoadPlugin(const char* path);
    int UnloadPlugin(const char* name);

    // 接口查询
    IChannel* GetChannel(const char* name);
    IOperator* GetOperator(const char* name);
    IDataEntity* CreateEntity(const char* type);

    // 流水线管理
    int BuildPipeline(const char* config);
    int StartPipeline();
    int StopPipeline();

private:
    std::map<std::string, IChannel*> channels_;
    std::map<std::string, IOperator*> operators_;
    std::map<std::string, std::function<IDataEntity*()>> entity_factories_;
};
```

### 4.2 PipelineBuilder (流水线构建器)

```cpp
// pipeline_builder.hpp
class PipelineBuilder {
public:
    PipelineBuilder& Source(IChannel* channel);
    PipelineBuilder& Pipe(IOperator* op);
    PipelineBuilder& Sink(IChannel* channel);

    int Build(Pipeline& pipeline);
    int Run();
    int Stop();

private:
    std::vector<PipelineStage> stages_;
};
```

---

## 五、配置文件格式

### 5.1 插件配置 (plugins.json)

```json
{
    "plugins": [
        {
            "name": "netcard",
            "path": "./libflowsql_channel_netcard.so",
            "type": "channel",
            "config": {
                "interface": "eth0",
                "buffer_size": 65536
            }
        },
        {
            "name": "npi_recognizer",
            "path": "./libflowsql_operator_npi.so",
            "type": "operator",
            "config": {
                "protocols": ["http", "dns", "tls"]
            }
        }
    ]
}
```

### 5.2 流水线配置 (pipeline.json)

```json
{
    "pipeline": {
        "name": "capture_and_analyze",
        "stages": [
            {
                "type": "channel",
                "name": "netcard",
                "config": {"interface": "eth0"}
            },
            {
                "type": "operator",
                "name": "npi_recognizer"
            },
            {
                "type": "operator",
                "name": "packet_parser"
            },
            {
                "type": "channel",
                "name": "kafka",
                "config": {"brokers": "localhost:9092"}
            }
        ]
    }
}
```

---

## 六、示例：使用框架

### 6.1 定义通道插件

```cpp
// netcard_channel.hpp
#include <flowsql/framework/channel/ichannel.hpp>

class NetCardChannel : public fast::IChannel {
public:
    int Load() override;
    int Unload() override;
    int Open() override;
    int Close() override;
    int Read(fast::IDataEntity* entity) override;
    int Write(fast::IDataEntity* entity) override;

    FAST_CHANNEL("netcard", ChannelType::SOURCE)
};

// netcard_channel.cpp
#include "netcard_channel.hpp"

BEGIN_CHANNEL_REGIST(NetCardChannel)
    ____INTERFACE(fast::IID_CHANNEL, fast::IChannel)
END_CHANNEL_REGIST
```

### 6.2 定义算子插件

```cpp
// npi_operator.hpp
#include <flowsql/framework/operator/ioperator.hpp>

class NPIOperator : public fast::IOperator {
public:
    int Load() override;
    int Unload() override;
    int Process(fast::IDataEntity* in, fast::IDataEntity* out) override;

    FAST_OPERATOR("npm.npi", "Protocol recognition")
};

// npi_operator.cpp
#include "npi_operator.hpp"

BEGIN_OPERATOR_REGIST(NPIOperator)
    ____INTERFACE(fast::IID_OPERATOR, fast::IOperator)
END_OPERATOR_REGIST
```

### 6.3 主程序

```cpp
// main.cpp
#include <flowsql/framework/plugin_registry.hpp>
#include <fast/launcher.hpp>

int main(int argc, char* argv[]) {
    fast::Launcher launcher;
    return launcher.Launch(argc, argv, []() {
        auto& registry = fast::PluginRegistry::Instance();

        // 加载配置
        registry.LoadConfig("config/plugins.json");
        registry.LoadConfig("config/pipeline.json");

        // 启动流水线
        registry.StartPipeline();

        // 等待退出
        std::cin.get();

        // 停止
        registry.StopPipeline();
        return 0;
    });
}
```

---

## 七、实施计划

### 第一步：接口定义 (1-2天)

| 任务 | 文件 |
|------|------|
| 定义 IChannel 接口 | `framework/channel/ichannel.hpp` |
| 定义 IOperator 接口 | `framework/operator/ioperator.hpp` |
| 定义 IDataEntity 接口 | `framework/data_entity/ientity.hpp` |
| 定义 Variant 类型 | `framework/data_entity/variant.hpp` |

### 第二步：核心框架 (2-3天)

| 任务 | 文件 |
|------|------|
| 实现 PluginRegistry | `framework/plugin_registry.hpp` |
| 实现 PipelineBuilder | `framework/pipeline_builder.hpp` |
| 实现 ChannelFactory | `framework/channel/channel_factory.hpp` |
| 实现 OperatorFactory | `framework/operator/operator_factory.hpp` |

### 第三步：示例插件 (2-3天)

| 任务 | 说明 |
|------|------|
| 实现示例 Source Channel | 文件/内存作为数据源 |
| 实现示例 Operator | 简单数据转换算子 |
| 实现示例 Sink Channel | 文件/内存作为数据汇 |

### 第四步：构建系统 (1-2天)

| 任务 | 说明 |
|------|------|
| 创建 framework CMakeLists.txt | 编译框架库 |
| 创建示例插件 CMakeLists.txt | 编译示例插件 |
| 更新主程序 CMakeLists.txt | 集成框架 |

### 第五步：Web管理系统 (3-4天)

| 任务 | 文件 | 说明 |
|------|------|------|
| 实现Web后端API | `web/api/plugin_api.hpp` | httplib REST API |
| 实现认证模块 | `web/auth/user_auth.hpp` | 用户/密码认证 |
| 实现插件管理服务 | `web/service/plugin_service.hpp` | 上传/激活/停用 |
| 实现Web前端 | `web/ui/index.html` | Vue.js管理界面 |
| 配置与集成 | `web/CMakeLists.txt` | 编译Web服务 |

---

## 八、Web管理系统设计

### 8.1 系统架构

```
┌─────────────────────────────────────────────────────────────────┐
│                    flowSQL Web 管理平台                          │
├─────────────────────────────────────────────────────────────────┤
│  ┌─────────────────────────────────────────────────────────────┐│
│  │                    Vue.js 前端 (Port:8080)                   ││
│  │  - 插件列表    - 上传插件    - 激活/停用                     ││
│  │  - 用户登录    - 流水线配置  - 状态监控                      ││
│  └─────────────────────────────────────────────────────────────┘│
│                              │                                   │
│                    HTTP/REST (JSON)                               │
│                              ▼                                   │
│  ┌─────────────────────────────────────────────────────────────┐│
│  │              httplib 后端 (Port:8081)                       ││
│  │  ┌─────────────┐ ┌─────────────┐ ┌─────────────┐           ││
│  │  │  认证模块   │ │ 插件管理API │ │ 流水线API  │           ││
│  │  └─────────────┘ └─────────────┘ └─────────────┘           ││
│  └─────────────────────────────────────────────────────────────┘│
│                              │                                   │
│                    进程间通信 / 文件系统                          │
│                              ▼                                   │
│  ┌─────────────────────────────────────────────────────────────┐│
│  │              PluginRegistry (主程序)                         ││
│  │  - 插件加载/卸载  - 流水线管理  - 运行时状态               ││
│  └─────────────────────────────────────────────────────────────┘│
└─────────────────────────────────────────────────────────────────┘
```

### 8.2 核心功能模块

根据 design.md 文档，Web管理系统需支持以下核心功能：

#### 8.2.1 通道管理 (Channel Management)

| 功能 | 说明 |
|------|------|
| 通道列表 | 显示所有已注册的通道 |
| 创建通道 | 支持 netcard、kafka、ss、ts、smq 五种类型 |
| 配置通道 | 通道profile配置（IP、端口、topic等） |
| 启用/停用 | 通道的激活状态控制 |
| 通道监控 | 通道实时状态（数据量、速率等） |

**通道类型与关键字**：
- netcard: 网卡通道 `from netcard.nic1`
- kafka: Kafka队列 `from kafka.q1`
- ss: 存储空间 `from ss.packets`
- ts: 数据库表空间 `from ts.npm.tcp_session`
- smq: 内存队列 `from smq.transactions`

#### 8.2.2 算子管理 (Operator Management)

| 功能 | 说明 |
|------|------|
| 算子列表 | 显示所有已注册的算子 |
| 注册算子 | SQL: `INSERT INTO operator(position,catelog,name,operatorfile,active)` |
| 算子分类 | storage(cut/exclude/include)、data(npm/statistic/explore/model) |
| 激活/停用 | 算子启用状态控制 |
| 算子配置 | 运行时参数配置 |

**算子分类**：
- 存储策略算子: cut、exclude、include
- 数据分析算子: npm、statistic、explore、model

#### 8.2.3 数据实体管理 (Data Entity Management)

| 功能 | 说明 |
|------|------|
| 实体列表 | 显示所有已注册的数据实体 |
| 创建实体 | 定义实体schema（字段、类型） |
| 实体查看 | 查看实体的字段定义 |

**SQL**: `INSERT INTO data_entity(name, schema, description) VALUES(...)`

#### 8.2.4 任务管理 (Task Management)

| 功能 | 说明 |
|------|------|
| 任务列表 | 显示所有任务 |
| 创建任务 | 基于SQL语句创建采集/分析任务 |
| 任务控制 | 启动、停止、暂停、恢复 |
| 任务状态 | running/paused/stopped/failed/completed |
| 任务历史 | 查看历史任务记录 |

**SQL控制**：
- `STOP TASK task_id`
- `PAUSE TASK task_id`
- `RESUME TASK task_id`

#### 8.2.5 模型管理 (Model Management)

| 功能 | 说明 |
|------|------|
| 模型列表 | 显示所有已注册的模型 |
| 模型训练 | `model.train <type> ... LABEL ... INTO model.xxx` |
| 模型推理 | `model.predict <model_name> ...` |
| 模型评估 | `model.evaluate <model_name> ...` |
| 模型版本 | 版本管理 |

**内置模型**：cnn、lstm、autoencoder、isolation_forest、kmeans、random_forest、xgboost

#### 8.2.6 SQL执行界面

| 功能 | 说明 |
|------|------|
| SQL编辑器 | 支持完整的类SQL语法 |
| 执行结果 | 表格/JSON展示 |
| 历史记录 | 查询历史保存 |

**SQL语法支持**：
- 数据采集: `SELECT *packet* FROM netcard.nic1 INTO ss.packets`
- NPM分析: `npm.basic packet FROM netcard.nic1 INTO ts.npm`
- 数据查询: `SELECT * FROM ts.npm.tcp_session WHERE ...`
- 统计分析: `statistic.hist bps FROM ... GROUP BY application`
- 探索分析: `explore.pca bps,rss FROM ...`
- 模型训练: `model.train cnn ... LABEL application INTO model.xxx`

#### 8.2.7 系统监控

| 功能 | 说明 |
|------|------|
| 运行时状态 | 系统资源使用情况 |
| 数据统计 | 采集/分析数据量统计 |

### 8.3 REST API 设计

#### 认证接口
| 方法 | 路径 | 说明 |
|------|------|------|
| POST | `/api/auth/login` | 用户登录 |
| POST | `/api/auth/logout` | 用户登出 |

#### 通道接口
| 方法 | 路径 | 说明 |
|------|------|------|
| GET | `/api/channels` | 获取通道列表 |
| POST | `/api/channels` | 创建通道 |
| GET | `/api/channels/{name}` | 获取通道详情 |
| PUT | `/api/channels/{name}` | 更新通道配置 |
| DELETE | `/api/channels/{name}` | 删除通道 |
| POST | `/api/channels/{name}/start` | 启动通道 |
| POST | `/api/channels/{name}/stop` | 停止通道 |

#### 算子接口
| 方法 | 路径 | 说明 |
|------|------|------|
| GET | `/api/operators` | 获取算子列表 |
| POST | `/api/operators` | 注册算子 |
| DELETE | `/api/operators/{name}` | 注销算子 |
| POST | `/api/operators/{name}/activate` | 激活算子 |
| POST | `/api/operators/{name}/deactivate` | 停用算子 |

#### 数据实体接口
| 方法 | 路径 | 说明 |
|------|------|------|
| GET | `/api/entities` | 获取实体列表 |
| POST | `/api/entities` | 创建实体 |
| GET | `/api/entities/{name}` | 获取实体详情 |

#### 任务接口
| 方法 | 路径 | 说明 |
|------|------|------|
| GET | `/api/tasks` | 获取任务列表 |
| POST | `/api/tasks` | 创建任务 |
| GET | `/api/tasks/{id}` | 获取任务详情 |
| POST | `/api/tasks/{id}/start` | 启动任务 |
| POST | `/api/tasks/{id}/stop` | 停止任务 |
| POST | `/api/tasks/{id}/pause` | 暂停任务 |
| POST | `/api/tasks/{id}/resume` | 恢复任务 |
| GET | `/api/tasks/history` | 任务历史 |

#### 模型接口
| 方法 | 路径 | 说明 |
|------|------|------|
| GET | `/api/models` | 获取模型列表 |
| POST | `/api/models` | 注册模型 |
| DELETE | `/api/models/{name}` | 删除模型 |
| POST | `/api/models/{name}/train` | 训练模型 |
| POST | `/api/models/{name}/predict` | 模型推理 |
| GET | `/api/models/{name}/evaluate` | 模型评估 |

#### SQL执行接口
| 方法 | 路径 | 说明 |
|------|------|------|
| POST | `/api/sql/execute` | 执行SQL语句 |

#### 系统接口
| 方法 | 路径 | 说明 |
|------|------|------|
| GET | `/api/health` | 健康检查 |
| GET | `/api/stats` | 系统统计 |

### 8.4 数据模型

```json
// 用户
{
    "username": "admin",
    "password_hash": "xxx",
    "role": "admin"
}

// 通道
{
    "name": "nic1",
    "type": "netcard",
    "catalog": "netcard",
    "profile": {
        "interface": "eth0",
        "buffer_size": 65536
    },
    "status": "active",
    "description": "网卡采集通道"
}

// 算子
{
    "name": "basic",
    "catalog": "npm",
    "position": "data",
    "operator_file": "/operators/libnpm_basic.so",
    "active": true,
    "description": "NPM基本分析算子"
}

// 数据实体
{
    "name": "packet",
    "schema": {
        "fields": [
            {"name": "timestamp", "type": "UINT64", "size": 8},
            {"name": "src_ip", "type": "STRING"},
            {"name": "dst_ip", "type": "STRING"},
            {"name": "protocol", "type": "STRING"}
        ]
    },
    "description": "网络数据包实体"
}

// 任务
{
    "id": "task_001",
    "name": "capture_nic1",
    "sql": "SELECT packet FROM netcard.nic1 INTO ss.packets",
    "status": "running",
    "create_time": "2024-07-14T00:00:00Z",
    "start_time": "2024-07-14T00:00:01Z"
}

// 模型
{
    "name": "traffic_classifier",
    "type": "cnn",
    "file_path": "/models/traffic_v1.h5",
    "version": 1,
    "accuracy": 0.95,
    "training_date": "2024-07-01T00:00:00Z",
    "description": "加密流量分类器"
}
```

### 8.5 前端页面设计

| 页面 | 功能 |
|------|------|
| 登录页 | 用户认证 |
| Dashboard | 系统概览、统计图表 |
| 通道管理 | 通道CRUD、状态监控 |
| 算子管理 | 算子CRUD、激活控制 |
| 数据实体 | 实体定义管理 |
| 任务管理 | 任务创建、控制、历史 |
| 模型管理 | 模型训练、推理、评估 |
| SQL工作台 | SQL查询执行 |
| 系统设置 | 配置管理 |

---

## 九、关键文件清单

### 新建文件 (框架)

```
src/framework/CMakeLists.txt
src/framework/plugin_registry.hpp
src/framework/plugin_registry.cpp
src/framework/pipeline_builder.hpp
src/framework/pipeline_builder.cpp
src/framework/channel/CMakeLists.txt
src/framework/channel/ichannel.hpp
src/framework/channel/channel_factory.hpp
src/framework/operator/CMakeLists.txt
src/framework/operator/ioperator.hpp
src/framework/operator/operator_factory.hpp
src/framework/data_entity/CMakeLists.txt
src/framework/data_entity/ientity.hpp
src/framework/data_entity/variant.hpp
src/framework/data_entity/entity_factory.hpp
```

### 新建文件 (示例插件)

```
src/plugins/example/CMakeLists.txt
src/plugins/example/simple_source.cpp
src/plugins/example/simple_operator.cpp
src/plugins/example/simple_sink.cpp
```

### 新建文件 (Web系统)

```
src/web/CMakeLists.txt
src/web/server.cpp
src/web/auth/user_auth.hpp
src/web/auth/session.hpp
src/web/api/plugin_api.hpp
src/web/api/pipeline_api.hpp
src/web/api/auth_api.hpp
src/web/service/plugin_service.hpp
src/web/service/pipeline_service.hpp
src/web/db/sqlite_helper.hpp
src/web/ui/index.html
src/web/ui/app.js
src/web/ui/styles.css
```

### 修改文件

```
src/CMakeLists.txt              # 添加 framework 和 web 子目录
src/plugins/protocol/npi/CMakeLists.txt  # 保持现有插件
```

---

## 十、验证方案

### 测试1：接口编译测试
```bash
mkdir build && cd build
cmake .. && make
# 验证：编译成功，无错误
```

### 测试2：插件加载测试
```cpp
// 加载示例插件
registry.LoadPlugin("./libsimple_source.so");
registry.LoadPlugin("./libsimple_operator.so");
registry.LoadPlugin("./libsimple_sink.so");
// 验证：插件加载成功
```

### 测试3：流水线测试
```cpp
// 构建并运行流水线
registry.BuildPipeline(config);
registry.StartPipeline();
// 验证：数据从source经过operator流向sink
```

### 测试4：集成测试
```bash
./flowsql --config config.json
# 验证：完整流程运行正常
```

### 测试5：Web服务测试
```bash
# 启动Web服务
./flowsql_web &
# 验证：服务启动成功，端口8081可访问
curl http://localhost:8081/api/health
# 验证：返回 {"status": "ok"}
```

### 测试6：插件上传/激活测试
```bash
# 登录获取Token
curl -X POST http://localhost:8081/api/auth/login \
  -H "Content-Type: application/json" \
  -d '{"username":"admin","password":"admin"}'
# 验证：返回Token

# 上传插件
curl -X POST http://localhost:8081/api/plugins/upload \
  -H "Authorization: Bearer <token>" \
  -F "file=@./libsimple_source.so"
# 验证：上传成功

# 激活插件
curl -X POST http://localhost:8081/api/plugins/simple_source/activate \
  -H "Authorization: Bearer <token>"
# 验证：激活成功
```

### 测试7：Web前端测试
```bash
# 访问前端
open http://localhost:8080
# 验证：登录页面显示，可正常登录
# 验证：插件列表显示，上传功能可用
```

---

## 十一、详细实现步骤

### Phase 1: 接口定义 (src/framework/)

#### 1.1 创建目录结构
```
src/framework/
├── CMakeLists.txt
├── plugin_registry.hpp
├── plugin_registry.cpp
├── pipeline_builder.hpp
├── pipeline_builder.cpp
├── channel/
│   ├── CMakeLists.txt
│   ├── ichannel.hpp
│   └── channel_factory.hpp
├── operator/
│   ├── CMakeLists.txt
│   ├── ioperator.hpp
│   └── operator_factory.hpp
└── data_entity/
    ├── CMakeLists.txt
    ├── ientity.hpp
    ├── variant.hpp
    └── entity_factory.hpp
```

#### 1.2 核心接口实现细节

**ichannel.hpp - 使用现有命名空间 `fast`**
```cpp
// 基于现有 loader.hpp 的接口模式
namespace fast {

// 通道类型枚举
enum class ChannelType {
    SOURCE = 0,  // 数据源
    SINK   = 1,  // 数据汇
    BOTH   = 2   // 源和汇
};

// {A1B2C3D4-E5F6-7890-ABCD-EF1234567890}
const Guid IID_CHANNEL = {0xa1b2c3d4, 0xe5f6, 0x7890, {0xab, 0xcd, 0xef, 0x12, 0x34, 0x56, 0x78, 0x90}};

interface IChannel : public IPlugin {
    virtual ChannelType Type() const = 0;
    virtual int Open() = 0;
    virtual int Close() = 0;
    virtual int Read(IDataEntity* entity) = 0;
    virtual int Write(IDataEntity* entity) = 0;
    virtual bool IsOpened() const = 0;
    virtual int Flush() = 0;
};

} // namespace fast
```

**ioperator.hpp**
```cpp
namespace fast {

// {B2C3D4E5-F6A7-8901-BCDE-F12345678901}
const Guid IID_OPERATOR = {0xb2c3d4e5, 0xf6a7, 0x8901, {0xbc, 0xde, 0xf1, 0x23, 0x45, 0x67, 0x89, 0x01}};

interface IOperator : public IPlugin {
    virtual const char* Name() const = 0;
    virtual int Process(IDataEntity* in, IDataEntity* out) = 0;
    virtual int ProcessBatch(std::vector<IDataEntity*>& in, std::vector<IDataEntity*>& out) = 0;
    virtual int Configure(const char* key, const char* value) = 0;
};

} // namespace fast
```

**ientity.hpp**
```cpp
namespace fast {

enum class EntityType {
    GENERIC = 0,
    PACKET = 1,
    RECORD = 2
};

// {C3D4E5F6-A7B8-9012-CDEF-123456789012}
const Guid IID_DATA_ENTITY = {0xc3d4e5f6, 0xa7b8, 0x9012, {0xcd, 0xef, 0x12, 0x34, 0x56, 0x78, 0x90, 0x12}};

interface IDataEntity {
    virtual EntityType Type() const = 0;
    virtual bool HasField(const char* name) const = 0;
    virtual Variant GetField(const char* name) const = 0;
    virtual int SetField(const char* name, const Variant& value) = 0;
    virtual int Serialize(std::string& output) const = 0;
    virtual int Deserialize(const std::string& input) = 0;
    virtual IDataEntity* Clone() const = 0;
};

} // namespace fast
```

### Phase 2: 核心框架实现

#### 2.1 PluginRegistry
- 单例模式
- 管理 IChannel、IOperator 实例
- 使用现有 PluginLoader 加载插件
- 提供 GetChannel、GetOperator 接口查询方法

#### 2.2 PipelineBuilder
- 构建数据处理流水线
- Source -> [Operator]* -> Sink 模式
- 支持启动、停止流水线

### Phase 3: 示例插件
- MemorySource: 内存数据源通道
- TransformOperator: 数据转换算子
- MemorySink: 内存数据汇通道

### Phase 4: Web管理系统
- 使用 httplib 作为 HTTP 服务器库
- SQLite 存储用户和插件元数据
- Vue.js 前端界面

---

## 十二、关键现有代码复用

| 现有文件 | 复用内容 |
|----------|----------|
| `src/common/loader.hpp` | IPlugin 接口, PluginLoader, BEGIN_PLUGIN_REGIST 宏 |
| `src/common/modularity/modularity.h` | IModule 接口, MDriver |
| `src/common/guid.h` | Guid 结构体 |
| `src/common/typedef.h` | EXPORT_API, thandle, 平台抽象 |
| `src/common/iquerier.hpp` | IQuerier 接口 |

---

## 十三、执行顺序

1. 创建 framework 目录结构
2. 实现 Variant 类型 (ientity.hpp)
3. 实现 IDataEntity 接口
4. 实现 IChannel 接口
5. 实现 IOperator 接口
6. 实现 PluginRegistry
7. 实现 PipelineBuilder
8. 创建 CMakeLists.txt
9. 实现示例插件
10. 实现 Web 后端
11. 实现 Web 前端
12. 测试验证
