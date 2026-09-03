# Feature: NPM 数据包契约与解析基础

状态：`[x]` 已完成
优先级：P0

## 业务意图

为离线文件和实时网卡采集建立唯一的 `packet` 数据契约。解码分为两个阶段：采集阶段对隧道报文保留完整可解码的 layer path，再按策略提取一个 MAC/IP/Port 端点上下文；NPM 阶段复用已缓存的 layer 结果，仅对会话前若干个 payload 包执行 protocol 识别。契约必须保留原始报文和采集元数据，避免重复分层和对每个包执行高成本协议识别。

## Non-Goals

- 不实现 pcap/pcapng 文件读取、回放或网卡采集后端。
- 不实现 AF_PACKET、AF_XDP、PF_RING、DPDK 的内存和队列管理。
- 不实现 TCP/UDP 重组、flow/session 状态、应用协议事务或性能指标计算。
- 不决定每个 TCP/UDP session 的 protocol 识别采样窗口；“前若干个 payload 包”的选择由后续 NPM session Feature 负责。
- 不实现离线过滤表达式、数据库存储和 HTTP 路由。
- 不在同一条 packet 记录中同时输出隧道内外两套 MAC/IP/Port；完整隧道信息由 layer path 保留，双视图结果留给后续隧道/多层地址 Feature。
- 不提取或定义 VXLAN VNI、GTP TEID、GRE Key 等隧道标识的 session 语义；后续 session Feature 处理地址重叠的隧道流时不得只使用内层五元组。
- 不修改现有 `IStreamChannel`、`IDataFrame` ABI；packet 的规范承载格式为 Arrow `RecordBatch`。
- 不把 NPI 的具体类型暴露到通用 packet 数据契约；NPI 仅通过适配器提供解码能力。

## 公共契约

公共头文件拟为 `src/framework/interfaces/ipacket.h`，命名空间为 `flowsql::packet`。本 Feature 不新增插件 IID；packet 是跨通道的数据值契约，协议识别继续使用已有 `flowsql::IProtocol`。

### C++ 数据结构

