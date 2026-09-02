# Sprint 22 规划

## Sprint 信息

- **Sprint**：Sprint 22
- **规划更新**：2026-07-25
- **开始日期**：待确认（原草稿日期为 2026-06-07，当前尚未开工）
- **预计工作量**：17.9 PD，评估区间 17-21 PD
- **Sprint 目标**：完成完整 Story 19.1 和完整 Story 19.2，建立可构造、可轮询、可处理、可释放的 packet block 进程内闭环，并交付可复用的 checked layer hints。
- **设计文档**：[design.md](design.md)，已按 PB 修订范围，待复审
- **关联待办**：`tasks/product_backlog.md` 的 Epic 19 / Story 19.1 / Story 19.2
- **状态**：💡 设计修订中，待确认

设计文档中的验收 ID 是本 Sprint 的稳定追踪键，格式为 `S19.1-Axx` 或 `S19.2-Axx`。本文件中的 Task 必须引用这些 ID；代码测试、评审记录和 Sprint Review 也必须引用相同 ID。

## 1. 迭代边界

### 1.1 本迭代范围

1. 通用 `IBlockPayload`、`BlockPayloadKind`、schema 判型，以及 Arrow wrapper。
2. 通用 `IBlockStreamChannel` / `IBlockStreamOperator`，包括 `OutputSchema()`、`BlockLease` 和 `ReleaseBlock(BlockLease&&)`。
3. `packet.v1` 逻辑 schema、`PacketDescriptor` AoS、`PacketBatchPayload` owning、`PacketBatchView` non-owning。
4. `logical_entity_id`、实体内唯一 `source_id`、实体级唯一 `packet_id` 和固定版本 source registry。
5. 批级强类型 sidecar、`PacketLayerHints`、present/absent 语义和 build-then-publish。
6. 直接复用公共 `eLayer` / `protocol::Layers` 和现有 NPI parser/dispatch，增加 checked `LayerHints()` façade、必要终态传播和 checked helper。
7. 插件加载器按完整插件集执行“全部注册 -> 全部 `Option()` -> 全部 `Load()` -> `Concurrency(N)` 配置 -> 按依赖顺序全部 `Start()`”，NPI 在 packet worker consumer 之前按冻结并发度完成 Hyperscan scratch 初始化。
8. Story 19.2 PB 最低契约样本闭环：验证 Ethernet + IPv4 + TCP、VLAN + IPv4 + UDP、IPv6 + TCP、截断/畸形包和一种固定 VXLAN 隧道样本，不重新盘点或认证完整协议能力。
9. façade 参数/输出 invariant、终态传播、未知 LINKTYPE、layer limit、pipeno 并发和 owner queue 释放测试。
10. 构造型 VXLAN 隧道样本、CTest 注册、ASan/UBSan/TSan 入口和 AoS+sidecar 性能基准。

### 1.2 本迭代不包含

1. pcap / pcapng 文件读取、多文件归并和 `pcap_replay`。
2. DPDK、AF_XDP、PF_RING、netmap 等实时采集通道实现；本 Sprint 只验证 adapter 输入契约可复用。
3. NPM flow/session 聚合、TCP 重组和 HTTP / TLS / DNS 等应用层识别。
4. `packet_filter.v1` predicate、SQL `WHERE` AST 和过滤下推执行；只交付 filter 所需的 checked hints 输入。
5. Scheduler 的 block stream 生产装配；当前 `BLOCK_STREAM_NOT_IMPLEMENTED` 门禁保留。
6. 通用字符串键值 packet context、per-packet 多态对象和跨版本持久化 `PacketLayerHints` 二进制格式。

### 1.3 依赖与前置条件

- 现有 `IBlockStreamChannel` / `IBlockStreamOperator` 是无实现的 Arrow 专用占位，可以整体替换 ABI。
- NPI 通过 `IID_PROTOCOL` 提供基础流量分析能力；`LayerHints()` 不调用 `Identify()`，不加载或扫描应用层规则。
- `protocol::Layers` 现有布局为 64 字节；`eLayer` 和 layer 数值必须保持不变。
- 当前注册宏会在 `pluginregist()` 内调用并忽略 `Option()` 结果，loader 又在每个 `.so` 注册后立即调用其 `Load()`；T0 必须改为单初始化批次并保留明确的失败回滚。
- 当前 NPI `Load()` 会间接调用 `Engine::Ready()`，而 `Concurrency(N)` 只能在 loader 返回后设置；T5 必须将建模与 scratch finalization 分离，保证并发度先于 `Ready()` 冻结。
- CMake 当前未将 `test_framework` 和 `test_npi` 都注册为有效 CTest 验收目标，测试任务必须闭合注册问题。

