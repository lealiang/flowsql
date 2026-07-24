# Sprint 22 Packet 数据面设计

本文定义 Sprint 22 的实现契约，范围为完整 Story 19.1 和完整 Story 19.2。设计状态为已确认、待实现；后续代码、测试和 Sprint 计划必须与本文保持一致。

## 1. 设计范围与关键决策

### 1.1 设计目标

Sprint 22 建立统一的 packet 块式数据面，使 `pcapfile`、DPDK、AF_XDP 等后续数据源能够输出相同的运行时数据结构，并使 NPI、NPM 和 `packet_filter.v1` 复用同一份分层上下文。

本 Sprint 交付以下能力：

1. 将 `IBlockStreamChannel` 和 `IBlockStreamOperator` 从 Arrow 专用接口改为通用 block payload 接口。
2. 定义 `packet.v1` 逻辑 schema、`PacketDescriptor`、`PacketBatchView` 和 owning `PacketBatchPayload`。
3. 定义 `logical_entity_id + source_id + packet_id` 身份模型。
4. 定义批级强类型 packet context sidecar，并交付完整 `PacketLayerHints`。
5. 复用公共 `eLayer` 和 `protocol::Layers`，提炼 NPI 的安全分层解析核心。
6. 冻结 `PollBlock()`、`BlockLease`、`ReleaseBlock()` 及 packet buffer 的生命周期。
7. 使用构造型 packet 和构造型 block source 完成协议、安全、并发及生命周期验收。

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

Story 19.2 作为完整 Story 一次性交付。`tunnel_depth`、安全解析、当前 NPI 分层协议盘点和至少一种固定隧道样本均在本 Sprint 内交付。

### 1.3 当前基线与改造边界

当前代码基线如下：

- `IBlockStreamChannel` 的 data event 和 `ReleaseBlock()` 只接受 `arrow::RecordBatch`。
- `IBlockStreamOperator::OnSchemaReady()` 和 `ProcessBlock()` 同样只接受 Arrow 类型。
- 两个 block stream 接口来自 Sprint 13 的占位设计，仓库内没有生产实现或调用方。
- Scheduler 会明确拒绝 `ChannelType::kBlockStream`。
- `eLayer` 位于公共网络定义中；`protocol::Layers` 固定保存最多 15 层，当前大小为 64 字节。
- NPI `NetworkLayer::Layer()` 已注册 Ethernet、VLAN、PPPoE、PPP、MPLS、IPv4 / IPv6、IPv6 扩展头、TCP、UDP、SCTP、GRE、VXLAN / VXLAN_GPE、GENEVE、L2TP 和 GTP parser。
- 现有主循环硬编码从 Ethernet 开始，没有 `link_type`、解析终态和完整的变长头边界校验。
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
| 分层上下文 | `PacketLayerHints` | 保存安全分层结果和派生定位字段 | payload 拥有连续 sidecar |

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
| `src/plugins/npi/iprotocol.h` | 继续定义并导出 `IID_PROTOCOL` / `IProtocol`，引用公共 Layers / Hints 类型 |
| `src/plugins/npi/layer.h/.cpp` | 实现无应用层依赖的安全分层核心 |
| `src/tests/test_packet_data_plane/benchmark_packet_data_plane.cpp` | 实现 AoS + sidecar 访问模式和 `Layer()` / `LayerHints()` 性能基准 |

`protocol::Layers` 只移动声明位置，不修改字段顺序、字段类型和 `MAX_LAYERS`。实现时使用 `static_assert(sizeof(protocol::Layers) == 64)` 防止无意改变 ABI。

### 2.3 公共设计归属

只有跨 Task 使用或冻结跨 Task 契约的内容进入本章。owner Task 负责冻结和实现契约，consumer Task 只能通过已冻结接口接入，不能各自复制或改写同一语义。

| 公共设计 ID | 契约 | Owner Task | Consumer Task |
| --- | --- | --- | --- |
| C-BLOCK-ABI | 通用 payload、schema、Channel/Operator ABI | T1 | T2、T3、T4、T8、T9 |
| C-LEASE | move-only lease、exactly-once reclaimer、owner queue | T2 | T1、T3、T8、T9 |
| C-PACKET-IDENTITY | logical entity、source registry、entity-level packet allocator | T3 | T4、T7、T8 |
| C-PACKET-SCHEMA | `packet.v1` 字段、版本和 Arrow metadata | T4 | T1、T3、T8 |
| C-LAYERS-ABI | 公共 `eLayer` / `protocol::Layers` / `PacketLayerHints` | T5 | T6、T7、T8、T9 |
| C-PARSER | 安全 parser step、父协议边界和支持矩阵 | T6 | T5、T7、T8、T9 |
| C-PUBLISH | sidecar 原位构建、build-then-publish、checked accessor | T7 | T3、T8、T9 |
| C-TEST | 构造型样本、CTest、Scheduler 回归 | T8 | T0-T7、T9、T10 |
| C-NONFUNCTIONAL | ABI static_assert、Sanitizer、TSan、性能预算 | T9 | T0-T8、T10 |

文档顺序不表示执行依赖，实际依赖以 `planning.md` Task 表为准。共享文件以 owner Task 为主修改方；consumer Task 若需要改变公共契约，必须先更新 owner Task 设计和 planning 依赖。

## 3. T0：设计/ABI 基线、公共头归属和实现分支准备

### 3.1 设计目标与验收责任

T0 冻结 Sprint 22 的当前代码基线、公共类型归属、ABI 迁移规则和测试入口基线，为 T1-T9 提供共同开工条件。T0 不单独关闭 Story 验收，但直接支撑 `S19.1-A09`、`S19.2-A01` 和所有涉及 public ABI / CTest 的实现 Task。

### 3.2 公共依赖

