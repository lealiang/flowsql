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
- Apache Arrow 依赖引入（Stage 1 引入，Stage 2/3 用于 IPC 通信）

## 关键设计决策

1. **命名空间**：统一使用 `flowsql`
2. **统一算子接口**：C++ 和 Python 算子共享 `Work(IDataFrame* in, IDataFrame* out)` 接口
3. **IDataFrame**：C++ 侧表格数据结构，内部基于 Apache Arrow RecordBatch 存储，与 pandas.DataFrame 语义对齐，支持 Arrow IPC 零拷贝传输和 JSON 序列化互通
4. **IChannel**：融合 `Catelog/Name/Put/Get` + `Open/Close` 生命周期
5. **IDataEntity**：采用 `std::variant<...>` 方案（FieldValue），C++17 类型安全
6. **Pipeline 负责 Channel ↔ DataFrame 转换**：算子只关心 DataFrame，Pipeline 处理通道 I/O 和批量转换
7. **复用现有代码**：PluginRegistry 包装 PluginLoader，不修改 loader.hpp
8. **Arrow 存储**：DataFrame 内部使用 Arrow RecordBatch 列式存储，两阶段模式（构建期用 ArrayBuilder，读取前 Finalize 生成不可变 batch）
9. **C++ ↔ Python 通信**：Stage 2/3 采用 REST + Arrow IPC 二进制传输，Stage 1 引入 Arrow 依赖并保留 JSON 端点用于调试

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

表格数据结构接口，C++ 算子和 Python 算子的统一数据交换格式。内部基于 Apache Arrow RecordBatch 存储。

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

    // Arrow 互操作（零拷贝）
    virtual std::shared_ptr<arrow::RecordBatch> ToArrow() const = 0;
    virtual void FromArrow(std::shared_ptr<arrow::RecordBatch> batch) = 0;

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

IDataFrame 的默认实现，基于 Apache Arrow RecordBatch 的薄包装。

#### 内部结构

```cpp
class DataFrame : public IDataFrame {
    std::shared_ptr<arrow::Schema> arrow_schema_;      // Arrow schema
    std::shared_ptr<arrow::RecordBatch> batch_;         // 不可变列式数据
    std::vector<std::unique_ptr<arrow::ArrayBuilder>> builders_;  // 构建期列构建器
    std::vector<Field> schema_;                         // FlowSQL schema
    int32_t pending_rows_ = 0;                          // 构建期待提交行数

    void Finalize();  // builders_ → batch_，构建期结束，生成不可变 RecordBatch
};
```

#### 两阶段模式

- **构建期**：`AppendRow()` / `AppendEntity()` 向 `builders_` 追加数据，`pending_rows_` 递增
- **读取前**：`Finalize()` 将 `builders_` 中的数据生成不可变 `batch_`（Arrow RecordBatch）
- **读取期**：`GetRow()` / `GetColumn()` / `RowCount()` / `ToArrow()` 从 `batch_` 读取
- `ToArrow()` / `FromArrow()` 零拷贝，直接返回/设置内部 `batch_`

#### DataType ↔ Arrow 类型映射

| FlowSQL DataType | Arrow 类型 | 说明 |
|-------------------|-----------|------|
| INT32 | `arrow::int32()` | 32 位有符号整数 |
| INT64 | `arrow::int64()` | 64 位有符号整数 |
| UINT32 | `arrow::uint32()` | 32 位无符号整数 |
| UINT64 | `arrow::uint64()` | 64 位无符号整数 |
| FLOAT | `arrow::float32()` | 单精度浮点 |
| DOUBLE | `arrow::float64()` | 双精度浮点 |
| STRING | `arrow::utf8()` | UTF-8 字符串 |
| BYTES | `arrow::binary()` | 二进制数据 |
| TIMESTAMP | `arrow::int64()` | 时间戳（复用 int64） |
| BOOLEAN | `arrow::boolean()` | 布尔值 |

#### 关键方法

- `SetSchema()`：根据 FlowSQL schema 创建对应的 Arrow schema 和 ArrayBuilder
- `AppendRow()` / `AppendEntity()`：向 builders_ 追加数据
- `Finalize()`：调用各 builder 的 `Finish()` 生成 Arrow Array，组装为 RecordBatch
- `GetRow(i)`：从 batch_ 中按行索引提取各列值
- `GetColumn(name)`：从 batch_ 中按列名提取整列数据
- `ToArrow()`：直接返回 batch_（零拷贝）
- `FromArrow(batch)`：直接设置 batch_ 并从 Arrow schema 反推 FlowSQL schema
- `ToJson()` / `FromJson()`：保持与 pandas `to_json(orient='split')` 兼容的 JSON 格式
- `Clear()`：重置 builders_ 和 batch_，回到构建期初始状态

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
        //    ReserveRows(batch_size_) 预分配 builder 容量
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
        //    直接从 out_frame 的 Arrow batch_ 读取，避免额外拷贝
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

#### Pipeline Arrow 优化

- `ReserveRows(n)`：在批量读取前预分配 Arrow ArrayBuilder 容量，减少内存重分配
- 批量 `AppendEntity()` 后统一调用 `Finalize()` 生成 RecordBatch，避免频繁构建
- sink 写回直接从 `batch_` 读取列式数据，无需中间转换
- 算子间传递 Arrow RecordBatch 时可实现零拷贝（同进程内）

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

## IDataEntity ↔ IDataFrame ↔ Arrow 映射关系