```cpp
#include <array>
#include <cstdint>
#include <memory>
#include <variant>

#include <common/network/netaddress.h>
#include <common/span.h>
#include <common/typedef.h>

namespace flowsql::packet {

constexpr size_t kMaxLayerDepth = 15;

enum class LayerStatus : uint8_t {
    kNotDecoded = 0,
    kDecoded = 1,
    kTruncated = 2,
    kMalformed = 3,
    kUnsupportedLinkType = 4
};

enum class ProtocolStatus : uint8_t {
    kNotAttempted = 0,  // 采集阶段或未命中 session 采样窗口
    kIdentified = 1,
    kUnknown = 2
};

enum class AddressFamily : uint8_t {
    kNone = 0,
    kIPv4 = 4,
    kIPv6 = 6
};

// 采集阶段只按需填充这些字段；未请求或报文不存在时 valid/variant 状态为空。
enum LayerFieldMask : uint32_t {
    kExtractNone = 0,
    kExtractMac = 1u << 0,
    kExtractIp = 1u << 1,
    kExtractPort = 1u << 2
};

enum class EndpointScope : uint8_t {
    kInnermost = 0,  // 默认：最内层可识别的 IP 上下文，面向业务 NPM
    kOutermost = 1   // 可选：最外层 IP 上下文，面向隧道承载流量
};

constexpr uint8_t kNoLayerIndex = 0xff;

struct LayerDecodeOptions {
    uint32_t field_mask = kExtractNone;
    EndpointScope endpoint_scope = EndpointScope::kInnermost;
};

// 复用 common/network/netaddress.h 中的地址布局，避免 packet 契约重新定义地址类型。
using IPv4Address = ::IPv4Address;
using IPv6Address = ::IPv6Address;

struct MacAddress {
    ::EtherAdderss value{};
    uint8_t valid = 0;
};

// monostate 表示未提取或报文中不存在地址；variant 不分配堆内存。
// 单个 C++ 对象的大小仍由 IPv6 分支和对齐决定，IPv4 的主要存储收益来自
// Arrow 中 IPv4/IPv6 分列，而不是把两个地址对象同时放入每个 packet。
using IpAddress = std::variant<std::monostate, IPv4Address, IPv6Address>;

struct LayerRef {
    uint16_t kind = 0;   // packet contract 的 layer code，NPI 适配器负责映射
    uint32_t offset = 0; // 相对 raw_data 起始位置
};

struct PacketMeta {
    int64_t timestamp_ns = 0;  // epoch ns；该字段是 packet 的权威事件时间
    uint32_t captured_len = 0; // raw_data 中实际保存的字节数
    uint32_t wire_len = 0;     // 线速长度；0 表示采集源未知
    uint32_t link_type = 0;    // libpcap DLT 数值
    uint32_t source_id = 0;    // 源内接口/队列标识
    uint64_t sequence = 0;     // source_id 范围内的单调递增序号
};

// 只读借用视图；仅在采集回调或当前 Poll 生命周期内有效。
struct PacketView {
    PacketMeta meta;
    Span<const uint8_t> bytes;
};

// 可跨线程、跨批次保留的报文字节。owner 负责维持 data 的生命周期。
struct PacketBytes {
    std::shared_ptr<const void> owner;
    const uint8_t* data = nullptr;
    uint32_t size = 0;
};

struct PacketLayerInfo {
    LayerStatus status = LayerStatus::kNotDecoded;
    uint8_t layer_count = 0;
    std::array<LayerRef, kMaxLayerDepth> layers{};
    EndpointScope endpoint_scope = EndpointScope::kInnermost;
    uint8_t network_layer_index = kNoLayerIndex;
    uint8_t transport_layer_index = kNoLayerIndex;
    uint32_t payload_offset = 0;
    MacAddress src_mac;
    MacAddress dst_mac;
    IpAddress src_ip;
    IpAddress dst_ip;
    uint8_t transport_protocol = 0;  // IP protocol number; 0 表示未知
    uint16_t src_port = 0;
    uint16_t dst_port = 0;
    uint8_t ports_valid = 0;
};

struct PacketProtocolInfo {
    ProtocolStatus status = ProtocolStatus::kNotAttempted;
    uint16_t id = 0;
    uint16_t sub_id = 0;
};

struct PacketRecord {
    PacketMeta meta;
    PacketBytes raw_data;
    PacketLayerInfo layer;
    PacketProtocolInfo protocol;
};

interface IPacketLayerDecoder {
    virtual ~IPacketLayerDecoder() = default;
    // 每个包都调用；先建立完整 layer path，再按 options 选择端点上下文。
    virtual PacketLayerInfo Decode(const PacketView& packet, const LayerDecodeOptions& options) = 0;
};

interface IPacketProtocolIdentifier {
    virtual ~IPacketProtocolIdentifier() = default;
    // 只在 session 采样窗口内调用；不得再次执行 layer 解析。
    virtual PacketProtocolInfo Identify(const PacketView& packet,
                                        const PacketLayerInfo& layer) = 0;
};

}  // namespace flowsql::packet
```

`src/framework/core/packet_codec.h` 提供与上述数据结构配套的无状态辅助入口：

```cpp
enum class PacketEnvelopeError : uint8_t {
    kNone = 0,
    kNullOutput,
    kLengthMismatch,
    kNullData,
    kWireLengthTooSmall,
    kAllocationFailed
};

PacketEnvelopeError ValidatePacketView(const PacketView& packet);
PacketEnvelopeError CopyPacketBytes(const PacketView& packet, PacketBytes* output);
std::shared_ptr<arrow::Schema> PacketSchema();

enum class PacketBatchError : uint8_t {
    kNone = 0,
    kNullOutput,
    kInvalidRecord,
    kArrowError,
    kAllocationFailed
};

PacketBatchError EncodePacketBatch(const std::vector<PacketRecord>& records,
                                   std::shared_ptr<arrow::RecordBatch>* output,
                                   std::string* error = nullptr);
```

`CopyPacketBytes` 对非空借用报文执行一次拥有化复制；`PacketSchema` 返回线程安全共享的固定 Schema；`EncodePacketBatch` 不重新解析报文，只校验记录并将已有字段编码到该 Schema。

约束如下：

