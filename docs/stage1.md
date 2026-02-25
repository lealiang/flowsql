# Stage 1：C++ 框架核心 — 设计文档

## 概述

Stage 1 实现 FlowSQL 插件式进程框架的 C++ 核心部分，包括：
- 统一数据接口（IDataEntity、IDataFrame）
- 通道接口（IChannel）
- 统一算子接口（IOperator），C++ 和 Python 算子共享相同的 `Work(IDataFrame*, IDataFrame*)` 接口
- 插件注册中心（PluginRegistry，包装现有 PluginLoader）
- 流水线（Pipeline）：负责 Channel ↔ DataFrame 转换，算子只操作 DataFrame
- 服务进程主框架（Service）
- 示例插件（MemoryChannel、PassthroughOperator）及测试

## 关键设计决策

1. **命名空间**：统一使用 `flowsql`
2. **统一算子接口**：C++ 和 Python 算子共享 `Work(IDataFrame* in, IDataFrame* out)` 接口
3. **IDataFrame**：C++ 侧表格数据结构，与 pandas.DataFrame 语义对齐，通过 JSON 序列化互通
4. **IChannel**：融合 `Catelog/Name/Put/Get` + `Open/Close` 生命周期
5. **IDataEntity**：采用 `std::variant<...>` 方案（FieldValue），C++17 类型安全
6. **Pipeline 负责 Channel ↔ DataFrame 转换**：算子只关心 DataFrame，Pipeline 处理通道 I/O 和批量转换
7. **复用现有代码**：PluginRegistry 包装 PluginLoader，不修改 loader.hpp

## 数据流架构

```
Pipeline 数据流：
  Channel(source) → [批量读取] → IDataFrame → Operator.Work() → IDataFrame → [批量写入] → Channel(sink)
```

算子不直接操作通道，只操作 DataFrame。Pipeline 框架负责：
1. 从 source channel 批量 Get() IDataEntity，组装成 IDataFrame
2. 调用 Operator.Work(in_frame, out_frame)
3. 将 out_frame 中的行逐个 Put() 到 sink channel

---

## 目录结构

```
src/
├── framework/
│   ├── CMakeLists.txt
│   ├── macros.h                      # Channel/Operator 注册宏
│   ├── interfaces/
│   │   ├── idata_entity.h            # DataType + FieldValue + Field + IDataEntity
│   │   ├── idataframe.h              # IDataFrame 接口
│   │   ├── ichannel.h                # IChannel（继承 IPlugin）
│   │   └── ioperator.h              # IOperator + OperatorPosition（继承 IPlugin）
│   └── core/
│       ├── dataframe.h               # DataFrame 默认实现（头文件）
│       ├── dataframe.cpp             # DataFrame 默认实现
│       ├── plugin_registry.h         # 插件注册中心（头文件）
│       ├── plugin_registry.cpp       # 插件注册中心
│       ├── pipeline.h                # 流水线 + PipelineBuilder（头文件）
│       ├── pipeline.cpp              # 流水线
│       ├── service.h                 # 服务进程主框架（头文件）
│       └── service.cpp               # 服务进程主框架
├── plugins/
│   └── example/
│       ├── CMakeLists.txt
│       ├── memory_channel.h          # 内存通道示例
│       ├── memory_channel.cpp
│       ├── passthrough_operator.h    # 透传算子示例
│       └── passthrough_operator.cpp
└── tests/
    └── test_framework/
        ├── CMakeLists.txt
        └── main.cpp                  # 框架集成测试
```

---

## 接口定义

### 1. IDataEntity（`framework/interfaces/idata_entity.h`）

数据实体接口，表示一行数据。

```cpp
namespace flowsql {

// 数据类型枚举
enum class DataType : int32_t {
    INT32 = 0,
    INT64,
    UINT32,
    UINT64,
    FLOAT,
    DOUBLE,
    STRING,
    BYTES,
    TIMESTAMP,
    BOOLEAN
};

// 字段值：C++17 std::variant 类型安全联合
using FieldValue = std::variant<
    int32_t,                // INT32
    int64_t,                // INT64
    uint32_t,               // UINT32
    uint64_t,               // UINT64
    float,                  // FLOAT
    double,                 // DOUBLE
    std::string,            // STRING
    std::vector<uint8_t>,   // BYTES
    bool                    // BOOLEAN (TIMESTAMP 复用 INT64)
>;

// 字段描述
struct Field {
    std::string name;
    DataType type;
    int32_t size = 0;
    std::string description;
};

// IID_DATA_ENTITY GUID
const Guid IID_DATA_ENTITY = {0xa1b2c3d4, 0x1234, 0x5678, {0x9a, 0xbc, 0xde, 0xf0, 0x12, 0x34, 0x56, 0x78}};

interface IDataEntity {
    virtual ~IDataEntity() = default;

    virtual std::string GetEntityType() const = 0;
    virtual std::vector<Field> GetSchema() const = 0;
    virtual bool HasField(const std::string& name) const = 0;
    virtual FieldValue GetFieldValue(const std::string& name) const = 0;
    virtual void SetFieldValue(const std::string& name, const FieldValue& value) = 0;

    virtual std::string ToJson() const = 0;
    virtual bool FromJson(const std::string& json) = 0;
    virtual std::shared_ptr<IDataEntity> Clone() const = 0;
};

}  // namespace flowsql
```

