# Stage 3：数据库闭环 + 流式架构 + 平台增强

## Context

Stage 1 完成了 C++ 框架核心，Stage 2 完成了 C++ ↔ Python 桥接 + Web 管理系统 + Gateway 多服务架构。当前平台已具备：
- Gateway 星型服务拓扑（路由注册/发现/转发/心跳）
- Scheduler 统一执行引擎（SQL 解析 → Pipeline → 算子调度）
- Python 算子完整链路（共享内存 + Arrow IPC 零拷贝）
- Web 管理界面（通道/算子/任务管理 + SQL 执行）
- 单算子 Pipeline（SELECT * FROM ... USING ... WITH ... INTO ...）

**Stage 2→3 过渡期架构清理（已完成）**：
- 消除 PluginRegistry + libflowsql_framework.so，回归纯 PluginLoader 架构
- IChannel/IOperator 解耦 IPlugin（纯接口不继承生命周期接口）
- IBridge 接口（Python 算子查询/遍历/刷新，替代 `dynamic_cast<IRegister*>` hack）
- 算子刷新链路（Web → Worker reload → Scheduler refresh → IBridge::Refresh）
- arrow_codec.py 统一转换层（兼容 Polars/Pandas/Arrow Table）

Stage 3 目标：补齐数据库读写闭环，扩展 SQL 能力和 Pipeline 编排，引入流式处理架构，提升平台可用性。

## 已完成：架构重构

### 纯插件架构回归

删除 PluginRegistry 和 libflowsql_framework.so，回归 PluginLoader 单层架构。

**设计决策**：
- PluginRegistry 双层索引 + 共享库导出符号导致多实例问题（静态库链接到多个 .so 时单例失效），根因无法优雅解决
- IPlugin::Load 签名从 `Load(IRegister*)` 改为 `Load(IQuerier*)`，插件只需查询能力，不需要注册能力
- Pipeline / ChannelAdapter 从 framework 移入 scheduler.so（唯一使用者）
- flowsql_framework 库消除，公共头文件归入 flowsql_common（header-only）

**关键文件**：
- `common/iplugin.h` — 从 framework 拆出，含 IRegister/IPlugin/注册宏
- `common/loader.hpp` — PluginLoader 实现 IRegister + IQuerier
- `common/iquerier.hpp` — 插件查询接口

### 接口解耦（IChannel/IOperator 去掉 IPlugin 继承）

**设计决策**：
- IChannel/IOperator 是纯数据接口，不应继承 IPlugin 生命周期接口
- 具体实现类（MemoryChannel、PassthroughOperator）显式多继承 `IPlugin` + 数据接口
- 内部使用的类（DataFrameChannel、PythonOperatorBridge）不需要 IPlugin，只实现数据接口

**关键文件**：
- `framework/interfaces/ichannel.h` — 纯接口，不继承 IPlugin
- `framework/interfaces/ioperator.h` — 纯接口，不继承 IPlugin

### IBridge 接口

**设计决策**：
- 替代 `dynamic_cast<IRegister*>` 向 PluginLoader 动态注册 Python 算子的 hack
- BridgePlugin 多继承 `IPlugin, IBridge`，通过注册宏暴露 IID_BRIDGE
- Scheduler 通过 `IQuerier::First(IID_BRIDGE)` 获取 IBridge，调用 FindOperator/TraverseOperators
- 刷新链路：Web 激活算子 → Worker reload → Scheduler `/refresh-operators` → `IBridge::Refresh()`

**接口方法**：
- `FindOperator(catelog, name)` → `shared_ptr<IOperator>`（生命周期安全）
- `TraverseOperators(fn)` — 遍历所有已发现的 Python 算子
- `Refresh()` — 重新从 Python Worker 发现算子

**关键文件**：
- `framework/interfaces/ibridge.h` — IBridge 纯接口
- `services/bridge/bridge_plugin.h` — `BridgePlugin : IPlugin, IBridge`

## 核心接口体系

