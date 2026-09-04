# Feature: NPM 离线数据包全量导入

状态：`[-]` 进行中
优先级：P0

## 业务意图

提供 `pcapfile` 源通道，将单个 pcap/pcapng 文件读取为有限、按文件顺序排列的
packet block stream，供 NPM 算子和后续过滤算子消费。导入必须保留原始报文
字节、事件时间、捕获长度、线速长度、来源和顺序；文件回放不能改变 packet 的事件时间。

`pcapfile` 是数据源通道，不是算子。文件读取、格式解析、buffer 所有权和 EOF 由通道
负责；`npm.basic`、`npm-offline-filter` 等算子负责后续分析或筛选。

## Non-Goals

- 不实现多文件归并、跨文件时间排序或独立的 `pcap_replay` 通道类型；回放是 `pcapfile` 的模式。
- 不实现离线过滤表达式、WHERE 下推、flow/session 聚合、TCP 重组或应用协议识别。
- 不实现实时网卡、AF_PACKET、AF_XDP、PF_RING、DPDK 等采集后端。
- 不实现 pcap/pcapng 写出、数据库落盘或前端管理界面。
- 不保证通道配置跨进程或重启持久化；本 Feature 只要求插件运行期的配置管理。
- 不实现压缩封装（如 `.gz`/`.zst`）的透明解压，也不承诺 ERF、snoop、NetMon 等
  非 pcap savefile 容器；这些输入必须以明确的 source error 拒绝。
- 不新增 packet 数据 Schema；复用 `npm-packet-contract` 已冻结的 packet 契约和本 Feature 使用的 block ABI。
- 不新增 `IStreamChannel` 适配路径；本 Feature 的正式数据面固定为现有
  `IBlockStreamChannel` + `arrow::RecordBatch` API。

## 前置依赖与架构边界

- `npm-packet-contract` 已冻结 `PacketMeta`、`PacketRecord`、原始字节所有权和 layer context 语义。
- 本 Feature 复用现有 `IBlockStreamChannel`、`BlockPollEvent`、`PacketRecord`、
  `PacketLayerInfo`、`PacketSchema()` 和 `EncodePacketBatch()` 契约；不新增 block payload、
  lease 或 packet Schema ABI。
- source 通过 `IID_PROTOCOL` 获取现有 `IProtocol`，每包调用一次 `Layer()` 填充
  `PacketLayerInfo`；不得在导入阶段调用 `Identify()`。
- 若宿主未发现 `IID_PROTOCOL`，`pcapfile` 插件在加载或通道打开阶段返回 `ENODEV`，不得
  静默发布没有 layer 信息的 data block；`Layer()` 对单包返回负值或非法 layer 数时，按
  现有 NPI 适配规则将该包标记为 `kMalformed` 并保留原始字节，不升级为文件 source error。
- `pcapfile` 内部使用 `PcapLayerAdapter` 将现有 `protocol::Layers` 转换为
  `PacketLayerInfo`；该 helper 不是插件接口，不注册新的 IID，也不把 NPI 具体类暴露到公共 ABI。
- block source 必须接入 block stream 执行路径；当前 Scheduler 的
  `BLOCK_STREAM_NOT_IMPLEMENTED` 仅是现状门禁，不是本 Feature 的完成状态。
- Scheduler、NPM/filter 算子和 source 通道之间只通过既有 `IQuerier`/纯虚接口协作，
  不直接依赖 `PcapFileChannel` 具体类。

### 代码组织与注册发现

- 公共 block ABI 归属 `src/framework/interfaces/`：
  `IBlockStreamChannel`、`IBlockStreamFactory`、`IBlockStreamManager` 及其 IID
  和跨插件可见的数据契约均放在此处。它们是 provider 之间共享的接口，不能放入
  `src/channels/pcapfile/`。
