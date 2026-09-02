# Sprint 22 Packet 数据面设计

本文定义 Sprint 22 的实现契约，范围为完整 Story 19.1 和完整 Story 19.2。Story 19.2 采用“现有 NPI 能力可信、只建设 checked façade 和 packet 上下文”的边界，当前处于修订后待复审状态；后续代码、测试和 Sprint 计划必须与确认后的本文保持一致。

## 1. 设计范围与关键决策

### 1.1 设计目标

Sprint 22 建立统一的 packet 块式数据面，使 `pcapfile`、DPDK、AF_XDP 等后续数据源能够输出相同的运行时数据结构，并使 NPI、NPM 和 `packet_filter.v1` 复用同一份分层上下文。

本 Sprint 交付以下能力：

1. 将 `IBlockStreamChannel` 和 `IBlockStreamOperator` 从 Arrow 专用接口改为通用 block payload 接口。
2. 定义 `packet.v1` 逻辑 schema、`PacketDescriptor`、`PacketBatchView` 和 owning `PacketBatchPayload`。
3. 定义 `logical_entity_id + source_id + packet_id` 身份模型。
4. 定义批级强类型 packet context sidecar，并交付完整 `PacketLayerHints`。
5. 复用公共 `eLayer`、`protocol::Layers` 和现有 NPI parser/dispatch，增加 checked `LayerHints()` façade、必要终态传播和上下文映射。
6. 冻结 `PollBlock()`、`BlockLease`、`ReleaseBlock()` 及 packet buffer 的生命周期。
7. 将插件装载改为完整批次的注册、`Option()`、`Load()`、`Start()` 生命周期，并保证 NPI 在 worker 启动前按冻结并发度完成 Hyperscan scratch 初始化。
8. 使用构造型 packet 和构造型 block source 完成协议、安全、并发及生命周期验收。

“可运行骨架”指以下进程内闭环可以独立构造和测试：

```text
synthetic block source
  -> IBlockStreamChannel::PollBlock()
  -> BlockLease
  -> IBlockPayload
  -> PacketBatchPayload / ArrowBlockPayload
  -> IBlockStreamOperator::ProcessBlock()
  -> IBlockStreamChannel::ReleaseBlock()
```

当前 Scheduler 对 `block_stream` 的拒绝门禁保留。本 Sprint 没有真实 packet source，不宣称 SQL Scheduler 的 block stream 生产链路已经可用；Scheduler 装配应在首个真实 block source Story 中完成。

### 1.2 非目标

以下内容不属于 Sprint 22：

- pcap / pcapng 文件读取、多文件归并和 replay 控制。
- DPDK、AF_XDP 等实时采集通道实现。
- NPM flow/session 聚合、TCP 重组和应用层识别。
- `packet_filter.v1` predicate、过滤下推和 SQL `WHERE` 执行。
- HTTP、TLS、DNS 等应用层协议结果。
- packet context 的通用动态属性容器。
- Scheduler 的 block stream SQL 生产路径。

Story 19.2 作为完整 Story 一次性交付。`PacketLayerHints`、checked façade、终态传播、`tunnel_depth` 和至少一种固定隧道样本均在本 Sprint 内交付；NPI 协议能力盘点、逐协议认证、完整 parser 重构和第二套 layer parser 不属于本 Sprint。

### 1.3 当前基线与改造边界

当前代码基线如下：

- `IBlockStreamChannel` 的 data event 和 `ReleaseBlock()` 只接受 `arrow::RecordBatch`。
- `IBlockStreamOperator::OnSchemaReady()` 和 `ProcessBlock()` 同样只接受 Arrow 类型。
- 两个 block stream 接口来自 Sprint 13 的占位设计，仓库内没有生产实现或调用方。
- Scheduler 会明确拒绝 `ChannelType::kBlockStream`。
- `eLayer` 位于公共网络定义中；`protocol::Layers` 固定保存最多 15 层，当前大小为 64 字节。
- NPI `NetworkLayer::Layer()`、现有 parser map/dispatch 和协议语义作为完整可用的可信基础能力，本 Sprint 不重新盘点或认证。
- 现有 `Layer()` 只输出 `protocol::Layers` 和层数，默认从 Ethernet 开始，缺少 `link_type`、结构化解析终态和 packet context 映射；本 Sprint 只补齐这些接口表达。
- `pluginregist()` 宏当前在接口注册后直接调用 `Option()` 并忽略返回值；loader 又在每个 `.so` 注册后立即调用该插件的 `Load()`，`app/main.cpp` 仍逐插件提交单元素加载批次。
- NPI 当前在 `Load() -> Engine::Create() -> Model::operator() -> Engine::Ready()` 中按默认并发度 1 创建 Hyperscan scratch；loader 返回后再调用 `Concurrency(N)` 只修改整数，不会重新创建 scratch。
- `test_npi` 是手工打印程序，`test_framework` 尚未注册 CTest，均不能直接作为 Sprint 验收证据。

本 Sprint 可以直接替换现有占位 block stream ABI，不保留 Arrow-only V1/V2 双轨。所有相关 `.so` 必须整体重新编译，发生虚函数签名变化的接口 IID 必须随 ABI 更新，禁止混用新宿主与旧插件。

## 2. 基础设计与公共依赖

### 2.1 总体分层与数据流

数据契约分为 4 层：

| 层次 | 对象 | 职责 | 所有权 |
| --- | --- | --- | --- |
| 逻辑 schema | `packet.v1` | 定义跨来源一致的字段语义和版本兼容规则 | 与存储实现无关 |
| 通用 block | `IBlockPayload` / `BlockLease` | 表达 payload 类型、schema 和一次 block 租约 | `BlockLease` 独占租约 |
| packet 运行时 | `PacketBatchPayload` / `PacketBatchView` | 保存 descriptor、buffer 引用和强类型 context | payload owning，view non-owning |
| 分层上下文 | `PacketLayerHints` | 保存 NPI 分层结果、解析终态和派生定位字段 | payload 拥有连续 sidecar |

`packet.v1` 不能代替进程内热路径结构，`PacketBatchView` 不能脱离 payload 和 lease 生命周期，sidecar 也不能独立于对应 descriptor 数组存在。

总体数据流如下：

```text
packet source
  -> resolve logical entity / source
  -> fill PacketDescriptor[]
  -> preallocate PacketLayerHints[]
  -> IProtocol::LayerHints(pipeno, ..., &hints[i])
  -> publish immutable PacketBatchPayload
  -> PollBlock() returns move-only BlockLease
  -> downstream reads PacketBatchView and optional hints
  -> optional IProtocol::Identify() uses the same pipeno and hints.layers
  -> ReleaseBlock(std::move(lease)) returns buffers exactly once
```

### 2.2 公共类型归属

公共类型按依赖方向放置，Framework 不反向依赖 NPI 插件实现：

| 文件 | 设计职责 |
| --- | --- |
| `src/common/network/netbase.h` | 保留公共 `eLayer` 定义 |
| `src/common/network/layers.h` | 从 `iprotocol.h` 提取原有 `protocol::MAX_LAYERS` 和 `protocol::Layers`，保持布局与语义兼容 |
| `src/common/network/packet_layer_hints.h` | 定义 `PacketParseStatus` 和 `PacketLayerHints` |
| `src/framework/interfaces/iblock_payload.h` | 定义 `BlockPayloadKind`、`BlockSchemaInfo`、`IBlockPayload` |
| `src/framework/interfaces/iblock_stream_channel.h` | 定义公开 `BlockLease` handle、`BlockPollEvent` 和通用 Channel 接口 |
| `src/framework/interfaces/iblock_stream_operator.h` | 定义通用 block operator 接口 |
| `src/framework/core/block_lease.h/.cpp` | 实现内部 `LeaseState`、exactly-once reclaimer 和 owner queue 归还逻辑，不暴露 backend owner |
| `src/framework/core/packet_batch.h/.cpp` | 定义 descriptor、entity/source 描述、view 和 owning payload |
| `src/framework/core/packet_context.h/.cpp` | 实现 `PacketContextView`、PImpl storage 和强类型 sidecar accessor |
| `src/framework/core/arrow_block_payload.h/.cpp` | 包装 `arrow::RecordBatch` |
| `src/common/iplugin.h` | 保留 `pluginregist` 导出 ABI，移除注册宏对 `Option()` 的隐式调用 |
| `src/common/loader.hpp` | 实现单初始化批次的注册、Option、Load、Start 状态机和失败回滚 |
| `src/app/main.cpp` | 一次提交完整插件列表，并只在批次 Load 成功后调用 `StartAll()` |
| `src/plugins/npi/iprotocol.h` | 继续定义并导出 `IID_PROTOCOL` / `IProtocol`，引用公共 Layers / Hints 类型 |
| `src/plugins/npi/layer.h/.cpp` | 复用现有 parser/dispatch，实现 checked façade 所需的终态传播和输出校验 |
| `src/tests/test_packet_data_plane/benchmark_packet_data_plane.cpp` | 实现 AoS + sidecar 访问模式和 `Layer()` / `LayerHints()` 性能基准 |

`protocol::Layers` 只移动声明位置，不修改字段顺序、字段类型和 `MAX_LAYERS`。实现时使用 `static_assert(sizeof(protocol::Layers) == 64)` 防止无意改变 ABI。

### 2.3 公共设计归属

只有跨 Task 使用或冻结跨 Task 契约的内容进入本章。owner Task 负责冻结和实现契约，consumer Task 只能通过已冻结接口接入，不能各自复制或改写同一语义。

| 公共设计 ID | 契约 | Owner Task | Consumer Task |
| --- | --- | --- | --- |
| C-PLUGIN-LIFECYCLE | 完整插件批次注册、Option、Load、Start、回滚和业务开放顺序 | T0 | T5、T7、T8、T9 |
| C-BLOCK-ABI | 通用 payload、schema、Channel/Operator ABI | T1 | T2、T3、T4、T8、T9 |
| C-LEASE | move-only lease、exactly-once reclaimer、owner queue | T2 | T1、T3、T8、T9 |
| C-PACKET-IDENTITY | logical entity、source registry、entity-level packet allocator | T3 | T4、T7、T8 |
| C-PACKET-SCHEMA | `packet.v1` 字段、版本和 Arrow metadata | T4 | T1、T3、T8 |
| C-LAYERS-ABI | 公共 `eLayer` / `protocol::Layers` / `PacketLayerHints` | T5 | T6、T7、T8、T9 |
| C-NPI-HINTS | 现有 NPI parser/dispatch 复用、checked façade、终态传播和 Layers 映射 | T6 | T5、T7、T8、T9 |
| C-PUBLISH | sidecar 原位构建、build-then-publish、checked accessor | T7 | T3、T8、T9 |
| C-TEST | 构造型样本、CTest、Scheduler 回归 | T8 | T0-T7、T9、T10 |
| C-NONFUNCTIONAL | ABI static_assert、Sanitizer、TSan、性能预算 | T9 | T0-T8、T10 |

文档顺序不表示执行依赖，实际依赖以 `planning.md` Task 表为准。共享文件以 owner Task 为主修改方；consumer Task 若需要改变公共契约，必须先更新 owner Task 设计和 planning 依赖。

### 2.4 插件批次生命周期

C-PLUGIN-LIFECYCLE 冻结单个 `PluginLoader` epoch 的初始化顺序：

```text
R: dlopen 全部 .so，并注册全部 IPlugin/IID
  -> O: 按注册顺序调用全部 Option()，检查每个返回值
  -> L: 仅在全部 Option() 成功后调用全部 Load()，检查每个返回值
  -> C: Load() 返回后、StartAll() 前，由装配方调用 Concurrency(N) 等运行资源配置
  -> S: 按依赖顺序调用全部 Start()
       NPI Start()/Ready() 成功后，后置 packet consumer 的 Start() 才能启动 worker
```

约束如下：