### 2. IDataFrame（`framework/interfaces/idataframe.h`）

表格数据结构接口，C++ 算子和 Python 算子的统一数据交换格式。

```cpp
namespace flowsql {

interface IDataFrame {
    virtual ~IDataFrame() = default;

    // Schema
    virtual std::vector<Field> GetSchema() const = 0;
    virtual void SetSchema(const std::vector<Field>& schema) = 0;

    // 行操作
    virtual int32_t RowCount() const = 0;
    virtual int AppendRow(const std::vector<FieldValue>& row) = 0;
    virtual std::vector<FieldValue> GetRow(int32_t index) const = 0;

    // 列操作
    virtual std::vector<FieldValue> GetColumn(const std::string& name) const = 0;

    // IDataEntity 互操作
    virtual int AppendEntity(IDataEntity* entity) = 0;
    virtual std::shared_ptr<IDataEntity> GetEntity(int32_t index) const = 0;

    // 序列化（C++ ↔ Python 数据交换，兼容 pandas to_json(orient='split')）
    virtual std::string ToJson() const = 0;
    virtual bool FromJson(const std::string& json) = 0;

    // 清空
    virtual void Clear() = 0;
};

}  // namespace flowsql
```

### 3. IChannel（`framework/interfaces/ichannel.h`）

通道接口，继承 IPlugin，融合元数据 + 生命周期 + 数据操作。

```cpp
namespace flowsql {

const Guid IID_CHANNEL = {0xc1d2e3f4, 0xabcd, 0xef01, {0x23, 0x45, 0x67, 0x89, 0xab, 0xcd, 0xef, 0x01}};

interface IChannel : public IPlugin {
    // 元数据
    virtual std::string Catelog() = 0;
    virtual std::string Name() = 0;
    virtual std::string Description() = 0;

    // 生命周期
    virtual int Open() = 0;
    virtual int Close() = 0;
    virtual bool IsOpened() const = 0;

    // 数据操作
    virtual int Put(IDataEntity* entity) = 0;
    virtual IDataEntity* Get() = 0;

    // 批量刷新
    virtual int Flush() = 0;
};

}  // namespace flowsql
```

### 4. IOperator（`framework/interfaces/ioperator.h`）

统一算子接口，继承 IPlugin。C++ 算子直接实现，Python 算子通过 PythonOperatorBridge 桥接。

```cpp
namespace flowsql {

const Guid IID_OPERATOR = {0xd4e5f6a7, 0xbcde, 0xf012, {0x34, 0x56, 0x78, 0x9a, 0xbc, 0xde, 0xf0, 0x12}};

enum class OperatorPosition : int32_t {
    STORAGE = 0,  // 存储策略算子
    DATA = 1      // 数据分析算子
};

interface IOperator : public IPlugin {
    // 元数据
    virtual std::string Catelog() = 0;        // 算类：cut, npm, statistic, explore, model
    virtual std::string Name() = 0;           // 算子名：packet, basic, hist, pca, cnn
    virtual std::string Description() = 0;
    virtual OperatorPosition Position() = 0;

    // 统一处理接口（C++ 和 Python 算子共享）
    virtual int Work(IDataFrame* in, IDataFrame* out) = 0;

    // 配置
    virtual int Configure(const char* key, const char* value) = 0;
};

}  // namespace flowsql
```

### 5. 注册宏（`framework/macros.h`）

```cpp
// Channel 注册宏
#define BEGIN_CHANNEL_REGIST(classname)                                                    \
    BEGIN_PLUGIN_REGIST(classname)                                                         \
    ____INTERFACE(flowsql::IID_PLUGIN, flowsql::IPlugin)                                   \
    ____INTERFACE(flowsql::IID_CHANNEL, flowsql::IChannel)

#define END_CHANNEL_REGIST() END_PLUGIN_REGIST()

// Operator 注册宏
#define BEGIN_OPERATOR_REGIST(classname)                                                   \
    BEGIN_PLUGIN_REGIST(classname)                                                         \
    ____INTERFACE(flowsql::IID_PLUGIN, flowsql::IPlugin)                                   \
    ____INTERFACE(flowsql::IID_OPERATOR, flowsql::IOperator)

#define END_OPERATOR_REGIST() END_PLUGIN_REGIST()
```