- T1 冻结两个新 IID，数值固定如下，不得在实现或测试中临时生成：

  ```cpp
  const Guid IID_BLOCK_STREAM_FACTORY = {
      0x2e8f1a4c, 0x6d71, 0x4f92, {0x91, 0x3a, 0xc7, 0x5d, 0x20, 0x8b, 0x64, 0xe1}};
  const Guid IID_BLOCK_STREAM_MANAGER = {
      0x7b3d5e90, 0x1a42, 0x4c86, {0xb4, 0x0f, 0x72, 0xd9, 0x5e, 0x31, 0xa8, 0x6c}};
  ```

  `iblock_stream_factory.h` 必须包含 `common/guid.h`、`common/typedef.h`、
  `functional` 和 `iblock_stream_channel.h`；`iblock_stream_manager.h` 必须包含
  `common/guid.h`、`common/typedef.h`、`functional` 和 `string`。两个头文件均位于
  `src/framework/interfaces/`，并保持 C++17 可独立编译。
- 具体通道实现归属 `src/channels/<category>/`。本 Feature 仅新增
  `src/channels/pcapfile/`，其中放置 pcap/pcapng reader、option 校验、channel、
  plugin 和注册适配；pcap 专属类型不得出现在公共 ABI 中。
- 每个通道 provider 可以独立编译为 `.so`，按现有 `IPlugin` 注册机制暴露一个或多个
  `IBlockStreamFactory`/`IBlockStreamManager` 实例；provider 不要求为每种通道重复定义
  一套接口。`IChannelRegistry` 继续承担具名通道实例的注册、查找和枚举。
- 调用方只通过 IID 查询接口，不读取或比较组件名称、插件类名或 `.so` 名称；组件只要
  注册了某个 IID，就以同等能力参与发现。`PluginLoader::Regist()` 允许同一 IID 存在多个
  实现，Scheduler、控制面路由和管理逻辑必须使用 `IQuerier::Traverse()` 聚合全部实现，
  不得使用 `First()` 假设只有一个实现。
- factory 的 `Get/List` 以 `(type, name)` 返回或枚举通道；manager 对不支持的 type 在
  修改状态前返回 `ENOTSUP`，对不存在的 `(type,name)` 返回 `ENOENT`。调用方遍历所有
  manager，只有一个实现接受请求才算成功；多个实现接受同一请求属于冲突并报错，不能
  静默覆盖；未找到接受者返回明确错误。对仍有 Scheduler 绑定或 outstanding batch 的
  channel，`ModifyChannel`/`RemoveChannel` 必须返回 `EBUSY`，不得先关闭并销毁实例。
  上述规则不要求调用方知道接受者属于哪个组件。
- `IBlockStreamFactory`/`IBlockStreamManager` 使用独立 IID，不复用
  `IID_STREAM_FACTORY`/`IID_STREAM_MANAGER`；现有 Arrow stream provider 的发现和行为
  保持不变。

## 公共契约

### 通道身份与数据面

`pcapfile` 通道的身份和类型固定为：

| 项目 | 契约 |
| --- | --- |
| `Category()` | `pcapfile` |
| `Name()` | 用户指定的逻辑通道名 |
| `Type()` | `ChannelType::kBlockStream` |
| 角色 | 仅允许 source |
| `Schema()` | 返回稳定逻辑标识 `packet`；字段和 metadata 以 `PacketSchema()` 为准 |
| 输出逻辑 Schema | 现有 `flowsql::packet::PacketSchema()`（metadata：`flowsql.entity=packet`、`flowsql.schema_version=1`、`flowsql.timestamp_unit=ns`） |
| 输出 payload | `std::shared_ptr<arrow::RecordBatch>`，由 `flowsql::packet::EncodePacketBatch()` 生成 |
| 读取接口 | `IBlockStreamChannel::PollBlock()` 的 `BlockPollEvent::batch` + `ReleaseBlock()` |

通道内部持有文件句柄、不可变 source registry 和 packet buffer。`PollBlock()`
返回的 `RecordBatch` 由 `shared_ptr` 持有；下游完成消费后调用一次 `ReleaseBlock()`，不得
保存 batch 内部的借用指针。通道不得把借用的文件读取缓冲跨 block 发布。
source 通道的 `Flush()` 为幂等 no-op 并返回 0；文件数据只通过 `PollBlock()` 发布。