1. `pluginregist()` 只创建静态插件实例并注册接口，不再隐式调用 `Option()`；导出函数签名保持不变，`opt` 由 loader 在 O 阶段传给对应插件。
2. `PluginLoader::Load()` 的一次数组调用是完整初始化批次。`app/main.cpp` 必须一次提交完整插件列表，禁止用多次单插件 `Load()` 拼接一个进程启动批次。
3. 一个 loader epoch 只接受一个初始化批次；第二次 `Load()` 在 `Unload()` 重置前必须失败，避免出现先加载插件已执行 `Load()`、后加载插件尚未注册的混合状态。
4. O 阶段和 L 阶段都以任意非零返回值为失败；失败后不得进入下一阶段或 `StartAll()`。
5. L 阶段完成后开放显式装配窗口。知道 worker 数量的装配方通过 `IQuerier` 获取 `IProtocol`，调用 `Concurrency(N)`；loader 不猜测 worker 数，也不把 `pipeno` 与 source/queue 混合。
6. S 阶段保持注册顺序，插件列表必须按依赖在先、消费者在后排列。NPI 必须排在会启动 packet worker 的消费者之前；consumer 的 `Start()` 只有在 NPI `Start()/Ready()` 成功后才会被调用。后续插件启动失败时，loader 逆序 `Stop()` 已启动的 consumer。
7. `IPlugin::Start()` 采用失败原子性：返回非零前必须自行撤销本次创建的线程、监听器和运行资源，恢复到可 `Unload()` 的 stopped 状态。loader 不对失败插件调用 `Stop()`，只逆序停止此前成功插件；该契约写入 `iplugin.h`，所有覆盖 `Start()` 的实现必须审计。
8. `StartAll()` 中途失败时，对此前成功启动插件按逆序调用 `Stop()` 并进入只允许卸载的失败态；`Unload()` 对已 Load 插件按逆序释放。O/L 失败时移除本批次注册的 IID、关闭 handle，并把 loader 恢复为可重新提交完整批次的空状态。
9. `StopAll()` / `Unload()` 不得与业务调用并发；停止顺序与启动顺序相反。

该契约不新增 `Prepare()` 或第二套插件生命周期。`Concurrency(N)` 是 L 与 S 之间的显式装配动作，NPI 的 scratch finalization 归入现有 `Start()`。

## 3. T0：插件批次生命周期、设计/ABI 基线和公共头归属

### 3.1 设计目标与验收责任

T0 负责 C-PLUGIN-LIFECYCLE，并冻结 Sprint 22 的当前代码基线、公共类型归属、ABI 迁移规则和测试入口基线。它承担 `S19.1-A09` 的插件生命周期部分、`S19.2-A10` 的批次启动顺序，并支撑 `S19.2-A01` 和所有涉及 public ABI / CTest 的实现 Task。

### 3.2 公共依赖

T0 实现第 2.4 节 C-PLUGIN-LIFECYCLE，并维护第 2 章公共设计归属表。T5 消费其 L/S 装配窗口完成 NPI 并发资源初始化，T7 的 packet consumer 排在 NPI 之后启动，T8/T9 验证阶段顺序、回滚和并发门槛。后续 Task 不能绕过 loader 状态机单独调用插件生命周期。

### 3.3 loader 状态机与批次实现

loader epoch 采用以下状态机：

```text
kEmpty -> Load(batch): kRegistering -> kOptioning -> kLoading
  -> success: kLoaded
  -> R/O/L failure + rollback: kEmpty
kLoaded -> StartAll(): kStarting
  -> success: kStarted
  -> failure + reverse Stop: kStartFailed
kStarted -> StopAll(): kLoaded
kLoaded / kStartFailed -> Unload(): kEmpty
```

任一阶段失败不得伪装成前一稳定状态：R/O/L 失败执行本批次回滚后回到 `kEmpty`；S 失败逆序 `Stop()` 已启动插件并进入 `kStartFailed`，只允许随后 `Unload()`，禁止在同一 epoch 直接重试可能已经部分初始化的 `Start()`。`StartAll()` 只接受 `kLoaded`，`StopAll()` 只对 `kStarted` 生效，第二次 `Load()` 在非 `kEmpty` 状态返回失败。

实现要求：

1. 两个 `PluginLoader::Load()` 重载共享同一批次实现，不能各自维护不同的阶段或回滚语义。
2. R 阶段记录每个 `.so` 的 handle、本次注册前各 IID vector 的长度、对应 IPlugin 列表和 option 副本；`plugins_ref_` 只在完整 L 阶段成功后提交。
3. `END_PLUGIN_REGIST()` 只返回插件实例，不调用 `Option()`。loader 对本批次注册的所有 IPlugin 执行 O/L，保留每个插件与 option 的关联。
4. `Option()`、`Load()` 和 `Start()` 均以 `rc != 0` 判定失败，不能只识别 `-1`。
5. O 失败时不调用任何 `Load()`；L 失败时只对已成功 Load 的插件逆序调用 `Unload()`；两条路径都截断本批次新增 IID vector 并关闭已打开 handle。
6. `app/main.cpp` 先解析完整 plugin list 和逐插件 option，再一次调用数组版 `Load()`；不得保留 `LoadPluginOnly()` 循环造成的伪批次。
7. 已启动后不支持热加载。当前测试若在 `StartAll()` 后追加插件，必须改为初始完整批次或独立 loader epoch，不能为测试保留生命周期漏洞。

### 3.4 ABI 与兼容基线

1. `protocol::Layers` 布局和大小保持不变，只迁移到公共头文件。
2. `eLayer` 直接复用，现有枚举数值不变。
3. `PacketDescriptor` 必须保持 standard-layout、trivially-copyable、固定 alignment、固定 `sizeof` 和关键字段 offset；`PacketLayerHints` 同样必须保持 trivially-copyable，sidecar 扩展只能新增独立 typed storage/accessor。
4. `IBlockStreamChannel`、`IBlockStreamOperator` 和 `IProtocol` 的虚函数表发生变化，必须生成并替换新的 IID 值、整体重编插件，并在宿主侧拒绝旧 IID；旧 IID 不得复用或通过兼容分支继续接受旧插件。descriptor / hint 布局变化也必须触发同样的 ABI 迁移评审。
5. `IBlockPayload` 不定义 IID；payload 判型使用 `BlockPayloadKind + schema`。
6. `packet.v1` major 1 的字段语义冻结；新增可选字段只提升 minor。
7. `PacketLayerHints` 是进程内公共结构，不作为跨版本持久化二进制格式。
8. Arrow `RecordBatch` 通过 wrapper 保留，现有 `IStreamChannel` 行为不变。
9. Scheduler 的 `BLOCK_STREAM_NOT_IMPLEMENTED` 错误在真实 block source 接入前保持不变。
10. `pluginregist(IRegister*, const char*)` 导出签名不变，但 `Option()` 的调用责任从注册宏迁移到 loader；所有插件和宿主仍必须整体重编并接受新的批次语义。

### 3.5 文件落点

- `tasks/sprints/sprint22-packet-data-plane/planning.md` / `design.md`：冻结 Task、公共设计和验收关系。
- `src/common/iplugin.h`：注册宏只注册接口并返回 IPlugin。
- `src/common/loader.hpp`：批次阶段、状态检查、option 传播和失败回滚。
- `src/app/main.cpp`：一次提交完整插件集合，在 L/S 之间保留显式装配窗口。
- `src/services/{web,stream,gateway,task,scheduler,database,catalog,bridge,router,binaddon}/*plugin.cpp`、`src/plugins/baseline/baseline_plugin.cpp`：审计现有 `Start()` override 的失败原子性，仅对不满足契约的实现做最小修复。
- `src/tests/test_framework/test_plugin_lifecycle.cpp`、`fixtures/plugin_lifecycle_*.cpp`：生产 loader 的跨 `.so` 事件顺序和失败回滚测试。
- `src/CMakeLists.txt`：确认新增测试目录和 CTest 总入口。
- 第 2.2 节列出的公共头：由 T1、T3、T5 分别实现，T0 只冻结归属和 ABI 迁移要求。

### 3.6 测试设计与通过门槛

T0 使用至少 3 个可记录事件的测试插件验证跨 `.so` 批次，不以 mock loader 替代生产 `PluginLoader`：

1. 事件日志严格满足所有 `register` 先于任一 `Option`、所有 `Option` 先于任一 `Load`、所有 `Load` 先于任一 `Start`；`Stop` / `Unload` 顺序相反。
2. 前置插件的 `Load()` 可以查询后置 `.so` 在 R 阶段注册的接口。
3. 任一插件的 `Option()` 失败时没有 `Load/Start`；任一 `Load()` 失败时没有 `Start`，且只逆序 Unload 已成功 Load 的插件；fixture 在创建部分线程/资源后返回 Start 失败时必须先自回滚，loader 再逆序 Stop 此前成功插件、进入 `kStartFailed`，随后完整 Unload 且不能直接重试 Start。
4. 回滚后没有本批次悬挂 IID/handle，loader 可以重新提交完整批次；启动后增量 `Load()` 被拒绝。
5. `app/main.cpp` 的多插件启动走单次批量 `Load()`，不再逐插件调用。
6. T0 枚举并审计仓库当前全部 11 个非 NPI `Start()` override，逐项记录“失败点、失败前已创建资源、自回滚路径、自动化测试”；发现不满足失败原子性时，在所属插件做最小修复并增加定向失败测试。T5 单独审计本 Sprint 新增的 NPI `Start()`。
7. 当前 `sizeof(protocol::Layers) == 64`，`eLayer` 数值基线由 compile-time assertion 固化。
8. 当前 Scheduler 对 `kBlockStream` 的拒绝路径存在；T8 必须增加生产路径回归测试，而不是删除门禁。
9. 所有发生虚函数签名变化的 IID 都进入 ABI 差异清单，旧 IID 加载测试失败。

T0 将生命周期用例接入 `test_framework` 并注册 CTest，直接运行该目标作为自身完成门槛；T8 在整体验收命令中复用并确认该 CTest 未被遗漏。测试必须使用确定的事件序号断言，不以日志文本人工检查替代。

### 3.7 完成出口

- C-PLUGIN-LIFECYCLE 在 loader、注册宏和应用入口中形成单一实现，顺序和失败回滚测试通过。
- 当前 11 个非 NPI `Start()` override 均有失败原子性审计证据；发现的问题已最小修复并有对应失败测试，NPI 审计由 T5 接管。
- 公共设计 owner/consumer、文件归属和 IID 迁移清单已冻结。
- T0-T9 的 Task 名称、依赖和设计章节与 planning 一致。
- CMake/CTest、ABI 和 Scheduler 当前基线均有后续验证 Task 接管。

## 4. T1：通用 payload、schema、Channel/Operator ABI 和 Arrow wrapper

### 4.1 设计目标与验收 ID

T1 负责 C-BLOCK-ABI，关闭 `S19.1-A01` 和 `S19.1-A02`。它定义通用 payload、schema 判型、Channel/Operator 公共接口和 Arrow wrapper，不实现 lease 回收策略、packet 物理布局或 `packet.v1` 字段映射。

### 4.2 公共依赖

T1 依赖 T0 冻结的公共文件归属和 ABI 迁移规则，并向 T2、T3、T4、T8、T9 提供 C-BLOCK-ABI。

### 4.3 类型和 schema

`IBlockPayload` 是结构对象的公共基类，不是可注册或查询的插件服务，因此不使用 IID 机制。IID 只用于 `IBlockStreamChannel`、`IBlockStreamOperator`、`IProtocol` 等接口的注册和查询。

接口模型：

```cpp
enum class BlockPayloadKind : uint16_t {
    kUnknown = 0,
    kArrowRecordBatch = 1,
    kPacketBatch = 2,
};

struct BlockSchemaInfo {
    std::string schema_name;
    uint16_t major = 0;
    uint16_t minor = 0;
};

interface IBlockPayload {
    virtual ~IBlockPayload() = default;

    virtual BlockPayloadKind Kind() const noexcept = 0;
    virtual const BlockSchemaInfo& Schema() const noexcept = 0;
    virtual size_t ItemCount() const noexcept = 0;
};
```

约束如下：

- `BlockPayloadKind` 表示 C++ 承载形态，不等同于逻辑 schema。
- `schema_name + major + minor` 表示逻辑数据契约。
- `packet.v1` 的规范值为 `schema_name = "packet"`、`major = 1`、`minor = 0`，对外写作 `packet.v1`。
- major 变化表示不兼容；minor 只能增加可选字段或能力，不能改变已有字段语义。
- 消费者必须校验 `Kind()` 和 schema，再使用公共 checked helper 转换为具体 payload。
- 不使用 `void*`、字符串 type name 或 IID 对 payload 判型。
- schema 由 channel 和 payload 自身持有，返回引用分别在 channel 或 payload 生命周期内稳定；禁止返回临时字符串指针。

minor 版本采用“最低能力 + 可忽略扩展”规则：消费者声明自己所需的 `required_minor`，生产者的 `minor < required_minor` 时拒绝；生产者的 `minor >= required_minor` 时允许消费，消费者必须忽略未知的可选字段。依赖 sidecar 的消费者不能只比较 minor，而必须通过具名 accessor 检查 sidecar present 和自身需要的字段能力。

### 4.4 Arrow 兼容

Arrow 通过 `ArrowBlockPayload` 包装：

```cpp
class ArrowBlockPayload final : public IBlockPayload {
 public:
    explicit ArrowBlockPayload(std::shared_ptr<arrow::RecordBatch> batch,
                               BlockSchemaInfo schema);

    const arrow::RecordBatch& Batch() const noexcept;
};
```