## 2. Story 验收标准

### 2.1 Story 19.1：通用 Block Payload / PacketBatchView / packet.v1

| ID | 验收标准 | 设计章节 | 主要测试 |
| --- | --- | --- | --- |
| S19.1-A01 | 通用 payload 类型、schema 判型和 no-IID 规则成立 | T1 §4.3-4.5 | `test_packet_data_plane` payload |
| S19.1-A02 | Channel/Operator 泛化，`OutputSchema()` 在首块前可读，Arrow wrapper 无回退 | T1 §4.6-4.8 | `test_packet_data_plane`、`test_stream` |
| S19.1-A03 | `packet.v1` 字段、编码、版本和兼容策略闭合 | T4 §7.3-7.4 | schema/Arrow metadata 测试 |
| S19.1-A04 | logical entity、source registry、实体级 packet allocator 闭合 | T3 §6.3-6.5 | 多 block/多 source identity 测试 |
| S19.1-A05 | Descriptor、buffer range、View/Payload 所有权边界闭合 | T3 §6.6 | descriptor/buffer range 测试 |
| S19.1-A06 | 强类型 sidecar 对齐、PImpl、present/absent、发布后只读成立 | T3 §6.6.2、T7 §10.3-10.7 | sidecar 测试 |
| S19.1-A07 | lease move、exactly-once、异常、Cancel、channel 先析构、owner queue 成立 | T2 §5.3-5.4 | 生命周期和跨线程释放测试 |
| S19.1-A08 | 构造型 source → channel → operator → release 闭环成立，Scheduler 门禁保持 | T2 §5.1、T8 §11.3-11.7 | 构造型 block source 测试 |
| S19.1-A09 | 插件批次生命周期、CTest、ABI、并发、sanitizer 和既有 Arrow 回归可执行 | T0 §3.3-3.7、T8 §11.4-11.7、T9 §12.3-12.5 | loader lifecycle / CTest / Sanitizer |

### 2.2 Story 19.2：PacketLayerHints 与 packet 上下文

| ID | 验收标准 | 设计章节 | 主要测试 |
| --- | --- | --- | --- |
| S19.2-A01 | 直接复用 `eLayer` / `protocol::Layers`，布局、checked helper 和 NPI 输出 invariant 收口 | T5 §8.3、T6 §9.5-9.6 | Layers ABI/helper + output invariant 测试 |
| S19.2-A02 | 单一终态 `PacketParseStatus` 和字段缺失语义闭合 | T5 §8.3-8.5、T7 §10.3 | status/absent 测试 |
| S19.2-A03 | 最内层 L2/L3/L4、IANA L4、IP version、VLAN/tunnel depth 映射正确 | T5 §8.4、T6 §9.5 | 基础链/隧道映射断言 |
| S19.2-A04 | NPI `LayerHints(pipeno, packet, cap_len, link_type, hints)` façade 成立，核心只收三类 packet 输入 | T5 §8.6-8.7 | NPI interface 测试 |
| S19.2-A05 | façade 参数校验、canonical 初始化、接口错误码/packet 终态分离和 Layers 输出 invariant 成立 | T6 §9.3-9.6 | façade/error/invariant 测试 |
| S19.2-A06 | 现有 NPI parser/dispatch 作为可信依赖直接复用且不做协议语义改造，PB 固定合法样本与旧 `Layer()` 层序列一致 | T6 §9.3、T6 §9.6 | 固定契约 corpus + 旧 Layer 对照 |
| S19.2-A07 | 固定 VXLAN 样本的完整链、最内层 offset、`tunnel_depth=1` 成立 | T8 §11.3-11.7 | `test_npi_layer` tunnel |
| S19.2-A08 | 必要 NPI 终态可传播为截断、畸形、unknown LINKTYPE 和 layer limit；旧 `Layer()` ABI/签名及 parser/dispatch 保持，仓库已有回归通过 | T6 §9.4-9.6、T8 §11.5-11.7 | façade contract/sanitizer + 旧 Layer 已有回归 |
| S19.2-A09 | sidecar 原位写入、零分配/零锁和性能回归门槛成立 | T7 §10.3-10.7、T9 §12.3-12.5 | benchmark + allocation counter |
| S19.2-A10 | 批次启动顺序、`Concurrency(N)` 先于 NPI `Start()/Ready()` 冻结、pipeno 亲和和不同 pipeno 并发无串扰成立 | T0 §3.3-3.7、T5 §8.6-8.8、T8 §11.5-11.7、T9 §12.3-12.5 | loader lifecycle / NPI startup / TSan |
| S19.2-A11 | NPM / `packet_filter.v1` 可通过 checked accessor 使用 hints，缺失时不重复解析 | T5 §8.4、T7 §10.3-10.7 | accessor/status tests |