T0 维护第 2 章公共设计归属表，重点冻结 C-BLOCK-ABI、C-LAYERS-ABI、C-TEST 和 C-NONFUNCTIONAL 的初始边界。后续 Task 可以实现这些契约，但不能在不更新本文的情况下改变类型 owner、IID 迁移或测试目标名称。

### 3.3 ABI 与兼容基线

1. `protocol::Layers` 布局和大小保持不变，只迁移到公共头文件。
2. `eLayer` 直接复用，现有枚举数值不变。
3. `PacketDescriptor` 必须保持 standard-layout、trivially-copyable、固定 alignment、固定 `sizeof` 和关键字段 offset；`PacketLayerHints` 同样必须保持 trivially-copyable，sidecar 扩展只能新增独立 typed storage/accessor。
4. `IBlockStreamChannel`、`IBlockStreamOperator` 和 `IProtocol` 的虚函数表发生变化，必须生成并替换新的 IID 值、整体重编插件，并在宿主侧拒绝旧 IID；旧 IID 不得复用或通过兼容分支继续接受旧插件。descriptor / hint 布局变化也必须触发同样的 ABI 迁移评审。
5. `IBlockPayload` 不定义 IID；payload 判型使用 `BlockPayloadKind + schema`。
6. `packet.v1` major 1 的字段语义冻结；新增可选字段只提升 minor。
7. `PacketLayerHints` 是进程内公共结构，不作为跨版本持久化二进制格式。
8. Arrow `RecordBatch` 通过 wrapper 保留，现有 `IStreamChannel` 行为不变。
9. Scheduler 的 `BLOCK_STREAM_NOT_IMPLEMENTED` 错误在真实 block source 接入前保持不变。

### 3.4 文件落点

- `tasks/sprints/sprint22-packet-data-plane/planning.md` / `design.md`：冻结 Task、公共设计和验收关系。
- `src/CMakeLists.txt`：确认新增测试目录和 CTest 总入口。
- 第 2.2 节列出的公共头：由 T1、T3、T5 分别实现，T0 只冻结归属和 ABI 迁移要求。

### 3.5 测试设计与通过门槛

T0 的配置检查必须在实现前记录并在 T10 复核：

1. 当前 `sizeof(protocol::Layers) == 64`，`eLayer` 数值基线可由 compile-time assertion 固化。
2. 当前 `test_npi` 是手工程序、`test_framework` 未注册 CTest；T8 必须将替代目标接入 CTest。
3. 当前 Scheduler 对 `kBlockStream` 的拒绝路径存在；T8 必须增加生产路径回归测试，而不是删除门禁。
4. 所有发生虚函数签名变化的 IID 都必须进入 ABI 差异清单，旧 IID 加载测试必须失败。

### 3.6 完成出口

- 公共设计 owner/consumer、文件归属和 IID 迁移清单已冻结。
- T1-T9 的 Task 名称、依赖和设计章节与 planning 一致。
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

## 8. T5：公共 Layers 提取、checked helper、`PacketLayerHints` 结构和 NPI façade/pipeno

### 8.1 设计目标与验收 ID

T5 负责 C-LAYERS-ABI，关闭 `S19.2-A01`、`S19.2-A02`、`S19.2-A04`，并承担 `S19.2-A10` 的 `pipeno` 契约。它定义公共结构和 façade，不在本 Task 中完成全部协议 parser。

### 8.2 公共依赖

T5 消费 T0 的 ABI 基线，向 T6、T7、T8、T9 提供公共 Layers/Hints 和 NPI 接口。`IProtocol` 继续通过 IID 注册查询，`PacketLayerHints` 是结构对象，不新增 IID。

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
- 只在完整验证某层头部后写入该层；失败层不进入数组。
- 有效 layer offset 单调递增且小于等于 `cap_len`；派生 offset 使用 32 位表示，`UINT32_MAX` 只表示 absent。
- `layers.payload` 表示已验证前缀后的安全 offset，不能超过 `cap_len`。
- `top layer` 由 `layers.layers[layercount - 1].layer` 派生；`layercount == 0` 时为 `eLayer::NONE`。
- 调用方不得在 `layercount == 0` 时直接调用现有不带保护的 `Layers::Top()`。
- 未使用的 `layers.layers[]` 槽位内容未定义，任何调用方和序列化逻辑都只能读取 `[0, layercount)`。

### 8.4 派生字段

派生字段默认指向最内层有效协议：

- `l2_offset`：从已提交 layer 序列尾部反向查找最内层有效 L2 层（`ETHERNET`、`VLAN`、`PPPoE_Session`、`PPP`、`MPLS`）的 offset。QinQ / MPLS stack 取最内层已验证 tag/label；隧道内层优先于外层。
- `l3_offset`：最内层 IPv4 或 IPv6 基础头起点，不指向 IPv6 扩展头。
- `l4_offset`：最内层 TCP、UDP 或 SCTP 头起点；未知协议或非首分片时为 `kInvalidLayerOffset`。
- `ip_version`：与 `l3_offset` 对应，只允许 0、4、6；没有有效 IP 层时为 0。
- `l4_protocol`：记录最内层 IP 的扩展链解析完成后、最终上层协议的 IANA Protocol / Next Header 数值，不使用 `eLayer` 替代；没有有效 IP 层时为 `kInvalidL4Protocol`。因此合法值 0（IPv6 Hop-by-Hop）不会与 absent 混淆。
- `vlan_depth`：完整已验证层序列中的 VLAN 层总数，包括外层和隧道内层 VLAN。
- `tunnel_depth`：每跨越一个已验证 GRE、VXLAN、VXLAN_GPE、GENEVE、L2TP 或 GTP 封装边界加 1。

