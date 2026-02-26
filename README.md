# FlowSQL

基于SQL语法的网络流量分析共创平台

## 项目简介

FlowSQL 是一个全栈式网络流量分析平台，通过扩展的SQL语法提供从数据采集、流量分析到数据探索的完整能力。用户无需深入了解底层技术（如DPDK、Hyperscan），只需使用熟悉的SQL语句即可构建自己的流量分析系统。

## 核心特性

- **数据采集**：通过SQL语句从网卡或存储空间采集网络流量
- **NPM分析**：实时或事后的网络性能监控分析
- **数据查询**：标准SQL查询语法支持
- **统计分析**：描述性统计分析能力
- **探索性分析**：支持PCA、朴素贝叶斯等机器学习算法
- **模型助手**：提供数据预处理和神经网络模型训练

## 快速开始

### 环境要求

- CMake 3.12+
- C++17 编译器
- Linux 系统

### 编译

```bash
cd src
mkdir -p build
cd build
cmake ..
make
```

## SQL语法示例

### 数据采集
```sql
select *packet* from *netcard.nic1, netcard.nic2* into *ss.packets*
```

### NPM分析
```sql
npm.basic packet from *netcard.nic1, netcard.nic2* into *ts.npm*
```

### 数据查询
```sql
select * from *ts.npm.tcp_session* where ip = '8.8.8.8'
```

### 统计分析
```sql
statistic.hist bps from *ts.npm.tcp_session*
where time = '[2024/07/14 00:00:00 - 2024/07/14 23:59:59]'
group by application granularity 60
```

### 探索性分析
```sql
explore.pca bps,rss,connect_suc_rate from *ts.npm.tcp_session*
where time = '[2024/07/14 00:00:00 - 2024/07/14 23:59:59]'
group by application granularity 60
```

## 扩展关键字

| 关键字 | 描述 |
|--------|------|
| 通道 | netcard、kafka、ss、ts、smq |
| 算子 | npm、statistic、explore、model |
| granularity | 数据聚合粒度（秒） |
| into | 指定数据输出目标 |
| label | 模型训练标签字段 |
| with | 算子或模型附加参数 |

## 架构

FlowSQL 采用插件化架构，核心由四个接口驱动：

```
IPlugin（生命周期基元）
├── IChannel（数据通道）    ← Pipeline 的源和汇
├── IOperator（数据算子）   ← Pipeline 的计算节点
└── ...（可扩展）

IModule（启停控制）         ← 需要后台运行的组件额外实现
```

### IPlugin — 插件生命周期基元

定义：`common/loader.hpp` | 标识：`IID_PLUGIN`

所有可加载组件的根接口。IChannel 和 IOperator 都继承自它，因此每个通道和算子天然具备生命周期管理能力。

- `Load()` — 插件初始化（三阶段加载的阶段 2），此时不应调用其他接口
- `Unload()` — 插件清理
- `Option(const char*)` — 接收加载时传入的配置参数

### IModule — 服务启停控制

定义：`common/loader.hpp` | 标识：`IID_MODULE`

用于需要"运行态"的组件（如后台进程、网络监听）。与 IPlugin 正交，一个插件类可以同时实现两者。

- `Start()` — 启动服务（阶段 3，所有 IPlugin::Load 完成后）
- `Stop()` — 停止服务（Unload 前逆序调用，Start 失败时自动回滚）

### IChannel — 数据通道

定义：`framework/interfaces/ichannel.h` | 标识：`IID_CHANNEL` | 继承 IPlugin

代表数据的入口或出口。`Catelog()` + `Name()` 构成唯一标识（如 `"example.memory"`）。

- `Open()` / `Close()` — 通道生命周期
- `Put(IDataEntity*)` — 写入数据实体
- `Get()` → `IDataEntity*` — 读取数据实体
- `Flush()` — 批量刷新

### IOperator — 数据算子

定义：`framework/interfaces/ioperator.h` | 标识：`IID_OPERATOR` | 继承 IPlugin

数据处理单元，接收 DataFrame 输入、产出 DataFrame 输出。

- `Work(IDataFrame* in, IDataFrame* out)` — 核心计算方法
- `Position()` — 算子位置（STORAGE 或 DATA 层）
- `Configure(key, value)` — 运行时调参

### IDataFrame / IDataEntity — 数据抽象

- `IDataFrame`（`framework/interfaces/idataframe.h`）— 列式数据集，Apache Arrow RecordBatch 后端，支持行列操作、Arrow 零拷贝互操作、JSON 序列化
- `IDataEntity`（`framework/interfaces/idata_entity.h`）— 单行数据实体，Schema + FieldValue，支持 Clone 和 JSON 序列化

### 数据流

```
┌─────────┐  Get()  ┌───────────┐  Work()  ┌───────────┐  Put()  ┌─────────┐
│ IChannel│ ──────→ │ IDataFrame│ ──────→  │ IOperator │ ──────→ │ IChannel│
│ (source)│         │ (input)   │          │           │         │ (sink)  │
└─────────┘         └───────────┘          └───────────┘         └─────────┘
```

所有组件通过 GUID 标识，由 PluginLoader 加载 .so 插件，PluginRegistry 统一索引和查询。插件加载遵循三阶段流程：`pluginregist()` 注册 → `IPlugin::Load()` 初始化 → `IModule::Start()` 启动服务。

## 项目结构

```
flowSQL/
├── src/
│   ├── common/           # 公共库：插件加载器、GUID、工具函数、网络包结构
│   ├── framework/
│   │   ├── interfaces/   # 核心接口：IDataFrame、IDataEntity、IChannel、IOperator
│   │   └── core/         # 实现：DataFrame、Pipeline、PluginRegistry、Service
│   ├── plugins/
│   │   ├── protocol/npi/ # NPI 协议识别插件（Hyperscan 正则 + 位图 + 枚举匹配）
│   │   └── example/      # 示例插件（MemoryChannel + PassthroughOperator）
│   ├── tests/            # 测试：test_npi、test_framework
│   └── thirdparts/       # 第三方依赖（ExternalProject 管理）
├── docs/                 # 设计文档
└── CLAUDE.md             # AI 助手指引
```

## 文档

- [Stage 1 设计文档](docs/stage1.md) — C++ 框架核心
- [Stage 2 设计文档](docs/stage2.md) — C++ ↔ Python 桥接 + Web 管理系统

## 许可证

本项目采用 MIT 许可证 - 详见 [LICENSE](LICENSE) 文件

## 贡献

欢迎提交 Issue 和 Pull Request

## 联系方式

项目地址：https://github.com/lealiang/flowsql