```
IPlugin（生命周期：Load/Unload/Start/Stop）
  ├── BridgePlugin : IPlugin, IBridge
  ├── SchedulerPlugin : IPlugin
  ├── GatewayPlugin : IPlugin
  ├── WebPlugin : IPlugin
  ├── MemoryChannel : IDataFrameChannel, IPlugin    ← 显式多继承
  └── PassthroughOperator : IOperator, IPlugin      ← 显式多继承

IChannel（纯接口：身份 + 生命周期 + 元数据，不继承 IPlugin）
  ├── IDataFrameChannel（批处理 + DataFrame）
  ├── IDatabaseChannel（批处理 + 数据库）
  └── IStreamChannel（流式，待实现）

IOperator（纯接口：元数据 + Work + Configure，不继承 IPlugin）

IBridge（纯接口：FindOperator + TraverseOperators + Refresh）
```

## 任务总览

```
P1（数据库闭环）—— 从 demo 到可用的关键跨越
  ├── DatabaseChannel 实现（ClickHouse 对接）
  ├── ChannelAdapter 自动适配（已完成）
  └── SQL 解析器增强（USING 可选 + 列选择，已完成）

P2（SQL + Pipeline 增强）—— 易用性提升
  └── 多算子 Pipeline（单 SQL 串联多个算子）

P3（平台增强）—— 体验优化
  ├── 异步任务执行（长任务非阻塞 + 进度反馈）
  └── Gateway 统一认证（按部署需求决定）

流式架构 —— 网络流量分析核心能力
  ├── IStreamChannel / IStreamOperator 接口
  ├── Scheduler 多角色任务调度（批处理/流式进程内/流式跨进程）
  ├── DPDK 大页内存零拷贝（netcard 场景）
  └── StreamWorker 通用流式算子容器
```

---

## P1：数据库闭环

### DatabaseChannel 实现

**前置条件**：IDatabaseChannel 接口已定义（`framework/interfaces/idatabase_channel.h`），含 IBatchReader/IBatchWriter 工厂模式。

**目标**：实现 ClickHouse 对接，作为第一个 IDatabaseChannel 具体实现。

**设计要点**：
- ClickHouse 原生支持 `FORMAT Arrow`，Arrow IPC 交换几乎零转换开销
- IBatchReader：执行 SQL 查询，流式返回 Arrow RecordBatch
- IBatchWriter：内部攒批，达到阈值自动 flush，`Close()` 返回写入统计
- 连接管理：通道 Open/Close 对应连接建立/释放，IsConnected() 检测可用性
- 通道标识：`Catelog()` 返回数据库实例名（如 `"clickhouse"`），`Name()` 返回库名（如 `"ts"`）

**配置方式**：通过 `Option()` 传入连接参数（host、port、database、user、password），或从配置文件读取。

### ChannelAdapter 自动适配（已完成）

**设计变更**：去掉 `system.store` 和 `system.extract` 两个显式算子，改为 Pipeline/Scheduler 层自动感知通道类型差异并完成格式转换。参考 Spark/Flink 设计，数据库读写能力下沉到通道层，用户 SQL 中不再需要感知中间搬运过程。

**新 SQL 语法**：
```sql
SELECT [* | col1, col2, ...] FROM <source> [USING <catelog.name>] [WITH key=val, ...] [INTO <dest>]
```

关键变化：USING 子句变为可选。没有 USING 时，Pipeline 做纯数据搬运；有 USING 时，Pipeline 在算子前后自动插入格式转换。

**SQL 示例**：
```sql
-- DataFrame → Database（自动写入，无需 system.store）
SELECT * FROM memory_data INTO clickhouse.my_table

-- Database → DataFrame（自动读取，无需 system.extract）
SELECT * FROM clickhouse.my_table INTO memory_result

-- Database → 算子 → DataFrame（自动适配）
SELECT * FROM clickhouse.my_table USING explore.chisquare

-- DataFrame → 算子 → Database（自动适配）
SELECT * FROM memory_data USING explore.chisquare INTO clickhouse.my_table
```