构造函数拒绝空 batch；`Batch()` 只返回 lease 生命周期内有效的只读借用引用，不向下游暴露可复制的 `shared_ptr`，因此 Arrow payload 不能绕过 lease 延长 buffer 生命周期。`BlockPollEvent` 不再直接暴露裸 `shared_ptr<arrow::RecordBatch>`。Arrow 仍可用于：

- 现有 DataFrame / Stream 生态互操作。
- 边界导出、调试和持久化。
- 将 `packet.v1` 映射为 Arrow schema。

packet 热路径不要求构造 Arrow array，也不为了 schema 访问而序列化 packet。

### 4.5 Payload checked accessor

通用消费者使用真实动态类型校验的 checked accessor，不直接根据 `Kind()` 做 `static_cast`：

```cpp
class PacketBatchPayload;
class ArrowBlockPayload;

const PacketBatchPayload* TryAsPacketBatch(const IBlockPayload& payload) noexcept;
const ArrowBlockPayload* TryAsArrowBlock(const IBlockPayload& payload) noexcept;
```

声明分别归入 `packet_batch.h` 和 `arrow_block_payload.h`，实现位于对应 `.cpp`。实现先使用 `dynamic_cast` 验证实际 C++ 类型，再校验 `Kind()`、schema name 和 major；任一条件不满足均返回 `nullptr`。`Kind()` 和 schema 仍作为快速拒绝条件，但不能代替真实类型校验。测试必须覆盖 Kind 正确但动态类型不匹配、schema major 不匹配和空 payload 三种拒绝路径。

### 4.6 Channel 接口与事件模型

```cpp
class BlockLease {
 public:
    BlockLease() noexcept;
    BlockLease(BlockLease&& other) noexcept;
    BlockLease& operator=(BlockLease&& other) noexcept;
    BlockLease(const BlockLease&) = delete;
    BlockLease& operator=(const BlockLease&) = delete;
    ~BlockLease() noexcept;

    explicit operator bool() const noexcept;
    const IBlockPayload* Payload() const noexcept;
};

struct BlockPollEvent {
    enum Kind {
        kData,
        kTimeout,
        kEof,
        kCancelled,
        kError,
    };

    Kind kind = kTimeout;
    BlockLease lease;
    int err = 0;
};

interface IBlockStreamChannel : public IChannel {
    virtual const BlockSchemaInfo& OutputSchema() const noexcept = 0;
    virtual BlockPollEvent PollBlock(int timeout_ms = 100) = 0;
    virtual int ReleaseBlock(BlockLease&& lease) = 0;
    virtual void Cancel() = 0;
    virtual bool IsFinished() const = 0;
};
```

默认构造的 `BlockLease` 为 Invalid。仅 `kData` event 携带有效 lease，其他 event 的 lease 必须为空。`kError` 使用 `err` 表达错误码；错误信息继续遵循框架现有错误契约，不在热路径 event 内新增动态字符串。

`OutputSchema()` 在 channel 初始化完成后即可调用，并在 channel 生命周期内不可变。执行器先调用 `OnSchemaReady(OutputSchema())`，再开始 `PollBlock()`；每个 data event 的 payload schema 必须与 channel output schema 相同。schema 变化需要新建 channel 实例，不能通过消费首个 block 隐式探测。

### 4.7 Operator 借用规则

`IBlockStreamOperator` 同步解除 Arrow 绑定：

```cpp
virtual int OnSchemaReady(const BlockSchemaInfo& schema) = 0;
virtual int ProcessBlock(const IBlockPayload& payload) = 0;
```

Operator 只借用 payload：

- 不接收、不保存和不释放 lease。
- 不得将 payload、view 或 packet 指针保存到 `ProcessBlock()` 返回之后。
- 返回 `0` 表示继续，`1` 表示主动停止，负数表示错误；任何返回路径都由执行器释放 lease。
- `OnSchemaReady()` 校验 schema name/major/minor，不能通过消费首个 block 探测 schema。
- 执行器必须在 `OnSchemaReady()` 和 `ProcessBlock()` 外层捕获 `...`；异常先转换为结构化负错误，再释放当前 lease，不能让 C++ 异常穿过插件 ABI。执行器或 operator 若需跨调用保存 schema，只能复制 `BlockSchemaInfo` 值，禁止保存 `OutputSchema()` 的引用。
- packet 事件时间来自每个 `PacketDescriptor::ts_ns`；若未来需要 batch watermark 或调度时间，使用独立、具名的 execution context 扩展，不复用含义不明的 `ts_ms`。

构造型执行器测试必须覆盖成功、主动停止、operator 错误和异常兜底 4 条释放路径。

### 4.8 文件落点、测试设计与完成出口

- `src/framework/interfaces/iblock_payload.h`：通用 payload 和 schema。
- `src/framework/interfaces/iblock_stream_channel.h`：Channel、`BlockPollEvent` 和公开 lease handle。
- `src/framework/interfaces/iblock_stream_operator.h`：通用 operator。
- `src/framework/core/arrow_block_payload.h/.cpp`：Arrow wrapper 和 checked accessor。

`test_packet_data_plane` 必须覆盖 Kind/schema/动态类型拒绝、首块前 `OutputSchema()`、Packet/Arrow 分派和 operator 异常路径；`test_stream` 必须证明既有 Arrow stream 无回退。上述接口、IID 和测试全部闭合后，T1 才能完成。

## 5. T2：`BlockLease`、LeaseState、exactly-once reclaimer、Cancel/Close 和 owner queue

### 5.1 设计目标与验收 ID

T2 负责 C-LEASE，关闭 `S19.1-A07`，并承担 `S19.1-A08` 中构造型 block source 生命周期闭环。它不实现真实 packet source，也不解除 Scheduler 的生产门禁。

### 5.2 公共依赖

T2 消费 T1 的 C-BLOCK-ABI，并向 T3、T8、T9 提供稳定的 lease/reclaimer 契约。跨线程归还必须遵守 T0 冻结的 ABI 和 owner thread 约束。

### 5.3 所有权状态机

```text
Invalid
  ^
  |
PollBlock(kData)
  |
  v
Active --move--> Active(new object) + Invalid(old object)
  |
  +-- ReleaseBlock(BlockLease&&) --> Released / Invalid
  |
  +-- ~BlockLease() -------------> Released / Invalid (异常兜底)
```

生命周期契约：

1. `PollBlock(kData)` 返回唯一的 move-only lease。
2. payload、descriptor、sidecar、entity/source 描述和 packet data 只在 lease 为 Active 时有效。
3. 正常路径必须调用 `ReleaseBlock(std::move(lease))`；返回 `0` 时调用成功并消费 lease。
4. `BlockLease` 析构执行 `noexcept` 兜底释放，覆盖异常、提前返回和调用方遗漏。
5. release state 必须保证底层资源 exactly once 回收；显式释放和析构兜底不能重复归还 buffer。
6. moved-from lease 为空；访问空 lease 返回 `nullptr`，再次释放返回参数错误且不触发回收。
7. lease 记录所属 channel/reclaimer identity。跨 channel 释放返回参数错误，不接管、不失效该 lease；原 lease 仍为 Active，并由正确 channel 的显式释放或自身析构兜底归还。
8. `ReleaseBlock()` 返回非 0 时不得遗失 Active 资源；除无效 lease 外，调用方对象保持 Active，离开作用域时仍会兜底释放。
9. move assignment 的目标若已持有 Active lease，必须先通过目标自身 reclaimer 完成兜底释放，再接管源 lease。
10. `lease = std::move(lease)` 是自移动 no-op，不改变 Active/Invalid 状态且不触发回收。
11. `Cancel()` 只设置取消状态、唤醒等待者并停止新生产，然后立即返回；它不等待 outstanding lease，避免消费线程等待自身造成死锁。
12. 通道关闭不得回收仍由 Active lease 引用的 buffer，稳定 release state 必须存活到最后一个 lease 归还。
13. `IsFinished()` 表示不会再产生新 block，不表示所有已发出的 lease 都已释放。
14. `PacketBatchView` 不持有 `shared_ptr`，不能绕过 lease 延长生命周期。

lease 内部使用共享 release state 或等价的稳定 reclaimer handle，使通道对象先于 lease 析构时仍能安全完成兜底释放。`LeaseState` 是 backend 回收 callback 的唯一 owner；`PacketBatchPayload` 只描述数据，不执行 backend 归还。释放顺序固定为：使 lease 失效、销毁 payload/view backing、最后调用一次 backend recycler。该共享状态不暴露给下游，也不允许复制 lease。

backend recycler callback 必须是 `noexcept` 且幂等保护由 `LeaseState` 提供；owner queue 在 `PollBlock()` 时为每个 Active lease 预留归还 token，正常归还路径不得因队列满而失败。内部 invariant 破坏只增加 fatal diagnostic 并终止当前 block source，不在析构路径抛异常、重试或吞掉资源所有权。

### 5.4 文件落点、测试设计与完成出口

- `src/framework/core/block_lease.h/.cpp`：`LeaseState`、reclaimer 和 owner queue。
- `src/framework/interfaces/iblock_stream_channel.h`：move-only lease 公开行为。
- `src/tests/test_packet_data_plane/`：生命周期和构造型 block source 用例。

测试必须覆盖 move/moved-from、自移动、显式释放、析构兜底、跨 channel 错误、channel 先析构、Cancel 后 outstanding lease、owner queue 跨线程归还和 exactly-once。所有路径都能释放且 TSan 下无 data race 后，T2 才能完成。

## 6. T3：Descriptor、buffer range、logical entity/source registry、allocator、View/Payload 和 context PImpl

### 6.1 设计目标与验收 ID

T3 负责 C-PACKET-IDENTITY 和 packet 运行时承载，关闭 `S19.1-A04`、`S19.1-A05`、`S19.1-A06`。它只定义 sidecar 容器和 accessor，不在本 Task 中实现 NPI 解析或填充。

### 6.2 公共依赖

T3 消费 T1 的 C-BLOCK-ABI 和 T2 的 C-LEASE，并向 T4、T7、T8 提供 identity、descriptor、view/payload 和 context PImpl。

### 6.3 逻辑实体

`logical_entity_id` 表示一个可共同分析的数据逻辑实体。一个逻辑实体可包含多个有内在关系的数据来源，例如：

- 两个或多个网卡。
- pcap / pcapng 文件。
- 日志数据。
- 配置数据。

`logical_entity_id` 是稳定字符串，用于配置、持久化、跨进程交换和结果关联。进程内可将其 intern 为数值 handle，避免在 packet 热路径重复保存和比较字符串；handle 只在当前进程有效，不能代替稳定 ID 对外输出。

每个 `PacketBatchPayload` 只属于一个逻辑实体，但允许包含该实体下多个 packet 来源的数据。

### 6.4 来源注册表

逻辑实体拥有带固定版本的 `SourceDescriptor` registry。registry 更新只能生成新版本，不能原地改变已发布版本：

```cpp
enum class SourceKind : uint16_t {
    kUnknown = 0,
    kNetworkInterface,
    kPacketFile,
    kLog,
    kConfiguration,
};

struct SourceDescriptor {
    uint32_t source_id = 0;
    SourceKind kind = SourceKind::kUnknown;
    std::string name;
    std::string locator;
};

struct LogicalEntityDescriptor {
    std::string logical_entity_id;
    uint64_t registry_version = 0;
    std::vector<SourceDescriptor> sources;
};
```

`SourceDescriptor` 负责来源类型和定位信息，packet 热路径只保存实体内唯一的 `uint32_t source_id`。日志或配置来源可以存在于 registry 中，但只有能够产生 packet 的来源才会出现在 `PacketDescriptor` 中。

`PacketBatchPayload` 必须持有 `std::shared_ptr<const LogicalEntityDescriptor>` 或等价的完整 immutable snapshot，固定一个 `registry_version` 直到最后一个 lease 释放；禁止只保存 registry 裸指针。这样同一 `source_id` 在 block 生命周期内不会改变含义。

### 6.5 Packet identity

身份规则如下：

- packet 的唯一键是 `(logical_entity_id, packet_id)`。
- `packet_id` 在一个逻辑实体的 packet 范围内唯一。
- 不同逻辑实体允许出现相同 `packet_id`。
- `packet_id` 不要求跨来源或全局单调；多个来源合并到同一实体时，producer 必须通过实体级 allocator 协调唯一性。
- 并发 source 通过原子预留不重叠的 ID 区间或串行分配取得 ID，source 不得各自从 0 开始。
- pcap 可复现读取使用固定输入顺序和固定 packet ordinal 生成 ID；实时采集若要继续使用同一 `logical_entity_id`，必须恢复持久化 high-water mark，否则创建新的实体 ID（或新的实体 epoch ID），不得复用未知旧 ID。
- `source_id` 在逻辑实体内唯一，只表达来源，不参与 packet 唯一键。
- `rx_queue` 只表达接收队列，不能复用为 `source_id`。