- `PacketView::bytes.size == meta.captured_len`。跨异步边界后必须转换为带 `owner` 的 `PacketBytes`，且 `PacketBytes::size == meta.captured_len`；`size > 0` 时 `data` 和 `owner` 均不可为空，不得保存借用指针。
- `wire_len == 0` 表示未知；已知时必须满足 `wire_len >= captured_len`。`raw_data` 的字节内容必须与输入报文完全一致，不因解码而修改。
- `sequence` 只要求在同一个 `source_id` 内递增，不承诺多源之间的全局顺序。离线源按文件读取顺序分配序号。
- `layer_count <= kMaxLayerDepth`，每个 `LayerRef::offset <= captured_len`，`payload_offset <= captured_len`。采集阶段不得因为端点选择而提前停止分层；隧道、扩展头和内层封装均进入同一 `layer path`。layer code 的数值空间属于 packet contract，NPI 适配器负责与已有 `protocol::eLayer` 映射。
- `protocol::Layers` 只允许作为 NPI 适配器内部的临时结构；公共 `PacketLayerInfo` 只保存与 NPI 解耦的 layer path 快照，不得将 `protocol::Layers` 暴露到 framework 接口或 Arrow 数据面。快照转换是每包一次、按 `kMaxLayerDepth` 有界的线性复制/类型映射，不得引入堆分配，也不属于第二次报文分层。
- 采集阶段完成一次 `IProtocol::Layer()` 后立即生成 layer path 快照；NPM 阶段仅对命中 session 采样窗口的包从快照重建临时 `protocol::Layers`，再调用 `IProtocol::Identify()`，未采样包不得重建或识别。由于 NPI 的 offset/payload 类型为 `uint16_t`，快照值无法表示时不得截断，应跳过 `Identify()` 并报告协议未知/不可识别。
- `LayerDecodeOptions::endpoint_scope` 按采集源/解码器配置，默认 `kInnermost`，可切换为 `kOutermost`；同一条记录只保留所选端点上下文。`network_layer_index` 和 `transport_layer_index` 指向完整 path 中的选中层，找不到时为 `kNoLayerIndex`。`payload_offset` 与地址/端口必须来自同一选中上下文。
- `endpoint_scope` 在同一逻辑输出流内必须固定；需要同时分析内外层时建立两个独立的投影/流，不得在同一 session 中混用两种 scope。默认最内层仅是选取策略，不改变完整 `layer path`。
- `EncodePacketBatch` 将一个 batch 视为同一逻辑输出流，拒绝混用不同 `endpoint_scope` 的记录；需要双视图时必须先分流。
- IP 取选中 network layer；若没有可识别的 IP 层，`network_layer_index=kNoLayerIndex`，不把其他层伪装成 IP。`transport_layer_index` 指该 network layer 之后同一封装段内的第一个 L4/隧道层（如 TCP、UDP、GRE、GTP）；`transport_protocol` 填该 IP 上下文最终对应的 IANA IP protocol/next-header 编号，隧道层没有 TCP/UDP 端点时 `ports_valid=0`。端口只从 TCP/UDP/SCTP 层提取。
- MAC 取与选中 IP 层属于同一封装段、且位于其之前的最近 Ethernet 层；若没有选中 IP 层，则按 scope 选择对应封装段的 Ethernet（无嵌套时即唯一 Ethernet）。不存在对应 Ethernet 层时 MAC 保持无效，不回退到另一封装段。
- MAC/IP/Port 不使用字符串。MAC 复用 `::EtherAdderss` 的 6 字节布局，并由 `MacAddress::valid` 区分“未提取/不存在”和有效的全 0 值；IPv4/IPv6 复用 `::IPv4Address`、`::IPv6Address`，通过 `IpAddress` 的 `std::variant` 分支区分类型，`std::monostate` 表示未提取或不存在。现有 `IPv4Address::addr/dword` 与 `IPv6Address::bytes[16]` 均按网络字节序原样映射；非热路径才调用 `to_str()`。端口是 `uint16_t` 加 `ports_valid`。不以 `0.0.0.0`、全 0 MAC 或空字符串冒充字段缺失。C++ 变体避免同时保存 IPv4/IPv6 两份地址并减少转换，单个对象的静态大小仍由最大分支决定，实际的 IPv4 内存节省由 Arrow 分列布局保证。
- `PacketProtocolInfo::id/sub_id` 对应已有 NPI `Protocol::id/subid`；当状态为 `kNotAttempted` 或 `kUnknown` 时二者都保持为 0，不生成 `protocol_name`。协议名称只在非热路径的词典/查询层按 ID 解析。
- `StreamBatch.ts_ms` 仅作为批次调度时间，取该批有效 packet 的最大 `timestamp_ns / 1'000'000`；packet 行内 `timestamp_ns` 始终是事件时间真相。

### Arrow Schema

每行对应一个 packet，Schema 版本为 `1`，元数据至少包含：
`flowsql.entity=packet`、`flowsql.schema_version=1`、`flowsql.timestamp_unit=ns`。

该 Schema 的可空字段语义只在 Arrow 路径中保证。当前 `IDataFrame::FieldValue` 没有显式 null 类型，转入 DataFrame 时不得静默把缺失字段映射为有效的 `0` 或空字符串；具体映射留给后续结果存储 Feature。`layer_ids` 和 `layer_offsets` 是固定长度传输列，只有前 `layer_count` 项有效。