## 3. Task 分解与追踪矩阵

### 3.1 Task 总表

| Task | 状态 | 内容 | 设计章节 | 验收 ID | 主要文件 | 测试目标 | 估算（PD） | 依赖 |
| --- | --- | --- | --- | --- | --- | --- | ---: | --- |
| T0 | [ ] | 插件批次生命周期、设计/ABI 基线和公共头归属 | T0 §3 | S19.1-A09、S19.2-A10；支撑 S19.2-A01 | `src/common/iplugin.h`、`loader.hpp`、`src/app/main.cpp`、现有 `IPlugin::Start()` overrides、`src/tests/test_framework/`、Sprint 文档、`src/CMakeLists.txt` | loader 顺序/回滚、Start 失败原子性审计/定向测试及 CTest 注册、ABI/Scheduler 基线 | 2.5 | - |
| T1 | [ ] | 通用 payload、schema、Channel/Operator ABI 和 Arrow wrapper | T1 §4 | S19.1-A01、S19.1-A02 | `src/framework/interfaces/iblock_payload.h`、`iblock_stream_channel.h`、`iblock_stream_operator.h`、`src/framework/core/arrow_block_payload.*` | `test_packet_data_plane` checked accessor/schema、`test_stream` | 1.5 | T0 |
| T2 | [ ] | `BlockLease`、LeaseState、exactly-once reclaimer、Cancel/Close 和 owner queue | T2 §5 | S19.1-A07、S19.1-A08 | `src/framework/interfaces/iblock_stream_channel.h`、`src/framework/core/block_lease.*`、构造型 block source | `test_packet_data_plane` 生命周期/异常 | 2.0 | T1 |
| T3 | [ ] | Descriptor、buffer range、logical entity/source registry、allocator、View/Payload 和 context PImpl | T3 §6 | S19.1-A04、S19.1-A05、S19.1-A06 | `src/framework/core/packet_batch.*`、`packet_context.*` | `test_packet_data_plane` packet/context | 2.0 | T1、T2 |
| T4 | [ ] | `packet.v1` schema、Arrow metadata 和兼容检查 | T4 §7 | S19.1-A03 | `src/framework/core/packet_batch.*`、`arrow_block_payload.*` | schema/Arrow tests（major、低 minor、高 minor、sidecar capability） | 1.0 | T3 |
| T5 | [ ] | 公共 Layers/Hints、NPI façade、pipeno 和分阶段初始化 | T5 §8 | S19.2-A01、S19.2-A02、S19.2-A04、S19.2-A10 | `src/common/network/layers.h`、`packet_layer_hints.h`、`src/plugins/npi/iprotocol.h`、`npi.*`、`engine.*`、`model.*`、`regexmatch.*` | `test_npi_layer` API/helper/startup/Ready 回滚 | 2.0 | T0 |
| T6 | [ ] | NPI `LayerHints()` checked façade、终态传播与映射适配 | T6 §9 | S19.2-A03、S19.2-A05、S19.2-A06、S19.2-A08 | `src/plugins/npi/layer.*`、`npi.*` | `test_npi_layer` façade/contract corpus | 1.3 | T5 |
| T7 | [ ] | 批级 sidecar 预分配、原位写入、发布同步和 hints checked accessor | T7 §10 | S19.1-A06、S19.2-A09、S19.2-A11 | `src/framework/core/packet_batch.*`、`packet_context.*`、NPI adapter integration | `test_packet_data_plane` sidecar/accessor 用例（含 absent 不重复解析） | 1.2 | T3、T6 |
| T8 | [ ] | 构造型 packet/block、完整单元测试、CTest 注册和回归路径 | T8 §11 | S19.1-A08、S19.1-A09、S19.2-A07、S19.2-A08、S19.2-A10 | `src/tests/test_packet_data_plane/`、`test_npi_layer/`、`test_framework/CMakeLists.txt`、`test_stream/`、`test_scheduler_e2e/` | `test_packet_data_plane`、`test_npi_layer`、`test_framework`、`test_stream`、`test_scheduler_e2e` | 2.5 | T2、T4、T7 |
| T9 | [ ] | ASan/UBSan/TSan、ABI static_assert、性能基准和 15% 回归门槛 | T9 §12 | S19.1-A09、S19.2-A09、S19.2-A10 | 独立 sanitizer 构建目录、`src/tests/test_packet_data_plane/benchmark_packet_data_plane.cpp`、相关 headers | `benchmark_packet_data_plane`、ASan/UBSan 下的 `test_framework` / `test_packet_data_plane` / `test_npi_layer`、TSan 并发测试 | 1.3 | T6、T7、T8 |
| T10 | [ ] | 验收证据、文档同步和 Sprint 收口 | T10 §13 | 无独立验收 ID；仅汇总 T0-T9 证据 | `planning.md`、`design.md`、必要 Backlog 状态 | CTest 输出、验收 ID 与文档映射审查 | 0.6 | T9 |