**实现文件**：
- `framework/core/channel_adapter.h/.cpp`：ChannelAdapter 工具类（ReadToDataFrame / WriteFromDataFrame / CopyDataFrame）
- `framework/core/sql_parser.h/.cpp`：USING 可选 + columns 字段 + HasOperator() 方法
- `services/scheduler/scheduler_plugin.h/.cpp`：类型感知执行路径（FindChannel / ExecuteTransfer / ExecuteWithOperator）

**Scheduler 执行路径**：
```
无算子：
  DataFrame → DataFrame  → CopyDataFrame()
  DataFrame → Database   → WriteFromDataFrame()
  Database  → DataFrame  → ReadToDataFrame()
  Database  → Database   → ReadToDataFrame() + WriteFromDataFrame()

有算子（算子始终面向 IDataFrameChannel）：
  source 是 Database → 先 ReadToDataFrame() 到临时 DFC
  sink 是 Database   → 算子输出到临时 DFC，再 WriteFromDataFrame()
  Pipeline: operator->Work(actual_source, actual_sink)
```

### 清理任务

- ~~删除 IDataEntity 相关死代码~~（已完成：删除 `idata_entity.h`，`DataType`/`FieldValue`/`Field` 移入 `idataframe.h`，清理 `DataFrame` 中 `AppendEntity`/`GetEntity`/`GenericDataEntity` 死代码）

---

## P2：Pipeline 增强

### 多算子 Pipeline

**现状**：Pipeline 只有 source → operator → sink 三个成员，单算子执行。多步分析需用户手动提交多条 SQL（前一条 INTO result_1，后一条 FROM result_1）。

**目标**：支持单条 SQL 串联多个算子。

**SQL 语法扩展**（方案待讨论）：
```sql
-- 方案 A：多 USING 子句
SELECT * FROM test_data USING explore.chisquare WITH threshold=0.05 USING explore.summary

-- 方案 B：PIPE 语法
SELECT * FROM test_data | explore.chisquare WITH threshold=0.05 | explore.summary
```

**Pipeline 改造**：
- 算子列表：`std::vector<IOperator*>` 替代单个 `IOperator*`
- 中间通道：算子之间自动创建临时 DataFrameChannel 传递数据
- Run() 循环调用：`op[0]->Work(source, tmp1)` → `op[1]->Work(tmp1, tmp2)` → ... → `op[n]->Work(tmpN, sink)`

---

## P3：平台增强

### 异步任务执行

**现状**：HandleCreateTask 同步等待 Scheduler 返回，大数据量时前端阻塞。

**目标**：长任务非阻塞执行 + 进度反馈。

**方案**：
- POST /api/tasks 立即返回 task_id + status=running
- Scheduler 在后台线程执行 Pipeline
- 前端轮询 GET /api/tasks/{id} 获取状态（或 WebSocket 推送）
- 任务完成后更新状态为 completed/failed，结果写入数据库

**涉及改动**：
- Scheduler：任务执行改为线程池异步
- Web：HandleCreateTask 改为非阻塞
- 前端：Tasks 页面增加进度轮询/刷新

### Gateway 统一认证

**现状**：无认证，Web 监听 127.0.0.1，仅限本机访问。

**适用场景**：需要暴露公网或多用户访问时。

**方案**：在 Gateway 层做统一认证（而非 Web 层单独做 JWT），所有经过 Gateway 转发的请求都经过认证检查。内部服务间通信（Scheduler ↔ PyWorker）不经过认证。

**优先级**：取决于部署需求，内网使用可暂缓。

---

## 流式架构

以 `SELECT * FROM netcard USING npm INTO ts.db1` 为驱动场景，设计流式处理能力。

### Channel 的本质：描述符，不是数据管道

Channel 是框架层面的命名、受管理的数据源/汇描述符。框架只看到 IChannel（统一管理），算子通过 dynamic_cast 获取具体类型后操作原生数据管道。

