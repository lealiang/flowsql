# Active Task

Feature：`npm-offline-import`
原子任务：T2.7 修正 pcapng section 结构处理
状态：已完成

## 业务意图

- 让每个 pcapng section 都按有限循环完成 SHB 结构和 options 校验，并在进入下一 section 或文件结束前确认已声明 IDB。
- 防止连续合法 section 通过递归消耗调用栈，同时保证 section 结构错误进入 source error 而不是 EOF。

## Non-Goals

- 不修改公共 block stream ABI、通道/plugin 生命周期、option parser、回放或 layer decode。
- 不在本切片把 reader 改为持久文件句柄或有界增量读取；该资源模型修正后续单独拆分。
- 不修改 Scheduler block source 执行路径或多 provider 路由。
- 不提前处理 T1 测试补强、T4 或 T5。

## 边界

- 首个及后续 SHB 都校验 byte-order magic、version、首尾 block length 和固定字段后的 options。
- SHB option 的 header、length、padding、terminator 任一损坏均返回 source error。
- 每个 section 在遇到下一 SHB 或文件结束前必须至少声明一个 IDB；缺少 IDB 时返回 source error，不得产生 EOF。
- 后续 SHB 在现有 `NextPcapng()` 循环内继续处理，禁止递归调用；多 section 的全文件 `source_id` 仍按 IDB 遇到顺序递增。
- 保持结构合法的 SHB + IDB 空 section 可以正常结束；不要求每个 section 必须含 EPB。

## 允许修改的文件

- `tasks/active_task.md`：更新本任务状态、验收证据和停止结果。
- `tasks/specs/feat-npm-offline-import.md`：仅新增并勾选 T2.7 或记录本任务阻塞，不改其他任务状态。
- `src/channels/pcapfile/pcap_file_channel.cpp`：仅修正 T2.7 的 SHB/section 解析和错误返回。
- `src/tests/test_pcapfile_import/test_pcapfile_import.cpp`：仅补 T2.7 的 section 结构与非递归回归断言。

## 验收

- 首个和后续 SHB 的损坏 option 均产生一次 `kError`，后续为 `kCancelled`。
- SHB-only 文件、前一 section 无 IDB、末尾 section 无 IDB 均产生 source error，不产生 EOF。
- 大量连续合法 section 通过循环解析并读取最后一个 EPB，不产生递归栈增长；最终 source/sequence 保持正确。
- SHB + IDB 且无 EPB 的空 section 仍正常产生 EOF。
- 定向命令：`cmake --build build --target test_pcapfile_import -j2`，随后运行 `build/output/test_pcapfile_import`。
- 完成后执行 `git diff --check`；不运行 Feature 全量 CTest 或无关回归。

## 时间盒与停止条件

- 时间盒：30 分钟。
- 仅在允许文件内完成 T2.7 测试、最小实现和定向验收；发现其他任务问题只记录，不扩展范围。
- 到达验收条件后立即勾选 T2.7、记录验收证据并停止；若当前错误无法在边界内修复，则记录复现依据后停止。

## 验收证据

- 测试先行红灯：新增 SHB-only 用例后，旧实现断言失败，确认其错误地产生 EOF 而非 source error。
- `cmake --build build --target test_pcapfile_import -j2`：通过。
- `build/output/test_pcapfile_import`：通过，输出 `[PASS] pcapfile import`；覆盖缺少 IDB、合法空 section、首个/后续 SHB option 损坏、错误终态以及 32768 个 section 的迭代解析和 source/sequence 断言。
- `git diff --check`：通过。
- 停止结果：T2.7 已达到验收条件；未进入有界增量 reader、Scheduler 路由或 T1 测试补强。