---

## 核心框架实现

### 6. DataFrame（`framework/core/dataframe.h/.cpp`）

IDataFrame 的默认实现。

- 内部存储：`vector<Field> schema_` + `vector<vector<FieldValue>> rows_`
- `AppendEntity()`：从 IDataEntity 提取字段值追加为一行
- `GetEntity()`：将一行数据包装为 IDataEntity 返回
- `ToJson()`：输出与 pandas `to_json(orient='split')` 兼容的 JSON 格式
- `FromJson()`：解析上述 JSON 格式

#### JSON 数据交换格式

```json
{
  "columns": ["src_ip", "dst_ip", "bytes_sent", "protocol"],
  "types": ["STRING", "STRING", "UINT64", "STRING"],
  "data": [
    ["192.168.1.1", "8.8.8.8", 1024, "HTTP"],
    ["10.0.0.1", "172.16.0.1", 2048, "DNS"]
  ]
}
```

### 7. PluginRegistry（`framework/core/plugin_registry.h/.cpp`）

插件注册中心，包装 PluginLoader::Single()。

- `LoadPlugin(path)`：加载单个 .so 插件
- `UnloadAll()`：卸载所有插件
- 通道查询：`GetChannel(catelog, name)` — 按 "catelog.name" 索引
- 算子查询：`GetOperator(catelog, name)` — 按 "catelog.name" 索引
- 遍历：`TraverseChannels(callback)`、`TraverseOperators(callback)`

### 8. Pipeline（`framework/core/pipeline.h/.cpp`）

流水线，负责 Channel ↔ DataFrame 转换。

```cpp
// Pipeline 内部执行逻辑
void Pipeline::Run() {
    DataFrame in_frame, out_frame;

    while (running_) {
        in_frame.Clear();
        out_frame.Clear();

        // 1. 从 source channels 批量读取，组装 DataFrame
        for (auto* ch : sources_) {
            for (int i = 0; i < batch_size_; ++i) {
                IDataEntity* entity = ch->Get();
                if (!entity) break;
                in_frame.AppendEntity(entity);
            }
        }

        if (in_frame.RowCount() == 0) break;  // 无数据

        // 2. 调用算子（C++ 或 Python，接口完全相同）
        if (operator_->Work(&in_frame, &out_frame) != 0) {
            state_ = PipelineState::FAILED;
            break;
        }

        // 3. 将结果 DataFrame 拆解为 Entity，写回 sink channel
        if (sink_) {
            for (int32_t i = 0; i < out_frame.RowCount(); ++i) {
                auto entity = out_frame.GetEntity(i);
                sink_->Put(entity.get());
            }
        }
    }
}
```

- `PipelineState`：IDLE / RUNNING / STOPPED / FAILED
- `PipelineBuilder`：链式构建 `AddSource() → SetOperator() → SetSink() → SetBatchSize() → Build()`
- 批量大小可配置（默认 1000 行/批）

### 9. Service（`framework/core/service.h/.cpp`）

服务进程主框架。

- `Init(argc, argv)`：初始化（gflags 参数解析）
- `LoadPlugins(config_path)`：从配置加载插件
- `Run()`：主循环
- `Shutdown()`：优雅关闭

---

## 示例插件

### 10. MemoryChannel（`plugins/example/memory_channel.h/.cpp`）

内存队列通道，用于测试。

- 内部使用 `std::queue<std::shared_ptr<IDataEntity>>` 存储
- `Put()`：克隆实体入队
- `Get()`：出队返回
- `Open()/Close()`：设置状态标志

### 11. PassthroughOperator（`plugins/example/passthrough_operator.h/.cpp`）

透传算子，将输入 DataFrame 直接复制到输出。

```cpp
int Work(IDataFrame* in, IDataFrame* out) override {
    out->SetSchema(in->GetSchema());
    for (int32_t i = 0; i < in->RowCount(); ++i) {
        out->AppendRow(in->GetRow(i));
    }
    return 0;
}
```

---

## IDataEntity ↔ IDataFrame 映射关系