```
Channel = 描述符
  ├── 身份（catelog, name）→ SQL 中 FROM/INTO 引用
  ├── 元数据（type, schema）→ 框架做类型检查和展示
  ├── 生命周期（open, close）→ 框架管理资源
  └── 数据管道的入口 → 算子通过 dynamic_cast 获取
       ├── DataFrameChannel: 入口就是自身（Read/Write）
       ├── DatabaseChannel: 入口是工厂方法（CreateReader/Writer）
       └── NetcardChannel: 入口是 GetRing()（返回 rte_ring*）
```

### 流式通道与流式算子

与批处理对称，新增流式层：

```
                    进程内                  跨进程
批处理通道      DataFrameChannel          —
批处理算子      C++ 算子（同步 Work）      PythonOperatorBridge
流式通道        进程内 IStreamChannel      netcard（独立进程，DPDK）
流式算子        进程内 IStreamOperator     npm（独立进程）/ StreamWorker（通用容器）
```

### 通道接口体系

> 注：核心接口继承关系见上方"核心接口体系"章节。以下为流式扩展设计。

IStreamChannel 不限定 IDataFrame 作为数据格式。数据格式由具体子类型决定，算子通过 dynamic_cast 获取所需类型——与批处理算子 cast 到 IDataFrameChannel 是同一模式。

```
IStreamChannel（流式行为基类，不绑定数据格式，待实现）
  ├── NetcardChannel（流式 + packet/rte_mbuf）
  ├── DataFrameStreamChannel（流式 + DataFrame）
  └── ...
```

### 部署模式：是否独立进程取决于复杂度

- 复杂流式算子（如 npm 网络性能分析）→ 独立进程部署
- 简单流式算子 → 放在通用 StreamWorker 里（定位类似 Python Worker：通用流式算子执行容器）

StreamWorker 是框架程序加载流式算子 .so 的实例，与其他 C++ 服务对等，符合"统一框架"原则。

### Scheduler 角色随任务类型变化

| 任务类型 | Scheduler 角色 | 数据路径 |
|---------|---------------|---------|
| 批处理 | 执行者（Pipeline 驱动） | 经过 Scheduler |
| 流式进程内 | 宿主（算子自管理线程） | 在 Scheduler 进程内 |
| 流式跨进程 | 编排者（HTTP 控制指令） | 不经过 Scheduler |

批处理时 Scheduler 在数据路径上（Pipeline 驱动 Work 调用）。流式跨进程时 Scheduler 只做控制面编排，数据直接在服务间流动（如 netcard → ring buffer → npm → ClickHouse）。

### DPDK 大页内存零拷贝

netcard 场景采用 DPDK Primary/Secondary 进程模型实现零拷贝：

- netcard 服务作为 DPDK Primary 进程，初始化大页内存，创建 rte_mempool 和 rte_ring
- npm/StreamWorker 作为 DPDK Secondary 进程，attach 到同一块大页内存
- NIC 通过 DMA 直接将数据包写入大页内存的 rte_mbuf
- rte_ring 传递 mbuf 指针（8 字节），不拷贝数据包本身
- 消费者（npm）通过指针直接访问大页内存中的数据包

整条链路零拷贝：NIC DMA → 大页内存 mbuf → ring 传指针 → 消费者原地读取。

### 流式任务执行流程

以 `SELECT * FROM netcard USING npm INTO ts.db1` 为例（netcard 和 npm 均为独立服务进程）：