VLAN、PPP、PPPoE 和 MPLS 是封装层，但不计入 `tunnel_depth`。IPv4-in-IPv6 和 IPv6-in-IPv4 属于本 Sprint 的 IP-in-IP 支持范围，每次 IP-in-IP 封装边界计为 1。

`PacketLayerHints` 不重复保存 IP 地址和端口值。`packet_filter.v1` 需要 IP / 端口条件时，必须根据 `l3_offset`、`l4_offset`、`ip_version`、`l4_protocol` 和 `parse_status` 通过 checked accessor 读取 packet header；offset 无效、状态不是可用终态或剩余长度不足时，条件匹配返回“不可用”，不得重新进行未约束解析。未来若需要直接存储地址/端口，只能作为新的具名 typed sidecar 或 minor 扩展。

### 8.5 解析终态

| 状态 | 语义 | 有效输出 |
| --- | --- | --- |
| `kNotParsed` | sidecar 已分配但尚未调用 classifier | 无保证；`layercount` 视为 0 |
| `kComplete` | 在已支持的合法终点完成解析 | 完整已识别层链和派生字段 |
| `kPartial` | 当前头合法，但下一协议未知或暂不支持 | 已验证前缀有效；未知层不写入 |
| `kTruncated` | `cap_len` 不足以读取基础头或声明长度 | 失败前已验证前缀有效 |
| `kMalformed` | 字节存在，但版本、header length 或协议字段不合法 | 失败前已验证前缀有效 |
| `kLayerLimit` | 下一层将超过 15 层或 `protocol::Layers` 的 offset 表示范围 | 已写入的 15 层或可表示前缀有效 |
| `kUnsupportedLinkType` | 外层 LINKTYPE 不支持 | 不读取 packet 字节，层链为空 |

状态判定规则：

- 每个 packet 只有一个终态，以解析过程中首次遇到的终止原因为准。
- 缺少所需字节是 `kTruncated`；字节足够但声明值非法是 `kMalformed`。
- 未知 EtherType、IP protocol 或合法但未注册的 next layer 是 `kPartial`。
- 非首 IP 分片是合法停止：状态为 `kComplete`，保留 IANA `l4_protocol`，`l4_offset` 无效。
- `wire_len > cap_len` 本身不改变 classifier 状态；只有解析某层需要的字节超出 `cap_len` 时才是 `kTruncated`。
- 下游在 `kTruncated`、`kMalformed`、`kLayerLimit` 下可以使用已验证 offset 做诊断，但不能将 `layers.payload` 解释为完整应用层 payload 起点。

### 8.6 NPI `LayerHints()` 接口

NPI 仍是基础、不可缺少的流量分析插件，调用方继续通过 `IQuerier` 和 `IID_PROTOCOL` 查询 `IProtocol`。`PacketBatchPayload`、`PacketLayerHints` 等结构对象不参与 IID 查询。

`LayerHints()` 只调用无状态安全 layer core，不调用 `Identify()`，不访问 Engine 的规则匹配入口，不触发 Hyperscan scan，也不因为分层而加载 HTTP / TLS / DNS 等应用规则。NPI 插件的常规 `Load()` 是否为其他调用方准备规则属于插件生命周期，不属于本方法的隐式副作用。

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

旧 `Layer()` 默认使用 `LINKTYPE_ETHERNET`，直接让安全解析核心写入调用方提供的局部 `protocol::Layers`。对 `kComplete` / `kPartial` 返回层数，对其他解析终态返回负数，并保留已验证前缀；新 packet 数据面必须使用 `LayerHints()` 获得完整状态。

旧 `Layer()` 的接口错误与新入口一致：`layers == nullptr`、`packet_size < 0`、`packet_size > 0 && packet == nullptr` 返回 `-EINVAL`；`pipeno` 越界返回 `-ERANGE`；`packet_size == 0` 且 `packet == nullptr` 允许作为空 packet，返回 0 并输出空层。发生负返回时，调用方不得继续 `Identify()`。

### 8.7 `pipeno` 语义

`pipeno` 是上游处理线程/pipe index，不是 `source_id` 或 `rx_queue`。NPI 的 Hyperscan scratch 按 `pipeno` 选择实例，因此：

1. `Concurrency(N)` 建立合法范围 `[0, N)`，每个上游 worker 持有稳定且唯一的 `pipeno`。
2. 同一处理 pass 若连续调用 `LayerHints()` 和 `Identify()`，两次调用必须使用当前 worker 的相同 `pipeno`。
3. 不同 `pipeno` 可以并发处理；同一 `pipeno` 不允许被多个线程同时调用 `Identify()`。
4. 安全分层核心本身不访问 Hyperscan scratch，解析结果不应随 `pipeno` 改变，但接口仍保留并校验该参数，避免后续应用识别失去线程亲和。
5. `pipeno` 不写入 packet context；它只属于当前执行实例的并发资源选择。同一处理 pass 若紧接着执行 `LayerHints()` 和 `Identify()`，两次调用必须使用当前 worker 的同一 `pipeno`；已发布的 thread-independent hints 被后续 worker 复用时，后续 `Identify()` 使用其当前 worker 的合法 `pipeno`，不继承旧线程编号。

`Concurrency(N)` 必须在 Hyperscan scratch 创建和 worker 启动前完成；当前 Hyperscan scratch 数组上限为 16，因此合法范围固定为 `1 <= N <= 16`。非法值必须在配置/`Ready()` 阶段使插件启动失败并保持旧值，不能写入固定数组；worker 启动后并发范围只读，禁止与 `LayerHints()` / `Identify()` 并发修改。

分层复用的核心函数只接收 `(packet, cap_len, link_type)`；`pipeno` 只存在于 NPI `IProtocol::LayerHints()` façade，用于校验当前 worker 和保持与 `Identify()` 的调用亲和。这样后续 pcapfile、DPDK 和 AF_XDP 可以直接调用同一 classifier，而不依赖文件状态、buffer owner、wall-clock 或 Hyperscan 规则。

