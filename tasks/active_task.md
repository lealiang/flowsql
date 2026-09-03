# Active Task

Feature：`npm-packet-contract`
原子任务：完成 `test_packet_contract` 的阶段调用次数、二进制字段、生命周期和异常语义测试
状态：已完成（WIP 清空）

## 边界

- 已覆盖 GRE/IP-in-IP 无内层 Ethernet、纯 Ethernet/ARP、空链路和 scope 一致性边界。
- 已覆盖采集阶段仅调用 layer decoder、NPM 阶段仅对选定包调用 protocol identifier 的调用计数。
- 已覆盖截断、畸形、未知协议、owner 生命周期和 Arrow nullable 二进制字段。
- 保持现有 `IStreamChannel`、`IDataFrame` 和 `IProtocol` ABI 不变。

## 验收

- 已验证 GRE/IP-in-IP 不继承跨封装段 MAC；纯 Ethernet/ARP 仅提取同段 MAC。
- 已验证采集每包只执行一次 layer 解码；protocol identifier 不调用 layer 解码。
- 已验证 mock NPI 仅收到一次 `Identify()` 调用且 `Layer()` 调用计数保持为 0。
- 已验证选中 transport layer 成为 `Layers::Top()`，完整 path 和 payload 在重建后保持一致。
- 已验证 offset 或 payload 超过 `uint16_t` 可表示范围时不截断、不调用 NPI，并返回 `kUnknown`。
- `git diff --check`、标准 CMake 编译和 `test_packet_contract` 已通过。