### 配置值

通道管理接口接收 JSON option；未知字段拒绝，规范字段如下：

```json
{
  "path": "/data/capture.pcap",
  "format": "auto",
  "batch_packets": 256,
  "replay_mode": "fast",
  "replay_speed_milli": 1000
}
```

- `path` 必填，必须指向普通文件；符号链接、目录和不可读文件在 `Open()` 前拒绝。
- `format` 为 `auto|pcap|pcapng`，`auto` 依据文件头判定；显式格式与文件头不一致时失败。
- `batch_packets` 为正整数，决定一个 block 的最大 packet 数，不改变文件顺序。
- `replay_mode` 为 `fast|timestamp`。`fast` 尽快输出；`timestamp` 按相邻 packet 时间差回放。
- `replay_speed_milli` 仅在 `timestamp` 下生效，必须大于 0；1000 表示原速。

### 文件格式支持矩阵

`format=auto` 只按文件头 magic 判断，不依赖扩展名；显式 `pcap`/`pcapng` 必须与
文件头一致。首个版本的容器边界如下：

| 容器 | 支持范围 | 不满足时 |
| --- | --- | --- |
| classic pcap savefile | 标准四种 magic（大/小端 × 微秒/纳秒）、version 2.4；解析 global header、单一 LINKTYPE、record header 和 packet bytes | 文件头、版本、端序、时间精度、长度或 LINKTYPE 字段非法时 source error |
| pcapng | 一个或多个 Section Header Block；每个 section 的 endian 独立；Interface Description Block + Enhanced Packet Block；按 interface 解析 `if_tsresol`（十进制或二进制）与 `if_tsoffset` | 缺少所需 SHB/IDB、section/block 长度或选项损坏、EPB 引用未知 interface、时间换算溢出时 source error |
| 其他 pcapng packet block | Simple Packet Block 和已废弃 Packet Block 首版不支持 | source error，不跳过并伪造 packet 时间戳 |
| 压缩或其他 capture 容器 | 不支持透明解压和格式转换 | source error |

pcapng 的 `source_id` 在整个文件内按 interface declaration 的遇到顺序分配；packet
block 的 interface ID 只在所属 section 内解释。`if_tsresol` 缺失时使用 pcapng 默认微秒精度；
十进制精度按 `10^-n`、二进制精度按 `2^-n` 以有理数整数运算换算到 epoch ns，`if_tsoffset`
作为有符号秒数加入；结果不是整数纳秒时采用 round-to-nearest、ties-to-even，且禁止浮点
近似。换算结果超出 `int64` 范围时为 source error。未知但结构合法的 pcap/pcapng LINKTYPE 仍可导入，交由
现有 layer decoder 产生 `kUnsupportedLinkType`，不能将链路类型支持范围误当成容器格式支持范围。

建议的值结构（实现内部，不作为跨插件 ABI）为：

```cpp
enum class PcapReplayMode { kFast, kTimestamp };

struct PcapFileSourceConfig {
    std::string path;
    std::string format = "auto";
    uint32_t batch_packets = 256;
    PcapReplayMode replay_mode = PcapReplayMode::kFast;
    uint32_t replay_speed_milli = 1000;
};
```

### block source 查找与管理接口

由于 `IStreamFactory` 的返回类型是 `IStreamChannel*`，不能把 block channel 强转为 Arrow
stream。新增公共头文件 `src/framework/interfaces/iblock_stream_factory.h`，使用上文冻结的独立 IID：

```cpp
interface IBlockStreamFactory {
    virtual ~IBlockStreamFactory() = default;
    virtual IBlockStreamChannel* Get(const char* type, const char* name) = 0;
    virtual void List(std::function<void(const char* type,
                                         const char* name,
                                         IBlockStreamChannel*)> callback) = 0;
};
```