### 8.8 文件落点、测试设计与完成出口

- `src/common/network/layers.h`：公共 `protocol::Layers` 和安全 helper。
- `src/common/network/packet_layer_hints.h`：状态、sentinel 和派生字段结构。
- `src/plugins/npi/iprotocol.h`：`LayerHints()` façade 和新 IID。
- `src/plugins/npi/layer.h/.cpp`：façade 到安全 core 的适配。

测试必须覆盖 `Layers` ABI/helper、canonical 初始态、状态/absent、最内层 offset、合法/非法 `pipeno`、不触发 `Identify()` / Hyperscan，以及旧 `Layer()` 的接口错误。公共结构、façade 和 IID 迁移闭合后，T5 才能完成。

## 9. T6：安全 parser core 和完整协议/隧道支持矩阵

### 9.1 设计目标与验收 ID

T6 负责 C-PARSER，关闭 `S19.2-A03`、`S19.2-A05`、`S19.2-A06`、`S19.2-A08`。它在 T5 公共结构上实现单一安全解析核心和完整 Story 19.2 协议矩阵。

### 9.2 公共依赖

T6 消费 T5 的 C-LAYERS-ABI，并向 T7、T8、T9 提供安全、无应用层识别的 classifier。所有 parser 共享同一个 step 契约和父协议边界模型。

### 9.3 单一核心、两个写入目标

不在 `LayerHints()` 外包装旧的不安全 `Layer()`，也不维护两套 parser。提取一个内部安全解析核心，并支持两个直接写入目标：

```text
IProtocol::Layer()
  -> safe layer core
  -> caller-owned protocol::Layers

IProtocol::LayerHints()
  -> safe layer core
  -> caller-owned PacketLayerHints::layers
  -> derived fields in the same parse loop
```

两个入口都直接写调用方内存，不创建局部 64 字节 `protocol::Layers` 再复制。`LayerHints()` 在同一解析循环中更新 L2/L3/L4、IP version、IANA L4 protocol、VLAN depth 和 tunnel depth，不进行第二次层数组扫描。

### 9.4 Parser step 契约

每个 parser step 返回结构化结果：

```cpp
enum class LayerStepStatus : uint8_t {
    kContinue,
    kComplete,
    kPartial,
    kTruncated,
    kMalformed,
};

struct LayerStepResult {
    LayerStepStatus status;
    uint32_t consumed;
    uint32_t next_limit;
    eLayer next;
    uint16_t upper_protocol;
    bool tunnel_boundary;
};
```

单步执行顺序固定为：

1. 检查固定最小头长度。
2. 在长度满足后读取固定字段。
3. 验证版本、header length、option length 和协议约束。
4. 使用 checked add 计算下一 offset，并将父协议声明的长度转换为 `next_limit = min(parent_limit, declared_end, cap_len)`。
5. 确认结果不超过 `next_limit` 且可由 `protocol::Layers` 表示。
6. 只有成功验证当前 layer 后才提交 layer 和派生字段；失败 step 不提交失败层。
7. `kContinue` 必须满足 `consumed > 0` 且 `next != eLayer::NONE`；若不能推进，返回 `kMalformed`，防止死循环。
8. tunnel depth 只在 tunnel header 成功提交后增加。

任何一步失败都不能提交未完整验证的 layer。

内层 parser 的每次读取同时受 `cap_len` 和父协议声明的 `next_limit` 约束，不能把 Ethernet padding 或容器尾部字节当作内层 packet。IPv4 total length、IPv6 payload length、UDP length、PPPoE length、GTP length 等字段都必须收紧该边界。

### 9.5 必须补齐的边界校验

安全核心至少覆盖：

- Ethernet / VLAN / QinQ 固定头和 EtherType。
- PPPoE payload length、PPP protocol。
- MPLS label stack 和 Bottom-of-Stack。
- IPv4 version、IHL、total length、fragment offset 和 IP-in-IP next protocol。
- IPv6 payload length 和 extension header 链。
- TCP data offset。
- UDP length。
- SCTP 基础头及需要遍历时的 chunk length。
- GRE flags 和可选字段长度。
- 标准 VXLAN 只校验 flags 并固定进入 inner Ethernet；VXLAN_GPE 独立校验 GPE next-protocol，再按其协议值分派，不读取标准 VXLAN 不存在的字段。
- GENEVE version 和 option length。
- L2TP flags、length、sequence 和 offset 可选字段。
- GTP version、length、sequence / N-PDU / extension 字段。

中央循环使用至少 32 位 offset，所有 `offset + length` 都使用 checked add。写入 `protocol::Layers` 的 `uint16_t` offset 前检查范围，超出表示能力时返回 `kLayerLimit`。

公共 `protocol::Layers` helper 也必须经过安全收口：`Top()` 对空层返回 `eLayer::NONE`；`Get/Forward/Backward/Top<Header>` 在 `packet == nullptr`、offset 越界或 `offset + sizeof(Header) > packet_size` 时返回空指针；`Payload/Data` 对 payload 超出 packet size 返回空/0。`operator(layer1, layer2)` 修复为同时比较两个参数，`Levels` 的 `degree[7]` 写入必须饱和而不能越界。需要完整 layer 列表的 packet context 代码直接遍历 `[0, layercount)`，不依赖只能容纳 7 个匹配项的 Levels union。

### 9.6 支持矩阵