```
IDataEntity（单行）          IDataFrame（表格）           pandas.DataFrame
┌─────────────────┐      ┌──────────────────────┐    ┌──────────────────┐
│ GetEntityType()  │      │ GetSchema() = columns│    │ df.columns       │
│ GetSchema()      │ ──→  │ RowCount() = len     │ ←→ │ len(df)          │
│ GetFieldValue()  │      │ GetRow(i) = row      │    │ df.iloc[i]       │
│ SetFieldValue()  │      │ GetColumn(name)      │    │ df[name]         │
└─────────────────┘      │ AppendEntity()       │    │ df.append()      │
   一行数据               │ ToJson() / FromJson()│    │ df.to_json()     │
                          └──────────────────────┘    └──────────────────┘
                             C++ 侧                      Python 侧
```

---

## 算子分类体系

```
算子 (IOperator)
├── 存储策略算子 (Position = STORAGE)
│   ├── cut.packet        — 数据包裁剪（C++）
│   ├── cut.payload       — 负载裁剪（C++）
│   ├── exclude.encrypted — 排除加密流量（C++）
│   ├── exclude.protocol  — 排除指定协议（C++）
│   ├── include.protocol  — 包含指定协议（C++）
│   └── include.address   — 包含指定地址（C++）
│
└── 数据分析算子 (Position = DATA)
    ├── npm.basic          — NPM 基本分析（C++ 高性能）
    ├── statistic.basic    — 基本统计量（C++ 或 Python）
    ├── statistic.hist     — 数据分布（C++ 或 Python）
    ├── statistic.correlation — 相关系数（Python）
    ├── explore.pca        — 主成分分析（Python）
    ├── explore.anova      — 方差分析（Python）
    ├── explore.chisquare  — 卡方分析（Python）
    ├── explore.regression — 回归分析（Python）
    ├── explore.logit      — 逻辑回归（Python）
    ├── explore.decisiontree — 决策树（Python）
    ├── model.cnn          — 卷积神经网络（Python）
    ├── model.lstm         — 长短期记忆网络（Python）
    ├── model.autoencoder  — 自编码器（Python）
    ├── model.isolation_forest — 孤立森林（Python）
    ├── model.kmeans       — K均值聚类（Python）
    ├── model.random_forest — 随机森林（Python）
    └── model.xgboost      — XGBoost（Python）
```

高性能场景（NPM、存储策略）用 C++ 算子，数据探索和模型训练用 Python 算子。
两者通过相同的 `Work(IDataFrame*, IDataFrame*)` 接口统一调度。

---

## 复用的现有文件

| 文件 | 复用内容 |
|------|---------|
| `src/common/loader.hpp` | IPlugin, IRegister, PluginLoader, BEGIN_PLUGIN_REGIST 宏模式 |
| `src/common/guid.h` | Guid 结构体 |
| `src/common/typedef.h` | interface 宏, EXPORT_API, thandle |
| `src/common/iquerier.hpp` | IQuerier, fntraverse |
| `src/common/toolkit.hpp` | get_absolute_process_path() |

---

## 构建集成

修改 `src/CMakeLists.txt`，新增：
```cmake
add_subdirectory(${CMAKE_SOURCE_DIR}/framework ${CMAKE_BINARY_DIR}/framework)
add_subdirectory(${CMAKE_SOURCE_DIR}/plugins/example ${CMAKE_BINARY_DIR}/example_plugin)
add_subdirectory(${CMAKE_SOURCE_DIR}/tests/test_framework ${CMAKE_BINARY_DIR}/test_framework)
```

构建产物：
- `libflowsql_framework.a` — 框架静态库
- `libflowsql_example.so` — 示例插件动态库
- `test_framework` — 框架测试可执行文件

---

## 验证方案

```bash
cd src/build && rm -rf * && cmake .. && make -j$(nproc)
# 1. 编译成功：libflowsql_framework.a + 示例插件 .so + test_framework
# 2. 现有 NPI 插件和 test_npi 仍正常编译
# 3. 运行 ./output/test_framework 验证：
#    - 插件加载和查询
#    - DataFrame 创建、行列操作、JSON 序列化/反序列化
#    - Pipeline 数据流通：MemoryChannel → PassthroughOperator → MemoryChannel
```

---

## 实施顺序

1. 创建 `src/framework/` 目录结构
2. `idata_entity.h` — 数据类型和实体接口
3. `idataframe.h` — DataFrame 接口
4. `ichannel.h` — 通道接口
5. `ioperator.h` — 算子接口
6. `macros.h` — 注册宏
7. `dataframe.h/.cpp` — DataFrame 默认实现（含 JSON 序列化）
8. `plugin_registry.h/.cpp` — 插件注册中心
9. `pipeline.h/.cpp` — 流水线（含 Channel ↔ DataFrame 转换）
10. `service.h/.cpp` — 服务主框架
11. 示例插件（MemoryChannel + PassthroughOperator）
12. 框架测试（test_framework）
13. CMakeLists.txt 集成 + 编译验证