`Get()`/`List()` 返回的 channel 指针均为非拥有指针；provider 在通道删除或插件卸载前
保持实例有效，调用方不得 `delete`。Scheduler 不得把该裸指针跨越 provider 的删除或卸载
生命周期保存；宿主必须保证消费期间 provider 仍然存活。

`PollBlock()` 的 `kData` 事件必须携带非空 batch，其余事件的 batch 必须为空；调用方对
每个已返回的 `kData` batch 恰好调用一次 `ReleaseBlock(batch)`，不得将 batch 内部 buffer
指针保存到下一次轮询或通道关闭之后。`ReleaseBlock()` 不转移 `shared_ptr` 所有权，返回
非零表示 batch 不属于该通道或重复释放。

`src/framework/interfaces/iblock_stream_manager.h` 提供与现有流通道一致的配置管理，使用上文冻结的独立 IID：

```cpp
interface IBlockStreamManager {
    virtual ~IBlockStreamManager() = default;
    virtual int AddChannel(const std::string& type,
                           const std::string& name,
                           const std::string& option) = 0;
    virtual int ModifyChannel(const std::string& type,
                              const std::string& name,
                              const std::string& option) = 0;
    virtual int RemoveChannel(const std::string& type,
                              const std::string& name) = 0;
    virtual void QueryChannels(
        std::function<void(const std::string& type,
                           const std::string& name,
                           const std::string& option,
                           const std::string& status)> callback) = 0;
};
```

两个接口使用新 IID，不复用 `IID_STREAM_FACTORY`、`IID_STREAM_MANAGER`。`PcapFilePlugin`
实现 `IPlugin + IBlockStreamFactory + IBlockStreamManager`；其具体 `.so` 名称和路由前缀
由实现任务冻结，但不得让 Scheduler 依赖具体插件类或组件名称。

接口发现必须遵循上面的多 provider 规则：Scheduler 先遍历全部 block factory，按
`(type, name)` 解析 source；管理请求遍历全部 block manager，并将唯一命中的请求路由给
对应 provider。单个 provider 可以只实现 factory 或只实现 manager，但 `pcapfile` 在本
Feature 中同时提供二者。

### packet 字段映射

每个结构合法的文件记录产生一个 `flowsql::packet::PacketRecord`，并由现有
`PacketSchema()` 编码为一行：

| 文件/来源字段 | packet 字段 | 规则 |
| --- | --- | --- |
| pcap/pcapng 时间戳 | `PacketMeta::timestamp_ns` | 按文件声明精度以整数有理数转换为 epoch ns；非整数纳秒按 ties-to-even 量化，不使用浮点近似 |
| `incl_len` / captured length | `PacketMeta::captured_len` | 等于实际拥有字节数 |
| `orig_len` / wire length | `PacketMeta::wire_len` | `0` 表示未知；非零时必须 `wire_len >= captured_len` |
| 文件 LINKTYPE / pcapng interface link type | `PacketMeta::link_type` | 使用标准 LINKTYPE 数值 |
| pcap 单源或 pcapng interface | `PacketMeta::source_id` | 在通道内稳定；pcap 单源为 0 |
| 文件记录顺序 | `PacketMeta::sequence` | 同一 source 内从 0 开始递增；现有 packet 契约没有额外的全局 ID 字段 |
| 捕获字节 | `PacketRecord::raw_data` | 原样保留，不因解析或回放修改 |

pcapng 的 interface 到 `source_id` 映射按 interface declaration 顺序固定；不按并发 worker
或 block 边界重新编号。一个 block 内允许包含多个 source，但仍保持文件全局顺序。