| 协议族 | Sprint 22 行为 |
| --- | --- |
| Ethernet、VLAN / QinQ | 完整安全解析 |
| PPPoE Session、PPP、MPLS stack | 完整安全解析 |
| IPv4、IPv6 | 完整安全解析 |
| IPv6 Hop-by-Hop、Routing、Fragment、ESP、AH、Destination Options | 完整边界校验；ESP 作为合法终点 |
| TCP、UDP、SCTP | 完整安全解析到 transport payload |
| GRE | 解析可选字段并进入支持的 inner layer |
| VXLAN | 解析 tunnel header，固定进入 inner Ethernet |
| VXLAN_GPE、GENEVE | 分别解析 GPE / option 字段并进入声明的 next protocol |
| L2TP、GTP | 解析可选字段并进入支持的 inner layer |
| IPv4-in-IPv6、IPv6-in-IPv4 | 解析互嵌 IP，计入 tunnel depth 并继续寻找最内层 L3/L4 |
| 其他合法 next protocol | `kPartial`，保留已验证前缀 |
| 非 Ethernet LINKTYPE | Sprint 22 返回 `kUnsupportedLinkType` |

首个支持的外层链路类型是标准 `LINKTYPE_ETHERNET` (1)。classifier 必须先判断 `link_type`，不支持时不得读取 packet 数据。后续增加 LINKTYPE 只扩展入口分派，不改变 `packet.v1` 或 `PacketLayerHints` major 版本。

### 9.7 文件落点、测试设计与完成出口

- `src/plugins/npi/layer.h/.cpp`：单一 parser core、step dispatcher 和协议实现。
- 必要的 NPI parser headers：仅承载协议头解析，不引入应用层识别依赖。
- `src/tests/test_npi_layer/`：protocol matrix、边界、旧接口和固定 corpus。

每种支持协议必须至少有正常、截断和畸形边界样本；隧道、IP-in-IP、offset 65535/65536、未知 LINKTYPE 和 layer limit 必须有确定终态。合法 Ethernet 输入上旧 `Layer()` 与新 core 的层序列一致，所有非法输入不越界后，T6 才能完成。

## 10. T7：批级 sidecar 预分配、原位写入、发布同步和 hints checked accessor

### 10.1 设计目标与验收 ID

T7 负责 C-PUBLISH，关闭 `S19.2-A09`、`S19.2-A11`，并完成 `S19.1-A06` 的 sidecar 集成。它把 T6 classifier 写入 T3 context，不承担独立协议 parser 或最终性能/Sanitizer 验收。

### 10.2 公共依赖

T7 消费 T3 的 packet context、T5 的 hints ABI 和 T6 的 classifier，向 T8、T9 提供 immutable payload、checked accessor 和可测量热路径。

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
- batch builder 必须检查每次 `LayerHints()` 返回值：`0` 才允许发布该 slot；`-EINVAL`、`-ERANGE` 或内部错误会中止当前 block 构造并释放其资源，不得发布 `kNotParsed` 或未初始化 slot。
- 当输入 packet 本身合法但解析失败时，接口返回 `0`，slot 发布为确定的 `kPartial` / `kTruncated` / `kMalformed` / `kLayerLimit` / `kUnsupportedLinkType`，由下游按状态处理。
- payload 发布前允许填充，发布后 sidecar 只读。
- parser 只初始化语义字段和实际使用的 layer slot，不对全部 15 个 slot 做逐包 blanket clear。
- 未使用 slot 不得被读取、比较或序列化。
- parser dispatch table 初始化后只读；热路径不分配内存、不加锁、不抛异常。
- 每包复杂度为 `O(min(layer_count, MAX_LAYERS))`。

### 10.4 缓存权衡

混合 AoS + sidecar 不是对所有访问模式都更快：

- 只处理 descriptor 的 operator 每包读取约 48 字节，不会被 `protocol::Layers` 的 64 字节及派生字段拖入 cache。
- 批量 classifier 顺序写连续 hints，批量 NPM/filter 顺序读连续 hints，适合硬件预取。
- 处理单个 packet 且同时访问 descriptor 和 hints 时，需要触碰两个内存区域，确实可能比大 AoS 多一次 cache line 获取，命中率可能下降。

本设计接受该权衡，因为不是所有下游都需要 layer context，而且把约 80 字节 hints 内联进每个 descriptor 会显著增加所有扫描的工作集。缓解措施是：

1. descriptor 数组和 hints 数组都连续分配并按相同 index 访问。
2. worker 以连续 packet range 为单位处理，不随机跨 batch 跳转。
3. classifier 直接原位写 `hints[i]`，不复制 64 字节 `Layers`。
4. 派生字段在解析循环内同步计算，不二次扫描层数组。
5. 性能基准同时测 descriptor-only、descriptor+hints 顺序扫描和单 packet 随机访问，不能只用一种访问模式证明布局优劣。

最终是否需要进一步调整布局，以 Sprint 22 基准数据为依据，不在未测量前将 hints 合并回 descriptor。

### 10.5 并发和发布模型

`PacketBatchPayload` 采用 build-then-publish：

1. 单线程或多个 worker 构造 descriptor 和不重叠 sidecar 区间。
2. 所有 worker 完成后 join 或 barrier；发布线程通过 release store 发布 immutable payload，读取线程通过 acquire load 获取，形成明确的 happens-before。
3. 发布后的 payload、view、registry 和 sidecar 全部只读。
4. 一个有效 lease 可以在明确的执行器所有权下移动到另一线程，但同一 lease 不得并发消费。`LeaseState` 的 release path 必须线程安全：若 backend（例如 AF_XDP / 部分 DPDK ring）要求 owner lcore 回收，跨线程 `ReleaseBlock()` / destructor 只能将 token 投递到 owner queue，由 owner drain；不得直接在错误线程调用 backend callback。
5. 多个下游若需并发读取同一 block，必须由上层 fan-out 建立明确的共享读取和合并 release 计数；Sprint 22 的基础 lease 本身不提供隐式复制。