**合计：17.9 PD。** T0、T2、T8、T9 是主要风险项；T6 只做现有 NPI parser/dispatch 上的 checked façade、必要终态传播和映射，不承担 parser 重建或协议认证。若 loader/Start 失败回滚、owner queue、终态传播机械改动或自动化验证需要额外工作，必须更新本表估算，不得把未完成验收留在“风险缓冲”中。

### 3.2 Task 完成出口

每个 Task 完成时，必须同时满足以下条件：

1. 对应设计章节没有未决术语或参数；设计变更先更新 `design.md`。
2. 对应验收 ID 至少有一个代码落点和一个可执行测试落点。
3. 有自动化测试责任的 Task 已把目标接入 CTest；耗时性能基准可以独立运行，但必须有非零失败退出和可复现命令。
4. 任务完成后在本表勾选 `[x]`，并在 Story 验收表同步状态。
5. 若发现依赖、文件或验收 ID 不匹配，先修订规划，再继续实现。

### 3.3 双向追踪规则

- `planning.md` 是 Task ID、名称、依赖、估算和状态的唯一来源；`design.md` 以同 ID、同名称、同顺序的 Task 章节承载实现和验证设计，文档结构本身就是双向映射。
- 跨 Task 的底层契约统一放在 `design.md` 第 2 章，并标明 owner/consumer；不再维护“技术章节 -> Task”的补偿性反向映射表。
- 每个验收 ID 必须由 T0-T9 中至少一个实现 Task 显式引用；T10 的证据汇总不能替代实现覆盖。
- Task 增删、拆分、合并、重命名或重编号时，两份文档必须在同一次变更中更新；设计新增契约时，必须在本表分配 owner Task、验收 ID 和测试目标。

## 4. 实施顺序与里程碑

T0 先完成插件批次生命周期和设计/ABI 基线；随后 T5 可与 T1/T2 主线并行。T3 在 T2 后开始，并可与 T5/T6 并行。T4 依赖 T3，T6 依赖 T5，其余按 Task 表依赖推进。

| 里程碑 | 任务 | 进入条件 | 退出证据 |
| --- | --- | --- | --- |
| M0 生命周期/设计基线 | T0 | 本文与 design.md 已同步 | loader 完整批次 R/O/L/C/S 顺序与失败回滚可测，验收 ID、文件落点和依赖图无孤立项 |
| M1 通用 block | T1、T2 | ABI 方案冻结 | Packet/Arrow payload 可轮询、可处理、可 exactly-once release |
| M2 packet view | T3、T4 | M1 完成 | `packet.v1`、entity/source identity、buffer range 和 View 可构造 |
| M3 LayerHints façade | T5、T6 | 公共 Layers 归属冻结 | checked façade、终态传播、映射和 PB 固定契约 corpus 通过 |
| M4 sidecar 闭环 | T7 | T3、T6 完成 | sidecar 原位填充、发布只读、NPM/filter checked accessor 可读 |
| M5 自动化验收 | T8、T9 | M1-M4 完成 | CTest、ASan/UBSan/TSan、性能回归预算通过 |
| M6 Sprint 收口 | T10 | M5 完成 | 所有验收 ID 有证据，文档和 Backlog 状态一致 |