### 6.6 Packet 运行时布局

#### 6.6.1 `PacketDescriptor` AoS

单 packet 常用元数据采用 AoS，避免每处理一个 packet 都从多个字段数组取值：

```cpp
constexpr uint16_t kUnknownRxQueue = UINT16_MAX;

struct PacketDescriptor {
    const uint8_t* data = nullptr;
    int64_t ts_ns = 0;
    uint64_t packet_id = 0;
    uint32_t source_id = 0;
    uint32_t link_type = 0;
    uint32_t cap_len = 0;
    uint32_t wire_len = 0;
    uint16_t rx_queue = kUnknownRxQueue;
    uint16_t reserved = 0;
};
```

在 64 位平台上目标大小约为 48 字节，使用 `static_assert` 固化实际布局。`reserved` 必须为 0，为兼容扩展保留，不能提前复用。

实现必须至少固化 `std::is_standard_layout_v<PacketDescriptor>`、`std::is_trivially_copyable_v<PacketDescriptor>`、`alignof(PacketDescriptor) == alignof(void*)`、`sizeof(PacketDescriptor) == 48` 以及 `offsetof` 的字段顺序；具体平台若不满足 48 字节，必须显式拒绝该 ABI，而不是静默接受不同布局。

descriptor 校验规则：

- `cap_len > 0` 时 `data` 不得为空。
- `wire_len >= cap_len`。
- `source_id` 必须能在当前 logical entity 的 registry 中解析。
- 同一 payload 内不得出现重复的 `packet_id`；producer 还必须保证实体生命周期内唯一。
- `link_type` 使用标准 pcap LINKTYPE 数值，不自定义第二套枚举。
- `rx_queue` 不适用时写 `kUnknownRxQueue`。

#### 6.6.2 强类型批级 sidecar

packet context 使用与 descriptor 同索引的批级强类型 sidecar：

```cpp
class PacketContextView {
 public:
    bool HasLayerHints() const noexcept;
    const PacketLayerHints* LayerHintsData() const noexcept;
    size_t LayerHintsCount() const noexcept;

 private:
    struct Impl;
    const Impl* impl_ = nullptr;
};
```

规则如下：

- sidecar 缺失时指针为空且 count 为 0。
- sidecar 存在时 count 必须等于 packet count，`layer_hints[i]` 对应 `descriptors[i]`。
- 不允许部分长度 sidecar；某个 packet 未解析时使用 `PacketParseStatus::kNotParsed`。
- 不使用 `void*`、`std::any`、字符串 key 或 per-packet 多态对象。
- 新 context 通过稳定的 PImpl storage 和新的具名、强类型 accessor 增加；不向公开 context struct 追加成员，不使用运行时字符串查找。
- payload 发布后 descriptor、sidecar 和 registry 均为只读。

这里的 sidecar 是“按上下文类型分栏”的混合布局，不是把 `PacketLayerHints` 的每一个字段继续拆成独立数组。这样既避免 descriptor 被约 80 字节的 hints 膨胀，也保证同一个 packet 的完整 hints 在 sidecar 内连续。

PImpl 只增加一次 batch 级间接访问；具体 sidecar 仍是连续强类型数组。`PacketContextView` 的公开布局固定为一个指针，后续增加 accessor 不改变已有对象布局。

#### 6.6.3 Payload 和 view

```cpp
struct PacketBufferView {
    const uint8_t* data = nullptr;
    size_t size = 0;
};

class PacketBatchView {
 public:
    size_t Size() const noexcept;
    const PacketDescriptor& Packet(size_t index) const;
    size_t BufferCount() const noexcept;
    PacketBufferView Buffer(size_t index) const;
    const LogicalEntityDescriptor& LogicalEntity() const noexcept;
    const PacketContextView& Context() const noexcept;
};

class PacketBatchPayload final : public IBlockPayload {
 public:
    PacketBatchView View() const noexcept;
};
```

`PacketBatchPayload` 拥有：

- 连续的 `PacketDescriptor[]`。
- 可选、连续的 `PacketLayerHints[]`。
- logical entity/source registry 的不可变固定版本引用或快照。
- 一个或多个只读 backend buffer range 描述；真正的 owner/reclaimer 只由 `LeaseState` 持有。

`PacketBatchView` 只保存指针、数量和借用引用，不拥有这些对象；`Buffer(i)` 同样是只读借用 range，不延长 buffer 生命周期。每个非空 packet 的 `[data, data + cap_len)` 必须完整落在一个已 pin 的 buffer range 内；不支持的分段 packet 必须在 source 侧线性化。构造阶段统一完成 schema、identity、长度、sidecar 对齐和 buffer 覆盖范围校验；校验成功后才能发布 payload。

### 6.7 文件落点、测试设计与完成出口

- `src/framework/core/packet_batch.h/.cpp`：identity、descriptor、view 和 owning payload。
- `src/framework/core/packet_context.h/.cpp`：PImpl 和 typed sidecar accessor。
- `src/tests/test_packet_data_plane/`：identity、descriptor、buffer range、view/payload 和 context 测试。

测试必须覆盖同实体跨 block 重复 ID、跨实体重复允许、多 source registry、allocator 分区与恢复、descriptor 长度/来源校验、buffer range、sidecar absent/alignment/只读发布。所有 public layout static_assert 和 lease 借用边界成立后，T3 才能完成。

## 7. T4：`packet.v1` schema、Arrow metadata 和兼容检查

### 7.1 设计目标与验收 ID

T4 负责 C-PACKET-SCHEMA，关闭 `S19.1-A03`。它定义逻辑 schema 和边界编码，不改变 T3 的运行时 AoS/PImpl 布局，也不把 Arrow 变成 packet 热路径主表示。

### 7.2 公共依赖

T4 消费 T1 的 BlockSchemaInfo/Arrow wrapper 和 T3 的 C-PACKET-IDENTITY、descriptor 语义，并向 T8 提供 schema/metadata 验收契约。

### 7.3 `packet.v1` 逻辑 Schema

`packet.v1` 是逻辑语义契约，不规定 Arrow、AoS 或 SoA 的物理布局。

| 字段 | 逻辑类型 | 可空 | 语义 |
| --- | --- | ---: | --- |
| `logical_entity_id` | UTF-8 string | 否 | 稳定的逻辑实体 ID；运行时可按 batch intern |
| `ts_ns` | int64 | 否 | packet 事件时间，Unix epoch 纳秒 |
| `source_id` | uint32 | 否 | 逻辑实体内唯一的来源 ID |
| `link_type` | uint32 | 否 | 标准 pcap `LINKTYPE_*` 数值 |
| `cap_len` | uint32 | 否 | 实际捕获并可安全访问的字节数 |
| `wire_len` | uint32 | 否 | 原始线上 packet 长度，必须满足 `wire_len >= cap_len` |
| `rx_queue` | uint16 | 是 | 接收队列；文件、日志等不适用来源为空 |
| `packet_id` | uint64 | 否 | `logical_entity_id` 内唯一的 packet ID |
| `data` | binary | 否 | packet 捕获字节，长度必须等于 `cap_len` |

Arrow 导出 `packet.v1` 时：

- schema metadata 写入 `flowsql.schema.name=packet`、`flowsql.schema.major=1`、`flowsql.schema.minor=0`。
- batch 级 `logical_entity_id` 物化为逻辑列，允许使用 dictionary encoding 降低重复字符串开销。
- `rx_queue == kUnknownRxQueue` 映射为 null。
- `PacketLayerHints` 不强行塞入 `packet.v1` 1.0 的核心列；后续边界格式需要导出 hints 时，以可选字段和 minor 版本扩展。

运行时 `PacketDescriptor` 的数值字段使用当前进程的 native representation；packet `data` 内的协议头保持网络字节序，由 parser / checked accessor 负责转换。Arrow 或其他边界格式按其标准 canonical numeric representation 编码，不把运行时 AoS 布局当作序列化格式。

兼容规则：

- 消费者必须要求 schema name 和 major 精确匹配。
- 新 minor 只能新增可选字段或可选 sidecar，旧消费者可以忽略未知扩展。
- 必填字段、单位、唯一性范围和 null 语义不能在同一 major 内改变。
- 运行时消费者若依赖某个 sidecar，必须显式检查其 present 状态，不能只根据 minor 推断数据一定存在。

### 7.4 文件落点、测试设计与完成出口

- `src/framework/core/packet_batch.h/.cpp`：运行时字段到逻辑 schema 的映射。
- `src/framework/core/arrow_block_payload.h/.cpp`：Arrow metadata 和边界转换。
- `src/tests/test_packet_data_plane/`：schema/metadata/compatibility 测试。

测试必须覆盖全字段、null 语义、major 拒绝、producer minor 低于 required minor 拒绝、未知高 minor 可忽略、缺失必需 sidecar capability 拒绝。schema 文档、Arrow metadata 和测试一致后，T4 才能完成。

## 8. T5：公共 Layers/Hints、NPI façade、pipeno 和分阶段初始化

### 8.1 设计目标与验收 ID

T5 负责 C-LAYERS-ABI，关闭 `S19.2-A01`、`S19.2-A02`、`S19.2-A04`，并承担 `S19.2-A10` 的 NPI 初始化和 `pipeno` 契约。它定义公共结构、façade 和 `Concurrency -> Ready` 顺序，不重新盘点、认证或改变 NPI 协议能力。

### 8.2 公共依赖

T5 消费 T0 的 C-PLUGIN-LIFECYCLE 和 ABI 基线，向 T6、T7、T8、T9 提供公共 Layers/Hints、已启动 NPI 和接口契约。`IProtocol` 继续通过 IID 注册查询，`PacketLayerHints` 是结构对象，不新增 IID。

### 8.3 状态和结构

直接复用公共 `eLayer` 和 `protocol::Layers`，不新增 layer enum，也不复制一套 layer type/offset 数组。

```cpp
enum class PacketParseStatus : uint8_t {
    kNotParsed = 0,
    kComplete,
    kPartial,
    kTruncated,
    kMalformed,
    kLayerLimit,
    kUnsupportedLinkType,
};

constexpr uint32_t kInvalidLayerOffset = UINT32_MAX;
constexpr uint16_t kInvalidL4Protocol = UINT16_MAX;

struct PacketLayerHints {
    protocol::Layers layers;
    uint32_t l2_offset;
    uint32_t l3_offset;
    uint32_t l4_offset;
    uint16_t l4_protocol;
    uint8_t ip_version;
    uint8_t vlan_depth;
    uint8_t tunnel_depth;
    PacketParseStatus parse_status;

    bool truncated() const noexcept {
        return parse_status == PacketParseStatus::kTruncated;
    }

    bool malformed() const noexcept {
        return parse_status == PacketParseStatus::kMalformed;
    }
};
```

`truncated` 和 `malformed` 是从单一终态枚举派生的查询，不存储重复 bool。`PacketLayerHints` 不包含 `vector`、`string`、`shared_ptr` 或虚函数，必须保持 trivially copyable。

实现必须使用 `std::is_trivially_copyable_v<PacketLayerHints>` 和固定字段 offset 的 `static_assert`；sidecar 后续扩展不得通过修改该结构尾部来完成。

sidecar 分配后必须对每个 slot 执行一次最小语义初始化，而不是依赖 `new T[n]` 的未定义内容：设置 `layers.layercount = 0`、`layers.payload = 0`、`l2/l3/l4_offset = kInvalidLayerOffset`、`l4_protocol = kInvalidL4Protocol`、`ip_version/vlan_depth/tunnel_depth = 0`、`parse_status = kNotParsed`。该初始化不清理 15 个 layer 槽位；解析入口在任何提前返回前都重复完成这些语义字段的重置。

`layers` 是原始层序列、layer offset 和 payload offset 的唯一事实来源：

- `layers.layercount <= protocol::MAX_LAYERS`。
- 只接收 checked façade 已确认可表示的 NPI layer；终态对应的失败层不进入数组。
- 有效 layer offset 单调递增且小于 `cap_len`；派生 offset 使用 32 位表示，`UINT32_MAX` 只表示 absent。
- `layers.payload` 表示 NPI 已解析前缀后的 checked offset；内部 cursor 使用 32 位计算，写入该 `uint16_t` 字段前必须同时满足 `cursor <= cap_len` 和 `cursor <= UINT16_MAX`，否则产生 `kLayerLimit`，禁止先窄化再校验。
- `top layer` 由 `layers.layers[layercount - 1].layer` 派生；`layercount == 0` 时为 `eLayer::NONE`。
- 调用方不得在 `layercount == 0` 时直接调用现有不带保护的 `Layers::Top()`。
- 未使用的 `layers.layers[]` 槽位内容未定义，任何调用方和序列化逻辑都只能读取 `[0, layercount)`。