NPI layer parser 的分发表只读，可以被不同 `pipeno` 并发调用。Hyperscan 应用识别仍遵守一个 worker 对应一个 `pipeno` / scratch 的独占规则。

### 10.6 错误处理与降级

| 场景 | 行为 |
| --- | --- |
| payload kind 未知 | operator 返回不支持错误，不做 unchecked cast |
| schema name/major 不匹配 | `OnSchemaReady()` 拒绝，不能尝试消费首块 |
| sidecar 缺失 | 不需要 hints 的 operator 正常运行；依赖 hints 的 operator 明确拒绝。Sprint 22 不允许在 NPM/filter 内部临时补齐或重复解析 |
| 单 packet `kPartial` | 可使用已验证层和派生字段，不能假定未知 next layer |
| 单 packet `kTruncated` / `kMalformed` | 保留诊断前缀，下游按策略过滤或计数，不重新做不安全解析 |
| `kLayerLimit` | 保留可表示前缀并计数，不能静默当作完整解析 |
| 不支持的 LINKTYPE | `kUnsupportedLinkType`，不读取 packet 数据 |
| operator 返回错误 | 执行器先释放 lease，再传播错误 |
| `Cancel()` 时有 outstanding lease | 停止新生产并立即返回；各 lease 独立归还，不能提前回收 buffer |

NPM 和 `packet_filter.v1` 可以复用 hints 快速定位，但仍必须校验对应 status、offset sentinel 和 `cap_len`。Hints 是经过验证的定位信息，不是绕过边界检查的授权。

### 10.7 文件落点、测试设计与完成出口

- `src/framework/core/packet_batch.*`、`packet_context.*`：sidecar storage、publish barrier 和 accessor integration。
- `src/plugins/npi/`：调用已冻结的 `LayerHints()` façade，不复制 parser。
- `src/tests/test_packet_data_plane/`：build-then-publish、range partition、checked accessor 和 absent sidecar 用例。

每个 slot 的 classifier 返回值都必须检查，发布前不能存在 `kNotParsed` 或未初始化状态；发布后 descriptor/sidecar/registry 只读。性能基准覆盖 descriptor-only、descriptor+hints 顺序和随机访问三种模式后，T7 才能完成。

## 11. T8：构造型 packet/block、完整单元测试、CTest 注册和回归路径

### 11.1 设计目标与验收 ID

T8 负责 C-TEST 的生产路径回归，关闭 `S19.1-A08`、`S19.1-A09`、`S19.2-A07`、`S19.2-A08`、`S19.2-A10` 的测试责任。T8 不重新定义 T1-T7 的接口，只提供构造型数据、测试装配、CTest 注册和 Scheduler 拒绝门禁验证。

### 11.2 公共依赖

T8 消费 T1-T7 的稳定契约，并把实际 target、CTest 名称和证据格式交给 T9/T10 复用。测试必须走生产插件加载/调度路径；test-only builder 不进入 production ABI。

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
| 基础链 | Ethernet+IPv4+TCP、Ethernet+IPv4+UDP、VLAN/QinQ+IPv4+UDP、Ethernet+IPv6+TCP |
| 封装层 | PPPoE+PPP、MPLS stack |
| IP 边界 | IPv4 IHL/total length/fragment、IPv6 extension 链和非首分片 |
| Transport | TCP data offset、UDP length、SCTP 基础头 |
| 隧道 | GRE、VXLAN、VXLAN_GPE、GENEVE、L2TP、GTP、IPv4-in-IPv6、IPv6-in-IPv4 parser 边界 |
| 固定隧道样本 | VXLAN 完整链、最内层派生 offset、`tunnel_depth == 1` |
| 截断 | 各固定头 `0..N-1`、所有变长头声明长度超过 `cap_len` |
| 畸形 | 非法 IP version、IPv4 IHL < 5、TCP offset < 5、option length 溢出 |
| 层限制 | 超过 15 层、`protocol::Layers` offset 65535（合法）/65536（`kLayerLimit`），以及派生 offset `UINT32_MAX` absent 边界 |
| LINKTYPE | Ethernet 成功；未知、Linux cooked、Null loopback 不读数据并返回 unsupported |
| `pipeno` | 合法范围、越界拒绝、同一处理 pass 的 LayerHints/Identify 使用相同 index |
| 一致性 | 旧 `Layer()` 与新安全核心在合法 Ethernet 输入上产生相同层序列 |
| 无应用识别 | `LayerHints()` 不调用 `Identify()`，不触发 Hyperscan scan |
| checked accessor | `packet_filter.v1` 通过 offset/status checked accessor 读取 IP/端口；sidecar absent 或状态不可用时返回不可用且不重复解析 |
| 并发 | 不同 `pipeno` 并发无串扰；range partition 不重叠；合法并行写在 TSan 下无 data race，禁止用实际同 slot 冲突制造未定义行为 |
| 旧接口异常 | 旧 `Layer()` 对空指针、负长度、pipeno 越界、截断、畸形、layer-limit 返回约定错误；调用方不得继续 `Identify()` |
| Sanitizer | `test_packet_data_plane` 和 `test_npi_layer` 均在 ASan / UBSan 下运行截断、畸形和构造型 corpus |

### 11.6 测试目标和命令

新增可自动运行的测试目标，避免继续依赖手工打印程序：

- `test_packet_data_plane`：payload、view、schema、identity 和 lease 生命周期。
- `test_npi_layer`：安全 layer parser、hints、隧道和 `pipeno`。
- `test_framework`：补充框架枚举/接口回归并注册 CTest。
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

安全解析完成后，使用项目支持的 sanitizer 构建配置额外执行 `test_npi_layer`。若当前构建系统尚无统一 sanitizer 开关，Sprint 任务必须增加最小、可重复的 ASan / UBSan 验证入口。

