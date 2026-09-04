# Active Task

Feature：`npm-offline-import`
原子任务：T4.5 block 执行终态与 batch 快照传播
状态：已完成

## 业务意图

- 让 block operator 的成功、主动停止、source 取消、异常和 operator 错误形成明确内部终态。
- 将 completed/stopped/cancelled 结果写入 `HandleExecute()` JSON，并让现有 `SchedulerBatchRuntime` 写入对应任务快照。
- 让 operator 异常和错误稳定进入 failed 快照，不能在 release 后继续轮询并误报成功。

## Non-Goals

- 不修改 T4.1～T4.4 的 provider/manager 路由、schema 门禁、poll 事件或 exactly-once release 契约。
- 不修改 pcapfile provider、reader、replay、背压实现、公共 block ABI 或公共 HTTP 接口。
- 不重构 `SchedulerBatchRuntime::ExecuteSqlFn`，不为运行中的同步 SQL 新增抢占式 RequestStop 取消令牌；本任务的 cancelled 来自 source `kCancelled` 事件。
- 不支持多 block source、多 block operator 或 Arrow/block 混合链路，不开始 T5 全量回归。

## 边界

- `ExecuteBlockOperator()` 使用私有内部终态：EOF + Flush 成功为 completed，`ProcessBlock()==1` + Flush 成功为 stopped，source `kCancelled` 为 cancelled，其余非零/异常为 failed。
- `ProcessBlock()` 返回 0 才继续，1 为主动停止，负数或大于 1 的非法正数均为 failed；抛异常转换为 `EFAULT` failed。
- data batch 无论 operator 成功、主动停止、返回错误或抛异常，仍先执行 T4.4 冻结的 exactly-once `ReleaseBlock()`。
- block route 的成功 JSON 使用 `status=completed|stopped|cancelled`；failed 继续使用现有 `OP_EXEC_FAIL`、`error_stage=execute` 错误 JSON。
- batch runtime 对 successful execute JSON 中的 stopped/cancelled 立即形成对应终态；completed 保持现有完成路径，failed 保持现有错误解析路径。

## 允许修改的文件

- `tasks/active_task.md`：冻结 T4.5 边界并记录状态和验收证据。
- `tasks/specs/feat-npm-offline-import.md`：仅在验收通过后勾选 T4.5，并在 T4.1～T4.5 均完成时勾选 T4 父任务。
- `src/services/scheduler/scheduler_plugin.h`：仅增加私有 block 执行终态及 helper 输出参数。
- `src/services/scheduler/scheduler_stream_executor.cpp`：仅生成 block completed/stopped/cancelled/failed 内部终态并修正异常/非法正返回值。
- `src/services/scheduler/scheduler_routes.cpp`：仅把 block completed/stopped/cancelled 写入同步执行 JSON。
- `src/services/scheduler/scheduler_batch_runtime.cpp`：仅解析执行结果 status 并写入 stopped/cancelled batch 快照。
- `src/tests/test_scheduler_e2e/test_scheduler_mutation_guard.cpp`：仅补 T4.5 同步结果和异步 batch 快照断言。

开始本任务前已有且必须保留、不再修改的基线 diff：`AGENTS.md` 以及 T4.1～T4.4 涉及的
`scheduler_plugin.h`、`scheduler_channel_admin.cpp`、`scheduler_routes.cpp`、
`scheduler_stream_executor.cpp`、`test_scheduler_mutation_guard.cpp` 和 Feature 文档改动。

## 验收

- 同步 block route 的 EOF 成功、主动停止和 source 取消分别返回 completed、stopped、cancelled；异常和 operator 错误返回 `OP_EXEC_FAIL/execute`。
- batch runtime 快照对上述路径分别为 completed、stopped、cancelled、failed、failed，且都有完成时间。
- `ProcessBlock()` 抛异常或返回错误时，当前 data batch 恰好 release 一次，后续 batch 不再轮询。
- 主动停止仍执行一次 `Flush()`；cancelled、异常和 operator 错误不执行 `Flush()`。
- 定向命令：`cmake --build build --target test_scheduler_mutation_guard -j2`，随后运行 `build/output/test_scheduler_mutation_guard`。
- 完成后执行 `git diff HEAD --check`；不运行 T5 或无关回归。

## 时间盒与停止条件

- 时间盒：30 分钟。
- 只完成 T4.5 的 block 终态与现有 batch snapshot 传播；达到验收条件后勾选 T4.5/T4 并停止。
- 若必须扩展公共 ABI 或重构 RequestStop/执行回调才能满足 source 事件终态，则记录具体阻塞并停止，不吸收 T5。

## 验收证据

- `cmake --build build --target test_scheduler_mutation_guard -j2`：通过。
- `build/output/test_scheduler_mutation_guard`：通过，输出
  `=== Scheduler mutation guard tests passed ===`。
- `git diff HEAD --check`：通过，无输出。
- 环境未提供 `clang-format`/`git-clang-format`；新增 C++ 行已人工检查，未超过 120 列。
- T4.5 与 T4 已勾选完成；未开始 T5。