### 8.4 派生字段

派生字段默认指向最内层有效协议：

- `l2_offset`：从已提交 layer 序列尾部反向查找最内层有效 L2 层（`ETHERNET`、`VLAN`、`PPPoE_Session`、`PPP`、`MPLS`）的 offset。QinQ / MPLS stack 取最内层 NPI layer；隧道内层优先于外层。
- `l3_offset`：最内层 IPv4 或 IPv6 基础头起点，不指向 IPv6 扩展头。
- `l4_offset`：最内层 TCP、UDP 或 SCTP 头起点；未知协议或非首分片时为 `kInvalidLayerOffset`。
- `ip_version`：与 `l3_offset` 对应，只允许 0、4、6；没有有效 IP 层时为 0。
- `l4_protocol`：记录最内层 IP 的扩展链解析完成后、最终上层协议的 IANA Protocol / Next Header 数值，不使用 `eLayer` 替代；没有有效 IP 层时为 `kInvalidL4Protocol`。因此合法值 0（IPv6 Hop-by-Hop）不会与 absent 混淆。
- `vlan_depth`：NPI 返回层序列中的 VLAN 层总数，包括外层和隧道内层 VLAN。
- `tunnel_depth`：NPI 返回层序列每跨越一个 GRE、VXLAN、VXLAN_GPE、GENEVE、L2TP 或 GTP 封装边界加 1。

VLAN、PPP、PPPoE 和 MPLS 是封装层，但不计入 `tunnel_depth`。Sprint 22 只用固定 VXLAN 样本验收 `tunnel_depth == 1`；其他封装沿用 NPI 既有层序列语义，不在本 Sprint 单独认证。

`PacketLayerHints` 不重复保存 IP 地址和端口值。`packet_filter.v1` 需要 IP / 端口条件时，必须根据 `l3_offset`、`l4_offset`、`ip_version`、`l4_protocol` 和 `parse_status` 通过 checked accessor 读取 packet header；offset 无效、状态不是可用终态或剩余长度不足时，条件匹配返回“不可用”，不得重新进行未约束解析。未来若需要直接存储地址/端口，只能作为新的具名 typed sidecar 或 minor 扩展。

### 8.5 解析终态

| 状态 | 语义 | 有效输出 |
| --- | --- | --- |
| `kNotParsed` | sidecar 已分配但尚未调用 classifier | 无保证；`layercount` 视为 0 |
| `kComplete` | 在已支持的合法终点完成解析 | 完整已识别层链和派生字段 |
| `kPartial` | 当前头合法，但下一协议未知或暂不支持 | 已提交前缀有效；未知层不写入 |
| `kTruncated` | `cap_len` 不足以读取基础头或声明长度 | 失败前已提交前缀有效 |
| `kMalformed` | 字节存在，但版本、header length 或协议字段不合法 | 失败前已提交前缀有效 |
| `kLayerLimit` | 下一层将超过 15 层或 `protocol::Layers` 的 offset 表示范围 | 已写入的 15 层或可表示前缀有效 |
| `kUnsupportedLinkType` | 外层 LINKTYPE 不支持 | 不读取 packet 字节，层链为空 |

状态判定规则：

- 每个 packet 只有一个终态，以解析过程中首次遇到的终止原因为准。
- 缺少所需字节是 `kTruncated`；字节足够但声明值非法是 `kMalformed`。
- 未知 EtherType、IP protocol 或合法但未注册的 next layer 是 `kPartial`。
- 非首 IP 分片是合法停止：状态为 `kComplete`，保留 IANA `l4_protocol`，`l4_offset` 无效。
- `wire_len > cap_len` 本身不改变 classifier 状态；只有解析某层需要的字节超出 `cap_len` 时才是 `kTruncated`。
- 下游在 `kTruncated`、`kMalformed`、`kLayerLimit` 下可以使用 checked façade 已提交的 offset 做诊断，但不能将 `layers.payload` 解释为完整应用层 payload 起点。

### 8.6 NPI `LayerHints()` 接口

NPI 仍是基础、不可缺少的流量分析插件，调用方继续通过 `IQuerier` 和 `IID_PROTOCOL` 查询 `IProtocol`。`PacketBatchPayload`、`PacketLayerHints` 等结构对象不参与 IID 查询。

`LayerHints()` 通过 checked façade 复用现有 `NetworkLayer` parser map/dispatch，不调用 `Identify()`，不访问 Engine 的规则匹配入口，不触发 Hyperscan scan，也不因为分层而加载 HTTP / TLS / DNS 等应用规则。Hyperscan database/scratch 由显式插件生命周期准备，不是本方法的隐式副作用。

`IProtocol` 增加：

```cpp
virtual int32_t LayerHints(int32_t pipeno,
                           const uint8_t* packet,
                           uint32_t cap_len,
                           uint32_t link_type,
                           PacketLayerHints* hints) noexcept = 0;
```

返回值只表达接口调用结果：

- `0`：`hints` 已产生确定终态；包括 `kComplete`、`kPartial`、`kTruncated`、`kMalformed`、`kLayerLimit` 和 `kUnsupportedLinkType`。
- `-EINVAL`：`hints == nullptr`，或 `cap_len > 0` 且 `packet == nullptr`。
- `-EOVERFLOW`：`cap_len > INT32_MAX`，无法无损传给现有 NPI parser 的 `int32_t size` 契约。
- `-ERANGE`：`pipeno` 不在已配置的并发范围内。
- 其他负值：实现内部错误；只要 `hints != nullptr`，实现必须先将其重置为 canonical `kNotParsed` 状态，不能留下半写结果。

数据异常不能混入接口错误码；调用成功后必须读取 `hints->parse_status`。

旧接口保留：

```cpp
virtual int32_t Layer(int32_t pipeno,
                      const uint8_t* packet,
                      int32_t packet_size,
                      protocol::Layers* layers) = 0;
```

旧 `Layer()` 保留当前 ABI/接口签名、默认 Ethernet 假设、入口路径和返回语义，不新增 `link_type` 或 `PacketParseStatus`。新 `LayerHints()` 复用同一 parser map 和 parser function，不通过调用旧 `Layer()` 后复制局部 64 字节 `protocol::Layers`，也不复制或重写协议 parser。

本 Sprint 只要求仓库已有旧 `Layer()` 回归保持通过，并在 PB 固定合法样本上与 `LayerHints()` 比较层序列。`LayerHints()` 的参数校验和错误码不得反向定义旧 `Layer()` 的异常输入语义。

### 8.7 NPI 初始化顺序与 `pipeno` 语义

NPI `Option()` 的空值和错误语义先于生命周期阶段冻结：`nullptr`、空串或空 JSON object 表示“不加载应用层规则”，返回 0，`LayerHints()` 仍可使用；非空配置必须是 JSON object，`ldfile` 若存在必须是非空字符串且配置文件加载成功。畸形 JSON、错误字段类型或 `ldfile` 加载失败返回非零，由 loader 停在 O 阶段，不进入任何插件的 L/S 阶段。并发度不重复放入 Option，仍只通过 L/S 窗口的 `Concurrency(N)` 配置。

NPI 在 C-PLUGIN-LIFECYCLE 中的阶段责任固定为：

```text
Option(): 解析并持有协议配置，不建模、不创建 Hyperscan 资源
  -> Load(): Engine::Create()/Config::Modeling() 只构建 recognizer graph，不调用 Ready()
  -> Concurrency(N): 在 loader 的 L/S 装配窗口冻结 worker 数
  -> Start(): 校验并发配置，调用 Engine::Ready()，创建 database 和 N 份 scratch
  -> NPI Start() success: 后置 packet consumer 才可启动并调用 LayerHints()/Layer()/Identify()
```

为保持 NPI 改动最小，保留现有 `void Concurrency(int32_t)` 接口签名和默认并发度 1。实现必须增加明确的阶段状态：

- `Concurrency(N)` 只允许在本插件 `Load()` 成功后、首次 `Start()` 前调用一次；`1 <= N <= 16` 时更新 pending 值。
- 非法 N 或 Start 前重复配置会标记启动配置错误并保持原 pending 值；由于接口保留 `void` 签名，错误统一由随后 `Start()` 的非零返回传播，使 `StartAll()` 失败。
- 首次 `Start()` / `Ready()` 成功后并发度永久冻结到 `Unload()`；后续 `Concurrency()` 不改变值，并输出可诊断的契约错误。
- 未显式调用 `Concurrency()` 时使用默认 N=1，保证不需要 NPI 应用识别的既有单线程装配可以启动。
- `Stop()` 不释放 database/scratch；同一 Load epoch 再次 `Start()` 复用已 ready 的只读资源，不重复编译或分配。`Unload()` 才释放并重置状态。

`Engine::Create()` 与 `Engine::Ready()` 必须成为两个真实阶段：`Model::operator()` 完成 recognizer graph 构建后直接返回，不再隐式调用 `Ready()`；`NetworkProtocolIdentify::Start()` 是唯一的 `Ready()` 调用点。`Ready()` 在 compile database 或第 k 份 scratch 分配失败时，必须释放本次 database 和全部 `[0, k)` scratch、恢复 `not-ready`，再由 `Start()` 返回非零；这满足 IPlugin Start 失败原子性，随后 T0 的 `StartAll()` 逆序回滚此前已启动插件，packet worker 不会进入。

`pipeno` 是上游处理线程/pipe index，不是 `source_id` 或 `rx_queue`。NPI 的 Hyperscan scratch 按 `pipeno` 选择实例，因此：

1. `Concurrency(N)` 建立合法范围 `[0, N)`，每个上游 worker 持有稳定且唯一的 `pipeno`。
2. 同一处理 pass 若连续调用 `LayerHints()` 和 `Identify()`，两次调用必须使用当前 worker 的相同 `pipeno`。
3. 不同 `pipeno` 可以并发处理；同一 `pipeno` 不允许被多个线程同时调用 `Identify()`。
4. checked façade 和现有 layer parser 不访问 Hyperscan scratch，解析结果不应随 `pipeno` 改变，但接口仍保留并校验该参数，避免后续应用识别失去线程亲和。
5. `pipeno` 不写入 packet context；它只属于当前执行实例的并发资源选择。同一处理 pass 若紧接着执行 `LayerHints()` 和 `Identify()`，两次调用必须使用当前 worker 的同一 `pipeno`；已发布的 thread-independent hints 被后续 worker 复用时，后续 `Identify()` 使用其当前 worker 的合法 `pipeno`，不继承旧线程编号。

`Concurrency(N)` 必须在 Hyperscan scratch 创建和 worker 启动前完成；当前 Hyperscan scratch 数组上限为 16，因此合法范围固定为 `1 <= N <= 16`。NPI 必须在完整插件列表中排在 packet worker 消费者之前，使其 `Start()/Ready()` 先完成；消费者 `Start()` 只能在依赖已启动后创建 worker。

NPI 内部 checked layer 入口只接收 `(packet, cap_len, link_type)`；`pipeno` 只存在于 `IProtocol::LayerHints()` façade，用于校验当前 worker 和保持与 `Identify()` 的调用亲和。这样后续 pcapfile、DPDK 和 AF_XDP 可以直接调用同一能力，而不依赖文件状态、buffer owner、wall-clock 或 Hyperscan 规则。

### 8.8 文件落点、测试设计与完成出口

- `src/common/network/layers.h`：公共 `protocol::Layers` 和安全 helper。
- `src/common/network/packet_layer_hints.h`：状态、sentinel 和派生字段结构。
- `src/plugins/npi/iprotocol.h`：`LayerHints()` façade 和新 IID。
- `src/plugins/npi/npi.h/.cpp`：NPI 阶段状态、`Start()` / `Stop()` 和 lifecycle guard；T6 只在同文件增加 `LayerHints()` façade 转发。
- `src/plugins/npi/engine.h/.cpp`、`model.cpp`：拆分 recognizer graph 构建与 `Ready()` finalization。
- `src/plugins/npi/regexmatch.h/.cpp`：使 Hyperscan database 和 N 份 scratch 的创建具备失败原子性。

