# Active Task

Feature：`npm-offline-import`
原子任务：T2.8 增量 reader 与资源错误收敛
状态：已完成

## 业务意图

- 让 `PcapFileReader::Open()` 只读取容器头，并通过持久文件句柄在 `Next()` 中增量读取记录或 block。
- 消除文件大小直接决定 `Open()` 内存占用的问题，并让晚到的截断、读取失败和 reader 分配失败稳定进入 source error 终态。

## Non-Goals

- 不修改公共 block stream ABI、option parser、回放、layer decode 或 Scheduler 路由。
- 不增加未在规格中定义的文件大小、packet 大小或 batch 字节数硬限制；单包 owning bytes 和 batch owner 语义保持不变。
- 不改 manager/factory 行为，也不处理 T1、T4 或 T5 的纠偏问题。
- 不修改 Scheduler block source 执行路径或多 provider 路由。

## 边界

- classic pcap 的 global header 在 `Open()` 读取，record header 和 packet bytes 在 `Next()` 按需读取。
- pcapng 的首个 SHB 在 `Open()` 读取，后续 block 在 `Next()` 逐块读取；不得保留整文件副本。
- 在按声明长度分配 block/packet 内存前，先以文件剩余字节数拒绝不可能满足的长度，避免恶意长度触发大分配。
- reader 内的 `std::bad_alloc`/`std::length_error` 转换为明确错误码；channel 仍保持一次 `kError`、后续 `kCancelled`。
- 保持 T2.1～T2.7 已有格式、字段、section、source/sequence、EOF 和错误语义不变。

## 允许修改的文件

- `tasks/active_task.md`：更新本任务状态、验收证据和停止结果。
- `tasks/specs/feat-npm-offline-import.md`：仅新增并勾选 T2.8，以及在全部验收通过后收敛 T2/T3 父任务状态。
- `src/channels/pcapfile/pcap_file_channel.cpp`：仅将 reader 改为持久句柄增量读取并收敛 reader 资源错误。
- `src/tests/test_pcapfile_import/test_pcapfile_import.cpp`：仅补增量读取、晚到截断和声明长度不触发大分配的回归断言。

## 验收

- classic pcap 和 pcapng 在 `Open()` 后截断尚未读取的数据时，首次 `PollBlock()` 产生 `kError`，后续为 `kCancelled`；不得从 `Open()` 的缓存继续发布旧数据。
- 声明长度远大于实际剩余文件时直接产生 source error，不先按声明长度分配内存。
- 既有 classic pcap、pcapng、多 section、replay、cancel、owner 和错误终态测试保持通过。
- 定向命令：`cmake --build build --target test_pcapfile_import -j2`，随后运行 `build/output/test_pcapfile_import`。
- 完成后执行 `git diff --check`；不运行 T4/T5 的 Scheduler 或 Feature 全量回归。

## 时间盒与停止条件

- 时间盒：30 分钟。
- 仅在允许文件内完成 T2.8 测试、最小实现和定向验收；发现其他任务问题只记录，不扩展范围。
- 到达验收条件后立即勾选 T2.8、记录验收证据并停止；若当前错误无法在边界内修复，则记录复现依据后停止。

## 验收证据

- 测试先行红灯：classic pcap 在 `Open()` 后截断到 global header，旧实现仍从整文件缓存发布 data，新增断言失败。
- `PcapFileReader` 现仅在 `Open()` 读取 classic global header 或首个 pcapng SHB；record、后续 block、options 和 packet bytes 均通过持久文件句柄顺序读取。
- classic packet 与 pcapng block 在按声明长度分配前先核对文件剩余长度；reader 内的 `std::bad_alloc`/`std::length_error` 分别收敛为 `ENOMEM`/`EOVERFLOW`。
- `cmake --build build --target test_pcapfile_import -j2`：通过。
- `build/output/test_pcapfile_import`：通过，输出 `[PASS] pcapfile import`；覆盖 Open 后 classic/pcapng 截断、超大声明 packet/block、原有格式解析、回放、cancel、背压、错误终态和 owner 生命周期。
- `git diff --check`：通过。
- 停止结果：T2.8 及其作为 T3 的完成前置已通过，T2/T3 父任务收敛为完成；未处理 T1、T4 或 T5 问题。