### 11.7 文件落点、测试设计与完成出口

- `src/tests/test_packet_data_plane/`：payload/context/lifecycle 与 benchmark target。
- `src/tests/test_npi_layer/`：安全 parser、hints、协议矩阵和 boundary corpus。
- `src/tests/test_framework/`、`src/tests/test_stream/`：既有回归并补 CTest 注册。
- `src/tests/test_scheduler_e2e/`：`BLOCK_STREAM_NOT_IMPLEMENTED` 生产路径门禁。

所有新增测试必须进入 CTest；Scheduler 仍拒绝真实 block stream 生产，且固定 VXLAN 样本、截断/畸形、未知 LINKTYPE、layer limit 和合法 pipeno 并发均有可执行用例后，T8 才能完成。

## 12. T9：ASan/UBSan/TSan、ABI static_assert、性能基准和 15% 回归门槛

### 12.1 设计目标与验收 ID

T9 负责 C-NONFUNCTIONAL，关闭 `S19.1-A09`、`S19.2-A09`、`S19.2-A10` 的跨 Task 验证责任。T9 不替代 T1-T8 的实现和本地测试，只定义独立构建、性能对照和失败门槛。

### 12.2 公共依赖

T9 消费 T0-T8 的 ABI、测试目标和构造型 corpus。ASan/UBSan 与 TSan 必须使用独立 build directory；benchmark 必须使用固定 release 构建和固定 batch size。

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
- 派生字段不触发第二次 layer 数组扫描。
- 记录混合 AoS + sidecar 在 3 种访问模式下的 cache / throughput 差异，为后续优化提供证据。
- 在相同安全解析核心和相同 corpus 下，`LayerHints()` 的 cycles/packet 相对安全 `Layer()` 不得回退超过 15%；若超过，必须记录具体 cache / 派生字段成本并在 Sprint Review 明确豁免，不得无证据验收。

基准目标名固定为 `benchmark_packet_data_plane`，由 `src/tests/test_packet_data_plane/benchmark_packet_data_plane.cpp` 提供。T9 必须使回归超过 15% 或零分配/零锁约束失败时返回非零状态，并保存 batch size 64/512/4096 的中位数结果。

### 12.4 ABI static_assert 与通过门槛

ABI static_assert、sanitizer、TSan、benchmark 和 15% cycles/packet 回归规则与 T0/T1/T5 的 public layout 一致。至少冻结并编译检查以下约束：

- `sizeof(protocol::Layers) == 64`，且公共 `eLayer` 枚举值未改变。
- `PacketDescriptor` 是 standard-layout、trivially-copyable，并满足 T3 冻结的 alignment、`sizeof` 和关键字段 offset。
- `PacketLayerHints` 是 trivially-copyable，并满足 T5 冻结的 layout、`sizeof` 和关键字段 offset。
- 虚函数签名变化对应新 IID；旧 IID 的加载测试必须失败。

任何 static_assert 编译失败或旧 IID 仍被宿主接受都阻止 T9 完成。

### 12.5 文件落点、验证命令与完成出口

文件落点：

- `src/tests/test_packet_data_plane/benchmark_packet_data_plane.cpp`：固定 corpus、3 种访问模式、`Layer()` / `LayerHints()` 对照、allocation/lock 计数和非零失败退出。
- `src/tests/test_packet_data_plane/CMakeLists.txt`：注册 `benchmark_packet_data_plane`，但不把耗时基准混入默认单元测试。
- `src/common/network/layers.h`、`packet_layer_hints.h`、`src/framework/core/packet_batch.h`：放置紧邻 public type 的 ABI static_assert，或由专用 ABI 测试统一引用检查。
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
cmake --build build-asan-ubsan -j$(nproc) --target test_packet_data_plane test_npi_layer
ctest --test-dir build-asan-ubsan \
  -R "test_packet_data_plane|test_npi_layer" --output-on-failure

cmake -B build-tsan src -DCMAKE_BUILD_TYPE=RelWithDebInfo \
  -DCMAKE_CXX_FLAGS="-fsanitize=thread -fno-omit-frame-pointer" \
  -DCMAKE_EXE_LINKER_FLAGS="-fsanitize=thread" \
  -DCMAKE_SHARED_LINKER_FLAGS="-fsanitize=thread"
cmake --build build-tsan -j$(nproc) --target test_packet_data_plane test_npi_layer
ctest --test-dir build-tsan \
  -R "test_packet_data_plane|test_npi_layer" --output-on-failure