source 在发布 block 前通过 `IID_PROTOCOL` 使用现有 NPI layer 适配逻辑，对每个非空且
LINKTYPE 支持的 packet 调用一次 `IProtocol::Layer()`，由 `src/channels/pcapfile/`
内的 `PcapLayerAdapter` 将结果转换为同一 `PacketRecord` 内的 `PacketLayerInfo`；不支持
的 LINKTYPE 直接写入 `kUnsupportedLinkType`，不调用 `Layer()`。转换规则复用现有
`NpiPacketLayerDecoder` 的字段、截断和畸形语义，但不依赖其具体类或新增 decoder IID。
导入阶段不得调用 `Identify()`；协议识别由后续 NPM 算子按采样策略执行。合法但截断或
畸形的 packet 保留原始字节并携带对应 `LayerStatus`，不得因单个坏包丢弃整个 block。

### 流语义与错误

`pcapfile` 的固定语义是：有限流、可靠 EOF、GLOBAL_FIFO、无静默丢包、单生产者；
若实现内部有队列，队列满时必须阻塞或返回明确的背压错误，不能跳过 packet。`PollBlock()` 事件遵循
`BlockPollEvent`：数据、超时、EOF、取消和错误互斥。

- 空文件正常打开后直接产生 EOF，不产生空 data block。
- 文件头、section/interface、record header、未知 pcapng block、长度溢出、压缩封装或 I/O
  错误属于 source error；错误后不得再产生 EOF。
- `wire_len != 0 && wire_len > captured_len` 是正常的捕获截断，交给 layer decoder 输出
  `kTruncated`；`wire_len == 0` 表示线速长度未知；文件实际不足 `captured_len` 字节则是
  读取错误，不伪造短 packet。
- `fast` 模式不等待；`timestamp` 模式只按相邻时间差等待，负差按 0 处理且不重排记录。
- `Cancel()` 停止新 block；调用方对已返回的 batch 仍须 exactly-once 调用 `ReleaseBlock()`；
  正常读完才发送 EOF。
- 通道 `Close()`、插件 `Stop/Unload()` 不得与仍在消费的 batch 并发销毁其 owner。

## 主链路

### 文件到 packet block stream

1. `PcapFilePlugin` 校验并规范化 option，建立 `pcapfile` 通道和 source registry。
2. `Open()` 校验 pcap/pcapng 文件头，解析 timestamp resolution、LINKTYPE 和 interface 定义。
3. 读取固定顺序的 record，校验长度，复制 packet bytes，填充 `PacketRecord` 的 `meta` 和
   `raw_data`，并递增各 source 的 `sequence`。
4. 调用一次 `IProtocol::Layer()`，将结果写入 `PacketRecord::layer`，再调用
   `EncodePacketBatch()` 构造 `std::shared_ptr<arrow::RecordBatch>` 后发布。
5. `PollBlock()` 返回 `BlockPollEvent::batch`；下游 block operator 只读消费，执行器随后调用
   `ReleaseBlock(batch)` 一次。
6. 文件末尾发送 EOF；I/O、格式或资源错误走 error 终态，不伪装成 EOF。

### Scheduler 编排

1. source 解析得到 `Type() == block_stream` 时，Scheduler 通过 `IBlockStreamFactory` 查找，
   不再命中当前 `BLOCK_STREAM_NOT_IMPLEMENTED` 分支。
2. Scheduler 在首个 block 前按现有 `PacketSchema()` 校验 `RecordBatch` schema，再调用
   `IBlockStreamOperator::OnSchemaReady()` 并启动 block operator；operator 不保存 batch 内部
   的借用指针。
3. 成功、主动停止、operator 错误、异常和取消路径都必须调用一次 `ReleaseBlock(batch)`；
   终态写入任务快照。

## 原子任务

- `[x]` T1：公共接口冻结与契约测试（每个切片 10～30 分钟）：
  - `[x]` T1.1：新增两个公共头文件，冻结上文 IID、include、纯虚方法签名、非拥有指针和 `ReleaseBlock()` exactly-once 语义。
  - `[x]` T1.2：冻结 `pcapfile` 的 type/category/role、option JSON 校验、插件注册和 IID-only 多 provider 发现规则。
  - `[x]` T1.3：新增最小接口契约测试，覆盖头文件独立编译、同 IID 多实现 `Traverse()`、factory 枚举和 manager 的 `ENOTSUP`/`ENOENT` 返回。