## 5. 测试与验证计划

### 5.1 常规验证

T8 负责常规构建、CTest 注册和生产路径回归，验证目标为 `test_packet_data_plane`、`test_npi_layer`、`test_framework`、`test_stream` 和 `test_scheduler_e2e`。可执行命令、失败条件和完成出口以 `design.md` T8 §11.6-11.7 为准，planning 不复制另一份命令基线。

### 5.2 必测分组

- `test_packet_data_plane`：S19.1-A01 到 S19.1-A08，以及 S19.2-A11；覆盖 payload/schema、identity、buffer range、sidecar、lease、owner queue、Cancel、构造型 operator、checked accessor 和 absent sidecar 不重复解析。
- `test_npi_layer`：S19.2-A01 到 S19.2-A08 及 S19.2-A10，覆盖 NPI 空/畸形 Option、`Load()` 不创建 scratch、`Concurrency(N)` 先于 `Start()/Ready()` 冻结、Ready 部分失败原子回滚、`LayerHints()` 的 façade 参数/输出 invariant、Ethernet + IPv4 + TCP、VLAN + IPv4 + UDP、IPv6 + TCP、固定 VXLAN、代表性截断/畸形终态、offset、LINKTYPE 和无应用识别；旧 `Layer()` 保持 ABI/签名及 parser/dispatch，运行仓库已有可执行回归，并只在 PB 固定合法样本上与 `LayerHints()` 比较层序列。不为其他协议新增认证 corpus。
- `test_scheduler_e2e`：S19.1-A08，覆盖 `block_stream` 生产装配仍返回 `BLOCK_STREAM_NOT_IMPLEMENTED`。
- `test_framework` / `test_stream`：S19.1-A02、S19.1-A09，覆盖全部注册先于全部 `Option()`、全部 `Option()` 先于全部 `Load()`、L/S 间装配窗口、全部 `Load()` 先于全部 `Start()`、失败回滚、既有 Arrow/Stream 回归和 CTest 注册。
- TSan：S19.1-A07、S19.2-A10，只测试已完成批次启动且并发度冻结后的合法 range partition、跨线程 lease 归还和不同 pipeno 并发；禁止制造同 slot data race。
- ASan/UBSan：S19.1-A09、S19.2-A05、S19.2-A08，运行 `test_framework` 的 loader 生命周期/handle 回滚，以及 PB 固定契约 corpus 的截断、畸形、长度溢出和非法入口样本，验证 façade 与现有 NPI 的集成边界。
- 性能：S19.2-A09，固定 PB 合法契约 corpus、batch size 64/512/4096、预热后 10 次中位数；`LayerHints()` 相对现有 `Layer()` 的 cycles/packet 回退不得超过 15%，且每包零 heap allocation、零锁。该对照量化 checked façade、终态和派生字段的增量成本。

### 5.3 可执行的性能与 Sanitizer 验证

T9 负责以下独立验证路径：

- Release `benchmark_packet_data_plane`：固定 batch size 64/512/4096 和 10 次中位数，覆盖 3 种 sidecar 访问模式。
- ASan/UBSan：执行 `test_framework` loader 生命周期/handle 回滚，以及 packet 数据面和 `LayerHints()` 固定契约 corpus 的截断、畸形、长度溢出用例。
- TSan：执行合法 range partition、跨线程 lease 归还和不同 `pipeno` 并发。
- ABI：编译期检查 public layout，并验证旧 IID 被拒绝。

独立构建目录、完整命令、15% 回归预算、零分配/零锁条件和非零失败规则以 `design.md` T9 §12.3-12.5 为准。

### 5.4 验收证据格式

T10 按 `design.md` T10 §13.3 定义的统一格式汇总 `acceptance_id`、实现位置、测试用例、可复现命令、结果和证据路径。T10 只整理 T0-T9 已产生的证据，不新增或替代功能验收责任。

## 6. 风险与缓解