测试必须覆盖 `Layers` ABI/helper、canonical 初始态、状态/absent、最内层 offset、`LayerHints()` 合法/非法 `pipeno`、不触发 `Identify()` / Hyperscan，以及旧 `Layer()` 的接口签名兼容。NPI 启动测试还必须证明：null/空 Option 成功且畸形/无效 `ldfile` 停在 O 阶段；`Load()` 后调用 `Concurrency(2)`，`StartAll()` 成功后 pipeno 0/1 各有可用 scratch；N=0/17 使启动失败且无 worker 进入；通过 test-only 内部故障注入使第 k 份 scratch 分配失败后，本次 database 和 `[0, k)` scratch 全部释放且 Start 返回非零；默认 N=1 可启动；Start 后再次配置不改变合法范围；Stop/Start 不重复创建资源。故障注入不得进入 public ABI。公共结构、façade、IID 迁移和分阶段初始化闭合后，T5 才能完成；checked 实现、终态传播和旧接口回归由 T6/T8 负责。

## 9. T6：NPI `LayerHints()` checked façade、终态传播与映射适配

### 9.1 设计目标与验收 ID

T6 负责 C-NPI-HINTS，关闭 `S19.2-A03`、`S19.2-A05`、`S19.2-A06`、`S19.2-A08`。它在 T5 公共结构和接口上实现 checked façade，复用现有 NPI parser/dispatch，并将必要终态和 Layers 映射为 `PacketLayerHints`。T6 不盘点或认证协议能力，不建设第二套 parser。

### 9.2 公共依赖

T6 消费 T5 的 C-LAYERS-ABI，向 T7、T8、T9 提供可直接写入 sidecar 的 `LayerHints()` 实现。现有 `NetworkLayer`、`parsers_map_`、`Delamination` parser function 和协议分派语义均是可信依赖；T6 只增加接口约束、必要终态表达和输出映射。

### 9.3 单一 parser/dispatch 与兼容边界

新旧入口共享同一 parser map 和 parser function：

```text
IProtocol::LayerHints()
  -> NetworkLayer checked façade
  -> existing parsers_map_ / Delamination parser functions
  -> caller-owned PacketLayerHints::layers
  -> bounded Layers-to-Hints derivation

IProtocol::Layer()
  -> existing NetworkLayer::Layer() compatibility entry
  -> existing parsers_map_ / Delamination parser functions
```

`LayerHints()` 不调用旧 `Layer()` 后再复制局部 64 字节 `protocol::Layers`，而是让 checked 入口直接写 `hints->layers`。允许为复用 parser map 抽取不改变语义的内部遍历 helper，但禁止复制 parser table、分叉协议 dispatch、重写协议 parser 或改变旧 `Layer()` 的签名、默认 Ethernet 假设和返回语义。

### 9.4 Checked façade 与终态传播

调用顺序固定为：

1. 只要 `hints != nullptr`，先将其重置为 T5 定义的 canonical `kNotParsed` 状态。
2. 校验 `packet`、`cap_len`、`pipeno` 和 `link_type`；`cap_len > INT32_MAX` 返回 `-EOVERFLOW`，不支持的 LINKTYPE 在读取 packet 前产生 `kUnsupportedLinkType`。
3. checked 入口在调用现有 `Delamination` parser function 前检查该 parser 已登记的固定最小头长度；不足时产生 `kTruncated`，且不调用 parser。
4. parser 返回后校验 `consumed > 0`、`consumed` 不小于固定最小头长度且不超过剩余 `cap_len`，并使用 checked add 推进 32 位 cursor。声明长度超过捕获长度产生 `kTruncated`，存在足够字节但 header length 或 parser 结果非法产生 `kMalformed`。
5. 写入 `protocol::Layers` 前检查 layer count、`eLayer` 值，以及 layer offset 和最终 payload 两类 `uint16_t` 字段的表示范围；任一待写值超过 `UINT16_MAX`，或下一层超过 `MAX_LAYERS`，均产生 `kLayerLimit`。
6. 正常停止原因由现有 NPI 解析路径传播为 `kComplete` 或 `kPartial`，不能由 façade 根据层数猜测。

现有内部解析结果只允许增加表达 `continue / complete / partial / malformed` 所需的最小终止原因；固定头不足由 checked 调用统一产生 `kTruncated`，layer/offset/payload 的层数或表示上限由中央入口统一产生 `kLayerLimit`。这些字段属于 NPI 内部实现，不进入 public ABI。现有 parser function 只做必要的终止原因赋值；除 PB 固定契约样本暴露的直接阻塞问题外，不在本 Sprint 增加协议分支、重写可选字段逻辑或开展逐协议安全加固。

接口错误码与 packet 终态严格分离：参数或 NPI 内部错误返回负值并保持 canonical `kNotParsed`；packet 数据导致的 `kPartial`、`kTruncated`、`kMalformed`、`kLayerLimit` 和 `kUnsupportedLinkType` 均返回 0，由调用方读取 `parse_status`。

### 9.5 输出 invariant 与派生字段

checked 入口提交结果前必须确认：

- `layers.layercount <= protocol::MAX_LAYERS`。
- `[0, layercount)` 中每个 layer type 合法，offset 单调递增且小于 `cap_len`。
- 32 位最终 cursor 在窄化前满足 `cursor <= cap_len` 和 `cursor <= UINT16_MAX`；发布后的 `layers.payload` 等于该 cursor，且不小于最后一个已提交 layer 的 offset。
- 未使用 layer slot 不参与比较、派生或发布。
- `kUnsupportedLinkType` 的层链为空；其余异常终态只暴露已提交的有效前缀。

若现有 parser 返回的 `consumed` 触发截断或畸形规则，当前失败层不提交；若已提交结果在最终 invariant 检查中出现不可能的内部矛盾，则 façade 返回内部错误并重置为 canonical `kNotParsed`，不得发布半写 sidecar。

L2/L3/L4、IP version、IANA L4 protocol、VLAN depth 和 tunnel depth 通过一次最多 `MAX_LAYERS`（15）的有界后处理从 `hints->layers` 和 checked packet view 派生。该后处理不是协议解析，不递归、不分配内存、不调用 `Identify()`；它以最小 NPI 改动换取清晰的映射边界，性能由 T9 的 15% 门槛约束。

公共 `protocol::Layers` helper 同时完成安全收口：`Top()` 对空层返回 `eLayer::NONE`；`Get/Forward/Backward/Top<Header>` 在 `packet == nullptr`、offset 越界或 `offset + sizeof(Header) > packet_size` 时返回空指针；`Payload/Data` 对 payload 超出 packet size 返回空/0。`operator(layer1, layer2)` 必须比较两个参数，`Levels::degree[7]` 写入必须饱和。packet context 直接遍历 `[0, layercount)`，不依赖最多容纳 7 个匹配项的 `Levels` union。

### 9.6 文件落点、测试设计与完成出口

- `src/plugins/npi/layer.h/.cpp`：checked 入口、现有 parser map 复用、必要终态传播、结果 invariant 和 Layers-to-Hints 映射。
- `src/plugins/npi/npi.h/.cpp`：`IProtocol::LayerHints()` façade 接入；旧 `Layer()` 入口保持。
- `src/tests/test_npi_layer/`：façade 参数/错误码、PB 固定契约 corpus、输出 invariant、固定 VXLAN 和旧接口对照。

T6 测试只覆盖 Ethernet + IPv4 + TCP、VLAN + IPv4 + UDP、IPv6 + TCP、固定 VXLAN、代表性截断/畸形、layer offset 与最终 payload 的 65535/65536 边界、unknown LINKTYPE 和 layer limit。PB 固定合法样本上旧 `Layer()` 与 `LayerHints()` 的层序列一致，终态和输出 invariant 确定，仓库已有旧接口回归保持通过后，T6 才能完成。其他协议沿用 NPI 可信能力，不新增能力表、逐协议 corpus、逐隧道 corpus 或 parser 重构验收。

## 10. T7：批级 sidecar 预分配、原位写入、发布同步和 hints checked accessor

### 10.1 设计目标与验收 ID

T7 负责 C-PUBLISH，关闭 `S19.2-A09`、`S19.2-A11`，并完成 `S19.1-A06` 的 sidecar 集成。它把 T6 checked façade 的结果写入 T3 context，不承担协议 parser 或最终性能/Sanitizer 验收。

### 10.2 公共依赖

T7 消费 T3 的 packet context、T5 的 hints ABI 和 T6 的 checked façade，向 T8、T9 提供 immutable payload、checked accessor 和可测量热路径。

### 10.3 原位构建

`PacketBatchPayload` 构造阶段一次性预分配连续的 `PacketLayerHints[]`。worker 直接调用：

```cpp
const int rc = protocol->LayerHints(pipeno,
                                    descriptors[i].data,
                                    descriptors[i].cap_len,
                                    descriptors[i].link_type,
                                    &layer_hints[i]);
if (rc != 0) {
    abort_block_build();
}
```

约束：

- 每个 worker 只写不重叠的 slot 区间；同一 slot 不允许并发写。
- batch builder 必须检查每次 `LayerHints()` 返回值：只有 `0` 才允许发布该 slot；任何非零返回（包括 `-EINVAL`、`-EOVERFLOW`、`-ERANGE` 或内部错误）都会中止当前 block 构造并释放其资源，不得发布 `kNotParsed` 或未初始化 slot。
- 当输入 packet 本身合法但解析失败时，接口返回 `0`，slot 发布为确定的 `kPartial` / `kTruncated` / `kMalformed` / `kLayerLimit` / `kUnsupportedLinkType`，由下游按状态处理。
- payload 发布前允许填充，发布后 sidecar 只读。
- checked façade 只初始化语义字段和实际使用的 layer slot，不对全部 15 个 slot 做逐包 blanket clear。
- 未使用 slot 不得被读取、比较或序列化。
- NPI parser dispatch table 初始化后只读；热路径不分配内存、不加锁、不抛异常。
- 每包复杂度为 `O(min(layer_count, MAX_LAYERS))`。

### 10.4 缓存权衡

混合 AoS + sidecar 不是对所有访问模式都更快：

- 只处理 descriptor 的 operator 每包读取约 48 字节，不会被 `protocol::Layers` 的 64 字节及派生字段拖入 cache。
- 批量 classifier 顺序写连续 hints，批量 NPM/filter 顺序读连续 hints，适合硬件预取。
- 处理单个 packet 且同时访问 descriptor 和 hints 时，需要触碰两个内存区域，确实可能比大 AoS 多一次 cache line 获取，命中率可能下降。

本设计接受该权衡，因为不是所有下游都需要 layer context，而且把约 80 字节 hints 内联进每个 descriptor 会显著增加所有扫描的工作集。缓解措施是：

1. descriptor 数组和 hints 数组都连续分配并按相同 index 访问。
2. worker 以连续 packet range 为单位处理，不随机跨 batch 跳转。
3. checked façade 直接原位写 `hints[i]`，不复制 64 字节 `Layers`。
4. 派生字段只做一次最多 15 层的顺序后处理，不重复解析 packet；该额外读取由性能基准约束。
5. 性能基准同时测 descriptor-only、descriptor+hints 顺序扫描和单 packet 随机访问，不能只用一种访问模式证明布局优劣。

最终是否需要进一步调整布局，以 Sprint 22 基准数据为依据，不在未测量前将 hints 合并回 descriptor。

### 10.5 并发和发布模型

`PacketBatchPayload` 采用 build-then-publish：

1. 单线程或多个 worker 构造 descriptor 和不重叠 sidecar 区间。
2. 所有 worker 完成后 join 或 barrier；发布线程通过 release store 发布 immutable payload，读取线程通过 acquire load 获取，形成明确的 happens-before。
3. 发布后的 payload、view、registry 和 sidecar 全部只读。
4. 一个有效 lease 可以在明确的执行器所有权下移动到另一线程，但同一 lease 不得并发消费。`LeaseState` 的 release path 必须线程安全：若 backend（例如 AF_XDP / 部分 DPDK ring）要求 owner lcore 回收，跨线程 `ReleaseBlock()` / destructor 只能将 token 投递到 owner queue，由 owner drain；不得直接在错误线程调用 backend callback。
5. 多个下游若需并发读取同一 block，必须由上层 fan-out 建立明确的共享读取和合并 release 计数；Sprint 22 的基础 lease 本身不提供隐式复制。

NPI layer parser 的分发表只读，可以被不同 `pipeno` 并发调用。checked façade 只写调用方提供的独立 hint slot；Hyperscan 应用识别仍遵守一个 worker 对应一个 `pipeno` / scratch 的独占规则。

### 10.6 错误处理与降级

| 场景 | 行为 |
| --- | --- |
| payload kind 未知 | operator 返回不支持错误，不做 unchecked cast |
| schema name/major 不匹配 | `OnSchemaReady()` 拒绝，不能尝试消费首块 |
| sidecar 缺失 | 不需要 hints 的 operator 正常运行；依赖 hints 的 operator 明确拒绝。Sprint 22 不允许在 NPM/filter 内部临时补齐或重复解析 |
| 单 packet `kPartial` | 可使用已提交层和派生字段，不能假定未知 next layer |
| 单 packet `kTruncated` / `kMalformed` | 保留已提交前缀，下游按策略过滤或计数，不重新解析 |
| `kLayerLimit` | 保留可表示前缀并计数，不能静默当作完整解析 |
| 不支持的 LINKTYPE | `kUnsupportedLinkType`，不读取 packet 数据 |
| operator 返回错误 | 执行器先释放 lease，再传播错误 |
| `Cancel()` 时有 outstanding lease | 停止新生产并立即返回；各 lease 独立归还，不能提前回收 buffer |