- `[x]` T2：文件解析与记录归一化（每个切片 10～30 分钟）：
  - `[x]` T2.1：实现 classic pcap magic/端序数值读取和 v2.4 global header 校验，加入最小 fixture。
  - `[x]` T2.2：实现 classic pcap record header、时间精度、长度校验和 packet bytes 读取，加入字段断言。
  - `[x]` T2.3：实现 pcapng SHB、section endian 和 block length 校验，加入多 section fixture。
  - `[x]` T2.4：实现 pcapng IDB 及 `if_tsresol`/`if_tsoffset`，建立 interface 到 source 的映射。
  - `[x]` T2.5：实现 pcapng EPB、未知 interface/不支持 block 拒绝和结构错误终态。
  - `[x]` T2.6：将记录映射为 `PacketRecord`，完成 owning bytes、source/sequence、整数时间量化和 `EncodePacketBatch()` 单元测试。
  - `[x]` T2.7：修正 pcapng section 结构处理，迭代解析连续 SHB、校验 SHB options，并拒绝缺少 IDB 的 section。
  - `[x]` T2.8：以持久文件句柄增量读取 classic pcap/pcapng，移除 `Open()` 整文件加载，并把读取或分配失败收敛为 source error；该切片同时作为 T3 生命周期与错误终态的完成前置。
- `[x]` T3：`pcapfile` 通道生命周期与生产语义（每个切片 10～30 分钟）：
  - `[x]` T3.1：实现 option parser/normalizer 和 `PcapFileChannel` 构造、`Open()`/`Close()`。
  - `[x]` T3.2：完成 pcapfile plugin 的 `IPlugin`/factory/manager 注册适配。
  - `[x]` T3.3：完成 factory 通道实例表、`Get/List` 和 provider 生命周期保持。
  - `[x]` T3.4：完成 manager 的 `Add/Modify/Remove/Query`、运行期配置表、`(type,name)` 冲突和 active channel 的 `EBUSY` 保护。
  - `[x]` T3.5：实现 `PollBlock()`/`ReleaseBlock()`、batch 上限、空文件 EOF 和正常 EOF once。
  - `[x]` T3.6：实现 `PcapLayerAdapter`，接入 `IID_PROTOCOL`/`IProtocol::Layer()` 及 layer 状态映射。
  - `[x]` T3.7：实现 fast/timestamp replay 和相邻时间差的确定性等待。
  - `[x]` T3.8：实现 cancel、背压、I/O/格式错误终态和 batch owner 生命周期。
- `[x]` T4：Scheduler block source 接入（每个切片 10～30 分钟）：
  - `[x]` T4.1：新增 block factory/manager 的 IID 遍历 helper，覆盖零个、一个和多个实现。
  - `[x]` T4.2：实现 `(type,name)` source 查找、重复通道冲突和 manager `ENOTSUP`/唯一命中路由。
  - `[x]` T4.3：解除 `BLOCK_STREAM_NOT_IMPLEMENTED`，接入现有 `IBlockStreamOperator::OnSchemaReady()`。
  - `[x]` T4.4：接入 `ProcessBlock()`、`kTimeout`/`kEof`/`kCancelled`/`kError` 事件和 `ReleaseBlock()`。
  - `[x]` T4.5：补齐成功、主动停止、异常、取消和 operator 错误路径的任务终态传播。
- `[ ]` T5：测试与工程闭环（每个切片 10～30 分钟）：
  - `[ ]` T5.1：补齐 classic pcap/pcapng 格式、字段、顺序、时间量化和结构错误 fixture。
  - `[ ]` T5.2：补齐 layer decode 调用次数、unsupported/truncated/malformed 和 raw bytes 保留测试。
  - `[ ]` T5.3：补齐 EOF、错误、replay、cancel、背压、ReleaseBlock exactly-once、owner 生命周期和 active channel `EBUSY` 测试。
  - `[ ]` T5.4：补齐多 provider IID 发现、冲突和 manager 路由测试。
  - `[ ]` T5.5：补齐 Scheduler E2E 测试和 block source 终态断言。
  - `[ ]` T5.6：注册 CTest、执行标准 CMake 构建并修复编译/测试失败。
  - `[ ]` T5.7：执行既有 stream/framework/scheduler 回归并完成 diff 检查。