```
1. SqlParser 解析 → source=netcard, op=npm, dest=ts.db1
2. Scheduler 查找组件：
   - netcard: Gateway 路由表 /netcard/* → 跨进程流式服务
   - npm: Gateway 路由表 /npm/* → 跨进程流式服务
   - ts.db1: 数据库连接配置
3. 判断为跨进程流式任务 → 创建 DistributedStreamingTask
4. Scheduler 编排：
   GET /netcard/ring_buffers → { "eth0": { "path": "/dev/shm/...", "schema": "..." } }
   POST /npm/start {
     "task_id": 42,
     "source": { "type": "ring_buffer", "path": "/dev/shm/flowsql_netcard_eth0" },
     "sink": { "type": "clickhouse", "database": "ts", "table": "db1" }
   }
5. 立即返回 { "task_id": 42, "status": "running", "type": "streaming" }
6. 数据面持续运行（Scheduler 不参与）：
   netcard → ring buffer → npm → ClickHouse
7. 控制面监控：Scheduler 定期 GET /npm/status/42
8. 停止：POST /npm/stop/42 → Flush → 任务结束
```

npm 算子内部的 Work 实现：

```cpp
int NpmOperator::Work(IChannel* in, IChannel* out) {
    // 从描述符获取实际数据管道
    auto* nc = dynamic_cast<NetcardChannel*>(in);
    auto* ring = nc->GetRing();                    // rte_ring*

    auto* db = dynamic_cast<IDatabaseChannel*>(out);
    db->CreateWriter("npm_results", &writer);      // IBatchWriter*

    // 启动工作线程，异步返回
    running_ = true;
    worker_ = std::thread([this, ring, writer] {
        while (running_) {
            rte_ring_dequeue_burst(ring, mbufs, ...);
            // 网络性能分析 → 结果写入数据库
        }
        writer->Flush();
        writer->Close(nullptr);
        writer->Release();
    });
    return 0;
}
```

### 流式服务的控制协议

Scheduler 通过 HTTP 对流式服务做 start/stop/status 编排：

```
启动：POST /npm/start { "task_id": 42, "source": {...}, "sink": {...} }
监控：GET /npm/status/42 → { "running": true, "batches": 5678, "rows": 1234567 }
停止：POST /npm/stop/42
```

netcard 服务提供 ring buffer 查询：

```
GET /netcard/ring_buffers → { "eth0": { "path": "/dev/shm/...", "schema": "..." } }
```

### 待深入设计

以下事项已识别但尚未展开设计：

- IStreamChannel / IStreamOperator 的具体接口方法
- ITask 统一任务抽象（BatchTask / InProcessStreamingTask / DistributedStreamingTask）
- StreamWorker 控制协议
- 服务发现与任务类型自动判断（Gateway 路由表）
- ring buffer 多消费者策略、满时策略、背压机制
- 流式任务的错误恢复与生命周期管理
- 部署配置中流式服务的声明方式

---

## 关键文件索引

### 架构重构新增/变更（Stage 2→3 过渡期）

| 文件 | 说明 |
|------|------|
| `common/iplugin.h` | 从 framework 拆出，含 IRegister/IPlugin/注册宏 |
| `common/loader.hpp` | PluginLoader 实现 IRegister + IQuerier |
| `common/iquerier.hpp` | 插件查询接口 |
| `framework/interfaces/ibridge.h` | IBridge 纯接口（新增） |
| `framework/interfaces/ichannel.h` | IChannel 纯接口（去掉 IPlugin 继承） |
| `framework/interfaces/ioperator.h` | IOperator 纯接口（去掉 IPlugin 继承） |
| `services/bridge/bridge_plugin.h` | BridgePlugin : IPlugin, IBridge |
| `plugins/example/memory_channel.h` | MemoryChannel : IDataFrameChannel, IPlugin |
| `plugins/example/passthrough_operator.h` | PassthroughOperator : IOperator, IPlugin |
| `python/flowsql/arrow_codec.py` | `_ensure_arrow_table()` 统一转换层 |

### P1 相关（已有/待实现）

| 文件 | 说明 |
|------|------|
| `framework/interfaces/idatabase_channel.h` | IDatabaseChannel 接口（已定义） |
| `framework/interfaces/idataframe.h` | IDataFrame 接口 + DataType/FieldValue/Field 类型定义 |
| `services/scheduler/scheduler_plugin.h` | ChannelAdapter + Pipeline + 类型感知执行 |