NPM 和 `packet_filter.v1` 可以复用 hints 快速定位，但仍必须校验对应 status、offset sentinel 和 `cap_len`。Hints 是经过 checked façade 收口的定位信息，不是绕过边界检查的授权。

### 10.7 文件落点、测试设计与完成出口

- `src/framework/core/packet_batch.*`、`packet_context.*`：sidecar storage、publish barrier 和 accessor integration。
- `src/plugins/npi/`：提供已冻结的 `LayerHints()` façade，复用现有 parser/dispatch。
- `src/tests/test_packet_data_plane/`：build-then-publish、range partition、checked accessor 和 absent sidecar 用例。

每个 slot 的 classifier 返回值都必须检查，发布前不能存在 `kNotParsed` 或未初始化状态；发布后 descriptor/sidecar/registry 只读。性能基准覆盖 descriptor-only、descriptor+hints 顺序和随机访问三种模式后，T7 才能完成。

## 11. T8：构造型 packet/block、完整单元测试、CTest 注册和回归路径

### 11.1 设计目标与验收 ID

T8 负责 C-TEST 的生产路径回归，关闭 `S19.1-A08`、`S19.1-A09`、`S19.2-A07`、`S19.2-A08`、`S19.2-A10` 的测试责任。T8 不重新定义 T0-T7 的接口或生命周期，只提供构造型数据、测试装配、CTest 汇总和 Scheduler 拒绝门禁验证。

### 11.2 公共依赖

T8 消费 T0-T7 的稳定契约，并把实际 target、CTest 名称和证据格式交给 T9/T10 复用。测试必须走生产插件加载/调度路径；test-only builder 不进入 production ABI。

### 11.3 构造型测试数据边界

构造型 packet builder 只存在于测试目标，不进入生产头文件、导出符号或 plugin ABI。builder 必须显式构造每个字段，禁止依赖宿主字节序或未初始化 padding。

固定隧道验收样本选用：

```text
Ethernet
  / IPv4
  / UDP
  / VXLAN
  / Ethernet
  / IPv4
  / TCP
  / payload
```

该样本必须断言：

- 完整 layer type 序列和每层 offset。
- `layers.payload` 和 top layer。
- L2 / L3 / L4 指向内层 Ethernet / IPv4 / TCP。
- `ip_version == 4`、`l4_protocol == 6`。
- `vlan_depth == 0`、`tunnel_depth == 1`。
- `parse_status == kComplete`。

### 11.4 Story 19.1 测试矩阵

| 类别 | 必测内容 |
| --- | --- |
| 插件生命周期 | 至少 3 个插件的完整 R/O/L/C/S 顺序、跨插件查询、L/S 装配窗口、Option/Load/Start 失败阻断、逆序回滚、无悬挂 IID/handle、启动后增量 Load 拒绝 |
| Payload | Kind、schema name/major/minor、checked cast、未知 Kind 拒绝 |
| Schema | `OutputSchema()` 在首个 block 前可读、`packet.v1` 全字段、Arrow metadata、major 拒绝、producer minor 低于 required minor 拒绝、未知高 minor 可忽略 |
| Identity | 同实体跨多个 block 的 packet ID 重复拒绝；跨实体允许重复；多 source 同实体可解析；allocator 分区和重启 high-water mark 规则可复现 |
| Descriptor | 空 data、长度关系、未知 source、unknown rx queue、空 batch、data 超出已 pin buffer range |
| Sidecar | absent、完整对齐、`kNotParsed`、长度不一致拒绝、发布后只读 |
| Lease | move-only、moved-from、自移动 no-op、显式释放、析构兜底、重复释放、跨 channel 释放、move assignment 覆盖 Active lease、channel 先析构、Cancel 后 outstanding lease、reclaimer exactly-once、noexcept recycler、跨线程归还投递到 owner queue |
| Channel | data、timeout、EOF、cancel、error；outstanding lease 不提前回收 |
| Operator | Packet / Arrow 分派；继续、主动停止、错误和 C++ 异常路径均释放；schema 只复制不保存引用 |
| 回归 | `IStreamChannel` / Arrow 既有测试无回退；Scheduler 门禁仍生效；Kind 正确但动态类型不匹配时 checked accessor 拒绝 |

### 11.5 Story 19.2 测试矩阵

| 类别 | 必测内容 |
| --- | --- |
| Façade 契约 | null 参数组合、`cap_len > INT32_MAX`、canonical 初始化、接口错误码/packet 终态分离、layercount/offset/payload 输出 invariant |
| PB 基础链 | Ethernet+IPv4+TCP、VLAN+IPv4+UDP、Ethernet+IPv6+TCP |
| 固定隧道样本 | VXLAN 完整链、最内层派生 offset、`tunnel_depth == 1` |
| 终态传播 | PB 固定路径的代表性固定头截断、声明长度超过 `cap_len`、IPv4 IHL < 5、TCP offset < 5，分别得到约定的 `kTruncated` / `kMalformed` |
| 层限制 | 超过 15 层；layer offset 和最终 payload 分别覆盖 65535（合法）/65536（`kLayerLimit`）；派生 offset 覆盖 `UINT32_MAX` absent 边界 |
| LINKTYPE | Ethernet 成功；未知、Linux cooked、Null loopback 不读数据并返回 unsupported |
| NPI 启动 | null/空 Option 与畸形/无效 ldfile；`Load()` 不调用 `Ready()`；L/S 窗口设置 N=2 后 `Start()` 创建 2 份 scratch；默认 N=1；N=0/17 启动失败；第 k 份 scratch 故障注入后 database/已有 scratch 原子回滚；Start 后并发范围不可变；Stop/Start 不重复创建资源 |
| `pipeno` | 启动成功后的合法范围、越界拒绝、同一处理 pass 的 LayerHints/Identify 使用相同 index |
| 一致性 | 旧 `Layer()` 与 `LayerHints()` 复用同一 parser/dispatch，并在 PB 固定合法样本上产生相同层序列 |
| 无应用识别 | `LayerHints()` 不调用 `Identify()`，不触发 Hyperscan scan |
| checked accessor | `packet_filter.v1` 通过 offset/status checked accessor 读取 IP/端口；sidecar absent 或状态不可用时返回不可用且不重复解析 |
| 并发 | 不同 `pipeno` 并发无串扰；range partition 不重叠；合法并行写在 TSan 下无 data race，禁止用实际同 slot 冲突制造未定义行为 |
| 旧接口兼容 | 旧 `Layer()` 的 ABI/签名、入口和返回语义保持，仓库已有可执行回归通过 |
| Sanitizer | `test_packet_data_plane` 和 `test_npi_layer` 在 ASan / UBSan 下运行 PB 固定契约 corpus，验证 façade 与现有 NPI 的集成边界 |

### 11.6 测试目标和命令

新增可自动运行的测试目标，避免继续依赖手工打印程序：

- `test_packet_data_plane`：payload、view、schema、identity 和 lease 生命周期。
- `test_npi_layer`：NPI 分阶段启动、checked façade、终态传播、hints 映射、固定隧道和 `pipeno`。
- `test_framework`：插件批次生命周期、框架枚举/接口回归并注册 CTest。
- `test_stream`：现有 Arrow stream 回归。
- `test_scheduler_e2e`：验证 `block_stream` 在真实 block source 接入前仍返回 `BLOCK_STREAM_NOT_IMPLEMENTED`。
- `benchmark_packet_data_plane`：记录 descriptor-only、descriptor+hints、随机单 packet 和 `Layer()` / `LayerHints()` 对照数据。

计划验证命令：

```bash
cmake -B build src
cmake --build build -j$(nproc) --target \
  test_packet_data_plane test_npi_layer test_framework test_stream test_scheduler_e2e
ctest --test-dir build \
  -R "test_packet_data_plane|test_npi_layer|test_framework|test_stream|test_scheduler_e2e" \
  --output-on-failure
```

façade 契约测试完成后，使用项目支持的 sanitizer 构建配置额外执行 PB 固定 `test_npi_layer` corpus。若当前构建系统尚无统一 sanitizer 开关，Sprint 任务必须增加最小、可重复的 ASan / UBSan 验证入口。

### 11.7 文件落点、测试设计与完成出口

- `src/tests/test_packet_data_plane/`：payload/context/lifecycle 与 benchmark target。
- `src/tests/test_npi_layer/`：NPI 分阶段启动、checked façade、终态传播、PB 固定契约 corpus、固定 VXLAN 和旧接口对照。
- `src/tests/test_framework/`、`src/tests/test_stream/`：loader 批次生命周期、既有回归并补 CTest 注册。
- `src/tests/test_scheduler_e2e/`：`BLOCK_STREAM_NOT_IMPLEMENTED` 生产路径门禁。

所有新增测试必须进入 CTest；Scheduler 仍拒绝真实 block stream 生产，且 loader 完整批次顺序/失败回滚、NPI `Concurrency -> Start/Ready`、façade/invariant、PB 基础链、固定 VXLAN、代表性终态传播、未知 LINKTYPE、layer limit、旧接口回归和合法 pipeno 并发均有可执行证据后，T8 才能完成。不新增 NPI 能力表或其他协议认证 corpus。

## 12. T9：ASan/UBSan/TSan、ABI static_assert、性能基准和 15% 回归门槛

### 12.1 设计目标与验收 ID

T9 负责 C-NONFUNCTIONAL，关闭 `S19.1-A09`、`S19.2-A09`、`S19.2-A10` 的跨 Task 验证责任。T9 不替代 T0-T8 的实现和本地测试，只定义独立构建、性能对照和失败门槛。

### 12.2 公共依赖

T9 消费 T0-T8 的生命周期、ABI、测试目标和构造型 corpus。NPI 性能与 Sanitizer 门禁只使用 PB 固定契约 corpus，不扩展为协议能力认证。ASan/UBSan 与 TSan 必须使用独立 build directory；benchmark 必须使用固定 release 构建和固定 batch size。ASan/UBSan 必须运行 `test_framework` 的 loader 阶段失败/handle 回滚用例；所有 NPI 验证必须先按 T0/T5 顺序完成批次启动，不能绕过 `StartAll()` 直接调用业务接口。

### 12.3 性能验收

功能测试之外，新增微基准记录以下结果。固定 release 构建、固定构造型 corpus、batch size 64/512/4096、预热后运行 10 次取中位数：

1. 仅扫描 `PacketDescriptor[]`。
2. 顺序扫描 descriptor + hints。
3. 随机访问单 packet descriptor + hints。
4. `Layer()` 直接写局部 `protocol::Layers`。
5. `LayerHints()` 原位写预分配 sidecar。

验收关注：

- `LayerHints()` 每包零 heap allocation、零锁。
- 不出现局部 `protocol::Layers` 到 sidecar 的 64 字节复制。
- 不对未使用的 15 层槽位做无条件清零。
- 派生字段只触发一次最多 15 层的顺序后处理，不重复解析 packet。
- 记录混合 AoS + sidecar 在 3 种访问模式下的 cache / throughput 差异，为后续优化提供证据。
- 在相同 PB 固定合法契约 corpus 下，`LayerHints()` 的 cycles/packet 相对现有 `Layer()` 不得回退超过 15%；该对照量化 checked façade、必要终态和派生字段的增量成本。超过门槛即判定 T9 失败。

基准目标名固定为 `benchmark_packet_data_plane`，由 `src/tests/test_packet_data_plane/benchmark_packet_data_plane.cpp` 提供。T9 必须使回归超过 15% 或零分配/零锁约束失败时返回非零状态，并保存 batch size 64/512/4096 的中位数结果。

### 12.4 ABI static_assert 与通过门槛

ABI static_assert、sanitizer、TSan、benchmark 和 15% cycles/packet 回归规则与 T0/T1/T5 的 public layout 一致。至少冻结并编译检查以下约束：

- `sizeof(protocol::Layers) == 64`，且公共 `eLayer` 枚举值未改变。
- `PacketDescriptor` 是 standard-layout、trivially-copyable，并满足 T3 冻结的 alignment、`sizeof` 和关键字段 offset。
- `PacketLayerHints` 是 trivially-copyable，并满足 T5 冻结的 layout、`sizeof` 和关键字段 offset。
- 虚函数签名变化对应新 IID；旧 IID 的加载测试必须失败。
- NPI N=2 的两个 `pipeno` 仅在 `Start()/Ready()` 成功后并发执行，TSan 期间不得调用 `Concurrency()` 或重新创建 scratch。