```
IDataEntity（单行）          IDataFrame（表格）           Arrow RecordBatch        pandas.DataFrame
┌─────────────────┐      ┌──────────────────────┐    ┌──────────────────┐    ┌──────────────────┐
│ GetEntityType()  │      │ GetSchema() = columns│    │ schema()         │    │ df.columns       │
│ GetSchema()      │ ──→  │ RowCount() = len     │ ←→ │ num_rows()       │ ←→ │ len(df)          │
│ GetFieldValue()  │      │ GetRow(i) = row      │    │ column(i)        │    │ df.iloc[i]       │
│ SetFieldValue()  │      │ GetColumn(name)      │    │ GetColumnByName()│    │ df[name]         │
└─────────────────┘      │ AppendEntity()       │    └──────────────────┘    │ df.append()      │
   一行数据               │ ToArrow() ──────────→│←── FromArrow()            │ df.to_json()     │
                          │ ToJson() / FromJson()│                           └──────────────────┘
                          └──────────────────────┘       零拷贝传输               Python 侧
                             C++ 侧
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

## C++ ↔ Python 通信协议（Stage 2/3 预告）

Stage 1 引入 Arrow 依赖，Stage 2/3 实现完整的 C++ ↔ Python 通信。

### 协议设计

采用 REST + Arrow IPC 二进制传输方案：

```
C++ Service                          Python Worker
    │                                      │
    │  POST /work                          │
    │  Content-Type: application/vnd.      │
    │    apache.arrow.stream               │
    │  Body: Arrow IPC stream (in_frame)   │
    │ ────────────────────────────────────→ │
    │                                      │  pyarrow.ipc.open_stream()
    │                                      │  → pandas DataFrame
    │                                      │  → operator.work(df_in)
    │                                      │  → df_out
    │  200 OK                              │
    │  Content-Type: application/vnd.      │
    │    apache.arrow.stream               │
    │  Body: Arrow IPC stream (out_frame)  │
    │ ←──────────────────────────────────── │
    │                                      │
```

### 关键特性

- **高性能**：Arrow IPC 二进制传输，C++ 和 Python 之间零序列化开销
- **零拷贝**：`pyarrow.RecordBatch` 与 `pandas.DataFrame` 之间通过 `to_pandas()` / `from_pandas()` 转换
- **JSON 调试端点**：保留 `POST /work?format=json` 端点，使用 JSON 格式传输，便于调试和开发
- **向后兼容**：Stage 1 的 `ToJson()` / `FromJson()` 接口保留，Stage 2/3 新增 Arrow IPC 通道

### Python 侧示例

```python
import pyarrow as pa
import pyarrow.ipc as ipc
import pandas as pd

# 接收 Arrow IPC stream → pandas DataFrame
reader = ipc.open_stream(request.body)
table = reader.read_all()
df_in = table.to_pandas()

# 算子处理
df_out = operator.work(df_in)

# pandas DataFrame → Arrow IPC stream 返回
batch = pa.RecordBatch.from_pandas(df_out)
sink = pa.BufferOutputStream()
writer = ipc.new_stream(sink, batch.schema)
writer.write_batch(batch)
writer.close()
response.body = sink.getvalue()
```

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

### Arrow 第三方依赖（`src/thirdparts/arrow-config.cmake`）

```cmake
ExternalProject_Add(arrow_ep
    URL https://github.com/apache/arrow/archive/refs/tags/apache-arrow-18.1.0.tar.gz
    SOURCE_SUBDIR cpp
    CMAKE_ARGS
        -DARROW_BUILD_STATIC=ON
        -DARROW_BUILD_SHARED=OFF
        -DARROW_COMPUTE=OFF
        -DARROW_JSON=ON
        -DARROW_IPC=ON
        -DARROW_WITH_UTF8PROC=OFF
        -DARROW_WITH_RE2=OFF
        -DCMAKE_INSTALL_PREFIX=${THIRDPARTS_INSTALL_DIR}
        -DCMAKE_BUILD_TYPE=Release
)
```

通过 `add_thirddepen(TARGET arrow)` 宏自动处理 include/link 目录和库链接。

### CMakeLists.txt 集成

修改 `src/CMakeLists.txt`，新增：
```cmake
add_subdirectory(${CMAKE_SOURCE_DIR}/framework ${CMAKE_BINARY_DIR}/framework)
add_subdirectory(${CMAKE_SOURCE_DIR}/plugins/example ${CMAKE_BINARY_DIR}/example_plugin)
add_subdirectory(${CMAKE_SOURCE_DIR}/tests/test_framework ${CMAKE_BINARY_DIR}/test_framework)
```

构建产物：
- `libflowsql_framework.a` — 框架静态库（链接 Arrow 静态库）
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

1. ~~创建 `src/framework/` 目录结构~~ ✅
2. ~~`idata_entity.h` — 数据类型和实体接口~~ ✅
3. ~~`idataframe.h` — DataFrame 接口（含 ToArrow/FromArrow）~~ ✅
4. ~~`ichannel.h` — 通道接口~~ ✅
5. ~~`ioperator.h` — 算子接口~~ ✅
6. ~~`macros.h` — 注册宏~~ ✅
7. ~~`dataframe.h/.cpp` — DataFrame 默认实现（Arrow RecordBatch 薄包装 + JSON 序列化）~~ ✅
8. ~~`plugin_registry.h/.cpp` — 插件注册中心~~ ✅
9. ~~`pipeline.h/.cpp` — 流水线（含 Channel ↔ DataFrame 转换 + Arrow 优化）~~ ✅
10. ~~`service.h/.cpp` — 服务主框架~~ ✅
11. ~~示例插件（MemoryChannel + PassthroughOperator）~~ ✅
12. ~~框架测试（test_framework）~~ ✅ — 5 个用例全部通过
13. ~~`arrow-config.cmake` — Arrow 第三方依赖配置~~ ✅ — 支持 pyarrow / 缓存 / 源码编译三级回退
14. ~~CMakeLists.txt 集成 + 编译验证~~ ✅ — 第三方依赖隔离到 build 目录外