| 列名 | Arrow 类型 | 可空 | 语义 |
| --- | --- | --- | --- |
| `timestamp_ns` | `int64` | 否 | packet 事件时间 |
| `captured_len` | `uint32` | 否 | 实际保存字节数 |
| `wire_len` | `uint32` | 否 | 线速长度，0 表示未知 |
| `link_type` | `uint32` | 否 | DLT 链路类型 |
| `source_id` | `uint32` | 否 | 源接口/队列标识 |
| `sequence` | `uint64` | 否 | 源内顺序 |
| `raw_data` | `binary` | 否 | 原始捕获字节 |
| `layer_status` | `uint8` | 否 | `LayerStatus` 数值 |
| `layer_count` | `uint8` | 否 | 有效 layer 数量 |
| `layer_ids` | `fixed_size_list<uint16>[15]` | 否 | layer code，前 `layer_count` 项有效 |
| `layer_offsets` | `fixed_size_list<uint32>[15]` | 否 | layer 起始偏移，前 `layer_count` 项有效 |
| `endpoint_scope` | `uint8` | 否 | `EndpointScope` 数值 |
| `network_layer_index` / `transport_layer_index` | `uint8` | 否 | 选中 layer 在 path 中的索引，`255` 表示不存在 |
| `payload_offset` | `uint32` | 否 | L4 payload 起始偏移，未知时为 0 |
| `src_mac` / `dst_mac` | `fixed_size_binary[6]` | 是 | 源/目标 MAC |
| `src_ip_v4` / `dst_ip_v4` | `uint32` | 是 | 源/目标 IPv4，network byte order |
| `src_ip_v6` / `dst_ip_v6` | `binary` | 是 | 源/目标 IPv6；非空时长度必须为 16 |
| `src_ip_family` / `dst_ip_family` | `uint8` | 否 | `AddressFamily` 数值 |
| `transport_protocol` | `uint8` | 是 | IP protocol number |
| `src_port` / `dst_port` | `uint16` | 是 | 源/目标端口 |
| `ports_valid` | `bool` | 否 | 端口是否有效 |
| `protocol_status` | `uint8` | 否 | `ProtocolStatus` 数值 |
| `protocol_id` / `protocol_sub_id` | `uint16` | 是 | NPI 数值协议编号 |

### 解码与异常语义

1. 先校验元数据和字节长度，再调用 `IPacketLayerDecoder`。元数据不合法（长度不一致、已知 `wire_len < captured_len`）或无法建立跨边界的字节所有权时拒绝该记录，不生成 Arrow 行。
2. `IPacketLayerDecoder` 对每个包执行一次 layer 识别；`wire_len > captured_len` 或所需字节不足时输出 `kTruncated`，保留原始字节和已验证字段。
3. 字节足够但协议头不合法时输出 `kMalformed`；链路类型暂不支持时输出 `kUnsupportedLinkType`。这两类记录都保留原始字节，单个坏包不得终止当前批次。
4. 采集阶段统一生成 `PacketProtocolInfo{kNotAttempted, 0, 0}`，不得调用 `IPacketProtocolIdentifier`。
5. NPM 仅在 session 采样窗口内调用 `IPacketProtocolIdentifier`；实现必须直接消费 `PacketLayerInfo`，不得再次调用 layer decoder。识别不到协议时输出 `kUnknown`，不覆盖 layer 状态。
6. Arrow 批次写入失败、内存分配失败等内部错误才终止当前批次并向上游报告。

## 主链路

### 采集到标准 packet 流

1. 文件或网卡后端产生 `PacketView`，携带源内序号、链路类型、时间戳和捕获字节。
2. 契约校验通过后建立 `PacketRecord` 的字节所有权；采集解码器调用一次 layer decoder，先缓存完整 layer path，再按 `LayerDecodeOptions` 填充选中端点的 `MAC/IP/Port`。
3. 按固定 Schema 组装 Arrow `RecordBatch`，协议字段保持 `kNotAttempted`，设置 `StreamBatch.ts_ms`，再写入现有 `IStreamChannel`。

### NPM 阶段协议补充

1. NPM 从 packet 行的五元组/源标识维护 session 采样状态，仅选取前若干个 payload 包。
2. 对被选中的包，使用原始字节、已缓存的完整 `layer_ids/layer_offsets` 及选中层索引调用 protocol identifier；适配器只将完整 path 在选中 transport（无 transport 时为选中封装段的最后可识别层）处截成有序前缀，令其成为 NPI `protocol::Layers::Top()`，不得再次调用 `IProtocol::Layer()`；未被选中的包不执行协议识别。
3. 识别结果只写入数值 `protocol_status/protocol_id/protocol_sub_id`，需要展示名称时在查询或字典层解析。