| 风险 | 影响 | 缓解 | 关联 Task/验收 |
| --- | --- | --- | --- |
| loader 仍按单插件执行 Option/Load，或 NPI 在并发度冻结前创建 scratch | 高 | T0 改为单初始化批次并传播阶段错误；T5 将 `Ready()` 移到 `Start()`，在 `Load()` 后、`StartAll()` 前冻结 `Concurrency(N)`；T8 验证顺序和回滚 | T0/T5/T8/T9，S19.1-A09、S19.2-A10 |
| 失败的 `Start()` 遗留自身线程或部分 Hyperscan 资源 | 高 | IPlugin 冻结失败原子性；NPI Ready 失败释放本次 database/scratch；fixture 和故障注入验证失败后 stopped/empty | T0/T5/T8/T9，S19.1-A09、S19.2-A10 |
| Block ABI 泛化影响旧 Arrow 占位接口 | 高 | 占位接口整体重编，更新 IID，保留 Arrow wrapper 和 `test_stream` 回归 | T1/T2/T8，S19.1-A02、S19.1-A09 |
| lease 跨线程回收不满足 AF_XDP/DPDK owner lcore | 高 | LeaseState exactly-once，线程安全 reclaimer 或 owner queue，TSan 测试 | T2/T8/T9，S19.1-A07 |
| entity 内 packet ID 被多 source/重启复用 | 高 | entity-level allocator，区间预留，持久 high-water mark 或新 entity epoch | T3/T8，S19.1-A04 |
| `LayerHints()` 参数、终态或 Layers 输出 invariant 传播错误 | 高 | canonical 初始化、接口错误码/packet 终态分离、layercount/offset/payload checked validation 和固定契约 corpus | T5/T6/T8/T9，S19.2-A01、S19.2-A05、S19.2-A08 |
| checked façade 改动影响旧 `Layer()` 兼容路径 | 高 | 复用同一 parser map，但不替换旧 `Layer()` 入口和返回语义；仓库已有回归保持通过，仅在 PB 固定合法样本做层序列一致性 | T6/T8/T9，S19.2-A06、S19.2-A08 |
| NPI 可信依赖与 façade 责任混淆 | 中 | 明确协议覆盖/语义归 NPI，Sprint 只补 façade、必要终态和上下文映射；禁止借机重构或逐协议认证 | T5/T6/T8，S19.2-A05、S19.2-A06 |
| sidecar 隔离造成单 packet cache 损耗 | 中 | 连续同 index、原位写、三种访问模式基准，15% 回归门槛 | T7/T9，S19.2-A09 |
| CTest 目标未实际执行 | 高 | 新增目标并在 CMake 注册，验收命令使用精确测试名 | T8/T10，S19.1-A09 |
| Scheduler 误被宣称可运行 | 中 | 保留 `BLOCK_STREAM_NOT_IMPLEMENTED`，只验收进程内构造闭环 | T2/T8，S19.1-A08 |

## 7. 交付物与后续衔接

### 7.1 Sprint 22 交付物

1. 通用 block payload / channel / operator ABI 和 Arrow wrapper。
2. `packet.v1`、`PacketDescriptor`、`PacketBatchPayload`、`PacketBatchView`、entity/source identity 和 buffer/lease 生命周期实现。
3. 插件完整批次 `Option/Load/Start` 生命周期，以及 NPI `Concurrency -> Ready` 的确定初始化顺序。
4. `PacketLayerHints`、checked NPI `LayerHints()` façade、必要终态传播、PB 固定契约样本和固定 VXLAN 样本。
5. `test_packet_data_plane`、`test_npi_layer`、CTest 注册、Sanitizer/TSan 入口和性能基准。
6. 设计、规划、测试证据和 Backlog 状态同步。

### 7.2 Sprint 22 外

- Sprint 23：`pcapfile` 单文件 block source，输出带 hints 的 `PacketBatchView`。
- 后续 pcapfile Story：多文件顺序读取、时间戳归并和 replay 收口；具体 Sprint 编号以 Sprint 索引为准。
- Epic 22：在 packet context 契约稳定后接入 `packet_filter.v1` 执行和过滤下推。
- Epic 20：复用 hints 进行 NPM Core、flow/session 和应用层算子实现。

## 8. Sprint 收口规则

Sprint 22 不能因“接口能编译”而验收。只有当全部 `S19.1-A01..A09` 和 `S19.2-A01..A11` 都有实现、测试和证据，且 M6 文档同步完成，Stories 才能标记完成。