```

T9 只能在 T1-T8 的功能测试和 CTest target 已存在后运行。任一命令非零退出、任一 ASan/UBSan/TSan 报告、cycles/packet 回退超过 15%、每包出现 heap allocation 或锁竞争都判定失败；不得以仅记录数据代替门禁。ABI assertion、三类独立构建和 batch size 64/512/4096 的中位数证据均按 T10 格式记录后，T9 才能完成。

## 13. T10：验收证据、文档同步和 Sprint 收口

### 13.1 设计目标与责任边界

T10 负责把已定义的实现和验证结果整理为可复核证据，关闭文档同步门禁。它不替代任何 T1-T9 的实现或测试责任；如果 T10 需要新增证据格式、自动检查规则或验收门槛，必须先在本章冻结。

### 13.2 对应验收 ID 与公共依赖

T10 没有独立验收 ID，只汇总 `S19.1-A01..A09` 和 `S19.2-A01..A11` 的既有证据。它消费 T8 的 C-TEST、T9 的 C-NONFUNCTIONAL、T1-T9 各 Task 的实现/测试落点以及 planning 的 Task 状态，不得修改已冻结契约或用文档勾选替代失败验证。

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
| S19.1-A09 | 19.1 | CTest、异常/并发/ABI/生命周期回归和 sanitizer 入口 | T0 §3.3-3.5、T8 §11.4-11.7、T9 §12.3-12.5 |
| S19.2-A01 | 19.2 | 直接复用 `eLayer` / `protocol::Layers`，结构体布局和 helper 安全收口 | T5 §8.3、T6 §9.5 |
| S19.2-A02 | 19.2 | 单一 `PacketParseStatus` 终态和字段 present/absent 语义 | T5 §8.3-8.5、T7 §10.3 |
| S19.2-A03 | 19.2 | 最内层 L2/L3/L4、IANA L4、IP version、VLAN/tunnel depth | T5 §8.4、T6 §9.5-9.6 |
| S19.2-A04 | 19.2 | `LayerHints(pipeno, packet, cap_len, link_type, hints)` 与 NPI `IID_PROTOCOL` 边界 | T5 §8.6-8.7 |
| S19.2-A05 | 19.2 | 安全 parser step、父协议 `next_limit`、checked add 和全支持矩阵 | T6 §9.4-9.5 |
| S19.2-A06 | 19.2 | Ethernet、VLAN/QinQ、IPv4/IPv6、扩展头、TCP/UDP/SCTP、GRE/VXLAN/GPE/GENEVE/L2TP/GTP、IP-in-IP | T6 §9.5-9.6 |
| S19.2-A07 | 19.2 | 构造型固定 VXLAN 隧道样本，最内层 offset 和 `tunnel_depth=1` | T8 §11.3-11.7 |
| S19.2-A08 | 19.2 | 截断、畸形、unknown LINKTYPE、层数限制、旧 Layer 错误语义 | T6 §9.7、T8 §11.5-11.7 |
| S19.2-A09 | 19.2 | 原位预分配 sidecar、零分配/零锁、AoS+sidecar cache 基准和回归预算 | T7 §10.3-10.7、T9 §12.3-12.5 |
| S19.2-A10 | 19.2 | 不调用 `Identify()` / Hyperscan，合法 pipeno 并发无串扰 | T5 §8.6-8.8、T8 §11.5、T9 §12.3-12.5 |
| S19.2-A11 | 19.2 | hints 供 NPM / `packet_filter.v1` checked accessor 复用，不重复解析 | T5 §8.4、T7 §10.3-10.7 |

### 13.5 Backlog 验收映射

| Backlog 验收项 | 设计 Task | 主要验证 |
| --- | --- | --- |
| 通用 `BlockPollEvent` payload | T1、T2 | Packet / Arrow payload 测试 |
| Arrow 不再是唯一 packet 表示 | T1、T3 | Packet 热路径无 Arrow 构造 |
| `packet.v1` 字段与版本 | T4 | schema / Arrow metadata 测试 |
| `PacketBatchView` 与 buffer 引用 | T2、T3 | 构造、访问和 lease 生命周期 |
| schema / context 扩展策略 | T3、T4、T7 | major/minor 和 sidecar present/absent |
| `protocol::Layers` 映射 | T5、T6 | layer sequence / offset 断言 |
| 最内层 L2/L3/L4 与 IP/L4 字段 | T5、T6 | 基础链和 VXLAN 样本 |
| VLAN / tunnel depth | T5、T6、T8 | QinQ 和固定 VXLAN 样本 |
| packet context 复用 | T3、T7 | sidecar 对齐和只读发布 |
| classifier 与来源解耦 | T5、T6 | 构造型 packet 和 LINKTYPE 测试 |
| 不调用应用层识别 | T5、T8 | Hyperscan 不被调用 |
| 截断、畸形、层数超限安全 | T6、T8、T9 | 边界测试和 sanitizer |
| DPDK / AF_XDP 可复用 | T4、T5、T6 | 输入仅 packet/cap_len/link_type |
| 至少一种隧道样本 | T8 | VXLAN 完整链断言 |
| `packet_filter.v1` IP/端口结构输入 | T5、T7 | checked accessor 状态和边界测试 |

### 13.6 设计门禁与文档同步

`planning.md` 已于 2026-07-23 按本设计完成同步：

1. Story 19.2 按完整 Story 规划，包含 tunnel depth、完整支持矩阵和固定隧道样本。
2. T0-T10 已按 planning 的 ID、名称和顺序建立设计章节，并绑定验收 ID、主要文件和测试目标。
3. Scheduler 范围、ABI 迁移、CTest、Sanitizer、TSan 和性能门槛均已纳入计划。
4. “可运行骨架”仅指构造型进程内闭环，Scheduler 生产装配明确延期。
5. 当前估算为 17.5 PD，评估区间为 16-20 PD。

后续任何设计变化必须在同一次变更中同步更新：

- 本文验收 ID 与设计章节映射。
- `planning.md` 的 Task、测试目标、主要文件、工作量和依赖。
- 受影响的 Backlog 验收项；禁止只调整一份 Sprint 文档。

实现完成的 Definition of Done：

- Story 19.1 和完整 Story 19.2 的验收映射全部闭合。
- 新增测试进入 CTest，验证命令实际执行对应二进制。
- 安全解析边界在 ASan / UBSan 下无越界和未定义行为。
- public ABI、接口 IID、框架文档、Sprint 计划和产品待办同步更新。
- 性能基准记录 3 种 sidecar 访问模式及原位 `LayerHints()` 路径。

### 13.7 文件落点与完成出口

- `planning.md` / `design.md`：Task、设计、验收和状态同步。
- `tasks/product_backlog.md`：仅在 Story 验收完成后更新状态。
- `review.md`：记录每个 acceptance ID 的实现、命令、结果和证据。

T10 只有在 T1-T9 全部完成、20 个验收 ID 均有实现与可执行证据、文档无范围漂移且 Backlog 状态同步后才能完成。