### 截断或异常报文

输入包先后经过元数据校验、分层解码和 Arrow 编码。校验失败的 envelope 被拒绝；截断、畸形或不支持链路类型的有效记录保留原包并带状态进入同一批次，后续算子自行决定是否过滤。

## 测试锚点

新增 `test_packet_contract` 单元测试目标，至少覆盖：

- 固定 Schema 的列名、Arrow 类型、可空性和 Schema 元数据完全匹配；合法 packet 的 `captured_len/raw_data`、时间戳和源内序号保持一致。
- `wire_len == 0`、`wire_len > captured_len`、零长度捕获等边界；非法长度和借用指针跨异步边界时被拒绝。
- `PacketLayerInfo` 直接复用 `common/network/netaddress.h` 的 `EtherAdderss`、`IPv4Address` 和 `IPv6Address`，不得引入重复地址结构；IPv4 的 `addr/dword` 网络字节序、IPv6 的 `bytes[16]` 原样映射，`IpAddress` 的 IPv4/IPv6/`std::monostate` 分支可断言。
- 构造外层 IPv4/UDP/VXLAN/内层 Ethernet/IPv4/TCP 样例时，默认策略选择内层 IP、TCP、内层 Ethernet；切换 `kOutermost` 后选择外层 IP、UDP、外层 Ethernet，同时两种结果的完整 layer path 相同。
- GRE/IP-in-IP 没有对应内层 Ethernet 时，内层 IP/端口仍可提取，但内层 MAC 必须保持无效，不得混用外层 MAC。
- 纯 Ethernet/ARP 等无 IP 样例仍可提取同封装段 MAC；不存在 Ethernet 的链路类型不得伪造 MAC。一个逻辑输出流混用两种 `endpoint_scope` 时应被配置校验拒绝。
- 采集阶段不调用 protocol identifier；NPM 阶段使用同一 layer path 识别协议且不再次调用 layer decoder，未采样包保持 `kNotAttempted`。
- NPM 适配器传给 NPI 的 `Layers::Top()` 必须是选中的 transport 层（或没有 transport 时的最后可识别层），并通过调用计数测试证明没有再次执行 `IProtocol::Layer()`。
- layer path 快照转换与重建只产生固定大小的内存读写；测试应验证采集每包一次快照、NPM 仅对采样包重建，并覆盖超过 NPI `uint16_t` 偏移范围时不截断、不调用 `Identify()`。
- 截断、畸形头和不支持链路类型分别得到对应 `LayerStatus`；未知协议得到 `ProtocolStatus::kUnknown`，原始字节保留，后续记录仍能进入同一批次。
- 多源序号只在 `source_id` 内递增；`StreamBatch.ts_ms` 等于批内最大 packet 事件时间毫秒值，不能覆盖行内 `timestamp_ns`。
- `PacketRecord` 持有 owner 时，释放采集端临时缓冲后原始字节仍可读；Arrow 编码不得产生越界读取，固定二进制地址/MAC 列不得发生文本转换。

## 原子任务

- `[x]` 冻结 `ipacket.h` 中的两阶段 packet 结构、layer path、字段掩码、二进制地址和状态契约。
- `[x]` 冻结隧道 layer path 与端点选择策略，默认最内层并支持最外层配置，明确选中层索引和 MAC 同封装段约束。
- `[x]` 明确输出流的 endpoint scope 一致性和 NPI protocol view 顶层映射，记录隧道标识留待 session Feature 的边界。
- `[x]` 固化 NPI `protocol::Layers` 到独立 layer path 快照的单次转换、采样包重建和 `uint16_t` 可表示性校验。
- `[x]` 完成现有 `netaddress.h` 地址类型适配，冻结 `MacAddress` 有效位、IPv4/IPv6 variant 和 Arrow 字节序映射。
- `[x]` 实现 packet envelope 校验、所有权转换和固定 Arrow Schema。
- `[x]` 实现固定 Arrow RecordBatch 编码与 packet 行字段映射。
- `[x]` 实现 NPI layer decoder 适配，完成 layer path、MAC/IP/Port 和 payload offset 映射。
- `[x]` 实现 NPI protocol identifier 适配，直接消费缓存 layer path，禁止重复 layer 解析。
- `[x]` 完成 `test_packet_contract` 的阶段调用次数、二进制字段、生命周期、截断、畸形和未知协议测试。