任何 static_assert 编译失败或旧 IID 仍被宿主接受都阻止 T9 完成。

### 12.5 文件落点、验证命令与完成出口

文件落点：

- `src/tests/test_packet_data_plane/benchmark_packet_data_plane.cpp`：固定 corpus、3 种访问模式、`Layer()` / `LayerHints()` 对照、allocation/lock 计数和非零失败退出。
- `src/tests/test_packet_data_plane/CMakeLists.txt`：注册 `benchmark_packet_data_plane`，但不把耗时基准混入默认单元测试。
- `src/common/network/layers.h`、`packet_layer_hints.h`、`src/framework/core/packet_batch.h`：放置紧邻 public type 的 ABI static_assert，或由专用 ABI 测试统一引用检查。
- `src/tests/test_framework/`：在 ASan/UBSan 下执行 loader 阶段失败、IID 截断、handle 关闭和重新加载用例。
- `src/tests/test_packet_data_plane/`、`src/tests/test_npi_layer/`：复用 T8 的构造型 corpus 执行 ASan/UBSan 和 TSan，不另建一套测试数据。

Release 性能验证固定执行：

```bash
cmake -B build-release src -DCMAKE_BUILD_TYPE=Release
cmake --build build-release -j$(nproc) --target benchmark_packet_data_plane
./build-release/output/benchmark_packet_data_plane --batch-sizes 64,512,4096 --runs 10
```

Sanitizer 验证必须使用独立构建目录，不能把 ASan/UBSan 与 TSan 混在同一个构建中：

```bash
cmake -B build-asan-ubsan src -DCMAKE_BUILD_TYPE=RelWithDebInfo \
  -DCMAKE_CXX_FLAGS="-fsanitize=address,undefined -fno-omit-frame-pointer" \
  -DCMAKE_EXE_LINKER_FLAGS="-fsanitize=address,undefined" \
  -DCMAKE_SHARED_LINKER_FLAGS="-fsanitize=address,undefined"
cmake --build build-asan-ubsan -j$(nproc) --target test_framework test_packet_data_plane test_npi_layer
ctest --test-dir build-asan-ubsan \
  -R "test_framework|test_packet_data_plane|test_npi_layer" --output-on-failure

cmake -B build-tsan src -DCMAKE_BUILD_TYPE=RelWithDebInfo \
  -DCMAKE_CXX_FLAGS="-fsanitize=thread -fno-omit-frame-pointer" \
  -DCMAKE_EXE_LINKER_FLAGS="-fsanitize=thread" \
  -DCMAKE_SHARED_LINKER_FLAGS="-fsanitize=thread"
cmake --build build-tsan -j$(nproc) --target test_packet_data_plane test_npi_layer
ctest --test-dir build-tsan \
  -R "test_packet_data_plane|test_npi_layer" --output-on-failure
```

T9 只能在 T0-T8 的功能测试和 CTest target 已存在后运行。任一命令非零退出、任一 ASan/UBSan/TSan 报告、cycles/packet 回退超过 15%、每包出现 heap allocation 或锁竞争都判定失败；不得以仅记录数据代替门禁。ABI assertion、三类独立构建和 batch size 64/512/4096 的中位数证据均按 T10 格式记录后，T9 才能完成。

## 13. T10：验收证据、文档同步和 Sprint 收口

### 13.1 设计目标与责任边界

T10 负责把已定义的实现和验证结果整理为可复核证据，关闭文档同步门禁。它不替代任何 T0-T9 的实现或测试责任；如果 T10 需要新增证据格式、自动检查规则或验收门槛，必须先在本章冻结。

### 13.2 对应验收 ID 与公共依赖

T10 没有独立验收 ID，只汇总 `S19.1-A01..A09` 和 `S19.2-A01..A11` 的既有证据。它消费 T8 的 C-TEST、T9 的 C-NONFUNCTIONAL、T0-T9 各 Task 的实现/测试落点以及 planning 的 Task 状态，不得修改已冻结契约或用文档勾选替代失败验证。

### 13.3 验收证据格式

每个 Story 的验收记录至少包含：

```text
acceptance_id: S19.x-Axx
implementation: <file:symbol>
test: <CTest name / case name>
command: <reproducible command>
result: PASS / FAIL
evidence: <summary or artifact path>
```

### 13.4 Story 验收 ID

验收条款是 planning、design、代码、测试和 Review 的稳定追踪键。每个 Task 章节在 3-12 章中声明其负责的完整 ID；T10 只汇总证据，不得用“全部”替代实现 Task 的显式覆盖。

| ID | Story | 可验证契约 | 设计章节 |
| --- | --- | --- | --- |
| S19.1-A01 | 19.1 | 通用 `IBlockPayload`、`BlockPayloadKind`、schema 判型和 no-IID 规则 | T1 §4.3-4.5 |
| S19.1-A02 | 19.1 | `IBlockStreamChannel` / `IBlockStreamOperator` 泛化、`OutputSchema()` 和 Arrow wrapper | T1 §4.6-4.8 |
| S19.1-A03 | 19.1 | `packet.v1` 字段、版本、编码和兼容策略 | T4 §7.3-7.4 |
| S19.1-A04 | 19.1 | `logical_entity_id`、固定 registry、`source_id` 和实体级 packet allocator | T3 §6.3-6.5 |
| S19.1-A05 | 19.1 | `PacketDescriptor` AoS、buffer range、view non-owning 和 payload owning | T3 §6.6 |
| S19.1-A06 | 19.1 | 强类型批级 sidecar、PImpl、present/absent 和发布后只读 | T3 §6.6.2、T7 §10.3-10.7 |
| S19.1-A07 | 19.1 | `BlockLease` move-only、exactly-once release、Cancel/Close、owner queue | T2 §5.3-5.4 |
| S19.1-A08 | 19.1 | 构造型 block source / operator 闭环，Scheduler 生产门禁保持 | T2 §5.1、T8 §11.3-11.7 |
| S19.1-A09 | 19.1 | 插件批次生命周期、CTest、异常/并发/ABI 回归和 sanitizer 入口 | T0 §3.3-3.7、T8 §11.4-11.7、T9 §12.3-12.5 |
| S19.2-A01 | 19.2 | 直接复用 `eLayer` / `protocol::Layers`，结构体布局、checked helper 和输出 invariant 收口 | T5 §8.3、T6 §9.5-9.6 |
| S19.2-A02 | 19.2 | 单一 `PacketParseStatus` 终态和字段 present/absent 语义 | T5 §8.3-8.5、T7 §10.3 |
| S19.2-A03 | 19.2 | 最内层 L2/L3/L4、IANA L4、IP version、VLAN/tunnel depth 映射 | T5 §8.4、T6 §9.5-9.6 |
| S19.2-A04 | 19.2 | `LayerHints(pipeno, packet, cap_len, link_type, hints)` 与 NPI `IID_PROTOCOL` 边界 | T5 §8.6-8.7 |
| S19.2-A05 | 19.2 | façade 参数校验、canonical 初始化、错误码/packet 终态分离和输出 invariant | T6 §9.4-9.6 |
| S19.2-A06 | 19.2 | 现有 NPI parser/dispatch 可信复用、不做协议语义改造，PB 固定合法样本与旧 `Layer()` 一致 | T6 §9.3、T6 §9.6 |
| S19.2-A07 | 19.2 | 构造型固定 VXLAN 隧道样本，最内层 offset 和 `tunnel_depth=1` | T8 §11.3-11.7 |
| S19.2-A08 | 19.2 | 必要 NPI 终态传播、unknown LINKTYPE 和层数限制；旧 `Layer()` ABI/签名、入口及仓库已有回归保持 | T6 §9.4-9.6、T8 §11.5-11.7 |
| S19.2-A09 | 19.2 | 原位预分配 sidecar、零分配/零锁、AoS+sidecar cache 基准和回归预算 | T7 §10.3-10.7、T9 §12.3-12.5 |
| S19.2-A10 | 19.2 | 批次启动、`Concurrency(N) -> Start/Ready`、pipeno 亲和和不同 pipeno 并发无串扰 | T0 §3.3-3.7、T5 §8.6-8.8、T8 §11.5-11.7、T9 §12.3-12.5 |
| S19.2-A11 | 19.2 | hints 供 NPM / `packet_filter.v1` checked accessor 复用，不重复解析 | T5 §8.4、T7 §10.3-10.7 |

### 13.5 Backlog 验收映射

| Backlog 验收项 | 设计 Task | 主要验证 |
| --- | --- | --- |
| 通用 `BlockPollEvent` payload | T1、T2 | Packet / Arrow payload 测试 |
| Arrow 不再是唯一 packet 表示 | T1、T3 | Packet 热路径无 Arrow 构造 |
| `packet.v1` 字段与版本 | T4 | schema / Arrow metadata 测试 |
| `PacketBatchView` 与 buffer 引用 | T2、T3 | 构造、访问和 lease 生命周期 |
| schema / context 扩展策略 | T3、T4、T7 | major/minor 和 sidecar present/absent |
| NPI adapter 可被实时来源复用 | T0、T5、T7 | 完整批次启动、worker 数冻结和输入契约测试 |
| 现有 NPI 作为可信分层依赖 | T6 | 复用同一 parser map/dispatch，旧 `Layer()` 回归保持 |
| `protocol::Layers` 映射 | T5、T6 | layer sequence / offset 断言 |
| 最内层 L2/L3/L4 与 IP/L4 字段 | T5、T6 | 基础链和 VXLAN 样本 |
| VLAN / tunnel depth | T5、T6、T8 | 单层 VLAN 和固定 VXLAN 样本 |
| packet context 复用 | T3、T7 | sidecar 对齐和只读发布 |
| checked façade 与来源解耦 | T5、T6 | 构造型 packet 和 LINKTYPE 测试 |
| 不调用应用层识别 | T5、T8 | Hyperscan 不被调用 |
| 必要终态与 Layers 输出收口 | T6、T8、T9 | 固定契约、output invariant 和 sanitizer 测试 |
| DPDK / AF_XDP 可复用 | T4、T5、T6 | 输入仅 packet/cap_len/link_type |
| 至少一种隧道样本 | T8 | VXLAN 完整链断言 |
| `packet_filter.v1` IP/端口结构输入 | T5、T7 | checked accessor 状态和边界测试 |

### 13.6 设计门禁与文档同步

`planning.md` 已于 2026-07-25 按本设计完成同步：

1. Story 19.2 按 PB 完整 Story 规划，信任并复用现有 NPI parser/dispatch，只建设 checked `LayerHints()` façade、必要终态传播、PacketLayerHints 映射、PB 固定契约样本和固定 VXLAN 隧道样本。
2. T0-T10 已按 planning 的 ID、名称和顺序建立设计章节，并绑定验收 ID、主要文件和测试目标。
3. 插件完整批次生命周期、NPI 分阶段初始化、Scheduler 范围、ABI 迁移、CTest、Sanitizer、TSan 和性能门槛均已纳入计划。
4. “可运行骨架”仅指构造型进程内闭环，Scheduler 生产装配明确延期。
5. 当前估算为 17.9 PD，评估区间为 17-21 PD。

后续任何设计变化必须在同一次变更中同步更新：

- 本文验收 ID 与设计章节映射。
- `planning.md` 的 Task、测试目标、主要文件、工作量和依赖。
- 受影响的 Backlog 验收项；禁止只调整一份 Sprint 文档。

实现完成的 Definition of Done：

- Story 19.1 和完整 Story 19.2 的验收映射全部闭合。
- 插件批次严格满足 R/O/L/C/S 顺序，失败的 `Start()` 自行回到 stopped；NPI scratch 在业务线程启动前按冻结的 N 原子创建，失败路径无悬挂 IID、database、scratch 或已启动 worker。
- 新增测试进入 CTest，验证命令实际执行对应二进制。
- checked façade 与现有 NPI 在 PB 固定契约 corpus 下通过 ASan / UBSan，且终态和输出 invariant 符合约定。
- public ABI、接口 IID、框架文档、Sprint 计划和产品待办同步更新。
- 性能基准记录 3 种 sidecar 访问模式及原位 `LayerHints()` 路径。

### 13.7 文件落点与完成出口

- `planning.md` / `design.md`：Task、设计、验收和状态同步。
- `tasks/product_backlog.md`：仅在 Story 验收完成后更新状态。
- `review.md`：记录每个 acceptance ID 的实现、命令、结果和证据。

T10 只有在 T0-T9 全部完成、20 个验收 ID 均有实现与可执行证据、文档无范围漂移且 Backlog 状态同步后才能完成。