## 实施进度与范围偏差

- 本轮实际修改曾提前覆盖 T2、T3、T4 和 T5 的部分实现文件；这属于执行范围偏差，不代表
  T1.1 的任务定义扩大，也不改变本规格的业务契约、公共 ABI 或 Non-Goals。
- 已勾选的后续切片仅表示已有实现和对应的聚焦测试证据；未勾选切片即使存在部分代码，仍须
  在重新装载为当前 `active_task` 后完成剩余验收，不能按“已有代码”直接视为完成。
- 后续工作从未勾选的最小切片继续；开始前必须在 `tasks/active_task.md` 冻结允许文件、验收命令、
  时间盒和停止条件。不得通过修改任务定义来吸收此前的越界改动。

## 测试锚点

| 验收 | 必测断言 |
| --- | --- |
| 公共接口 | 两个头文件可独立编译；IID 数值稳定；同 IID 多实现可由 `Traverse()` 发现；`Schema()=="packet"`、source `Flush()` 幂等；factory/manager 裸指针不被调用方释放；active channel 修改/删除返回 `EBUSY`；每个 `kData` batch 恰好一次 `ReleaseBlock()` |
| 格式读取 | pcap 四种 magic、version 2.4、微秒/纳秒时间戳；pcapng 多 section/interface、EPB、十进制/二进制 `if_tsresol`、`if_tsoffset`、非整数纳秒量化和溢出；format auto 与显式格式校验；压缩/其他容器和不支持 packet block 拒绝 |
| 字段保留 | `timestamp_ns`、`captured_len`、`wire_len`、`link_type`、`source_id`、`sequence` 和 raw bytes 精确一致 |
| 顺序与身份 | 多 block 仍为文件 GLOBAL_FIFO；同 source 的 `sequence` 递增；不同 source 的编号不互相覆盖 |
| layer decode | 支持的 LINKTYPE 每包调用一次 `IProtocol::Layer()`；不调用 `Identify()`；`PacketLayerInfo` 与 `PacketRecord` 同行；unsupported/truncated/malformed 保留原包 |
| 有限流 | 空文件 EOF；正常末尾只产生一次 EOF；EOF 后不再产生 data；I/O/格式错误不产生 EOF |
| 回放 | fast 不等待；timestamp 按时间差和 speed 生效；时间倒退不重排且不负等待 |
| 生命周期 | cancel 后停止生产；成功、主动停止、错误、异常和析构路径 `ReleaseBlock()` exactly-once；channel 先析构不悬挂 batch owner |
| 注册发现 | 可同时加载多个 block provider；Scheduler/管理面遍历全部实现；重复 `(type,name)` 拒绝且不覆盖；缺失 provider/通道返回明确错误；active channel 修改/删除返回 `EBUSY` |
| Scheduler | `block_stream` source 不再返回 `BLOCK_STREAM_NOT_IMPLEMENTED`；`PacketSchema()` 不匹配拒绝；任务终态可观测 |
| 工程门禁 | `test_pcapfile_import`、接口契约测试、既有 `test_stream`/`test_framework`/`test_scheduler_e2e` 接入 CTest，标准 CMake 构建和 `git diff --check` 通过 |

## 完成出口

1. T1.1-T5.7 全部完成，所有接口、配置和错误语义在本文件中有对应实现位置。
2. pcap/pcapng 固定 fixture 的字段、顺序、raw bytes、layer status、EOF 和错误测试全部通过。
3. Scheduler block source 生产路径已可执行，不再以占位错误结束；现有 Arrow stream 回归无变化。
4. 标准命令 `cmake -B build src`、`cmake --build build -j$(nproc)` 和对应 CTest 全部通过。
