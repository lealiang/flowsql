# Sprint 18 检视记录

## 评审信息

- 评审日期：2026-04-15
- 评审范围：
  - 流式调度并发安全与生命周期边界修复（1-8 项）
  - 代码可维护性收敛（共享 source 装配逻辑拆分、工具函数去重）
  - 回归测试充分性
- 评审目标：
  - 对照计划确认实现完成度
  - 识别并记录残留风险
  - 形成可追踪的验证证据

## 刚刚 Review 结论（原始记录）

1. 本轮代码变更整体按计划完成，1-8 项目标均已落地。
2. 目标达成情况：
   - 并发安全：`StreamTaskGroup::Snapshot` 竞态问题已按“无锁元数据 + 节点锁”方案修复。
   - 生命周期：`StreamRuntime` 的 Reset 残留问题已修复。
   - TOCTOU：共享订阅上限检查已收敛到 Hub 内部锁区。
   - 资源管理：`FanOutStreamChannel/StreamHubChannel` 的 Open 失败回滚已补齐。
   - 可维护性：shared source 订阅逻辑已从 `scheduler_stream_executor.cpp` 拆分到独立编译单元。
3. 当时识别的 2 个残留风险：
   - R-01：`error_no` 与 `error_meta` 分离发布，存在极短窗口快照字段不一致风险。
   - R-02：`BuildSubscriberInput()` 未检查 `Open()` 返回值，失败路径可能被静默吞掉。

## 残留风险处置结果

### R-01（已关闭）

- 修复动作：
  - 将 `error_no` 合并进 `GroupErrorMeta`，改为单对象原子发布/读取。
- 结果：
  - 快照读取不再存在“错误码与错误文本撕裂”的窗口。

### R-02（已关闭）

- 修复动作：
  - `BuildSubscriberInput` 增加错误码输出；
  - `Open()` 失败时返回 `nullptr`；
  - `AddSubscriber` 对失败路径显式返回错误并中止注册。
- 结果：
  - subscriber 输入通道创建失败可观测、可追踪，不再隐式继续执行。

## 补充测试（按评审建议新增）

1. `Stop -> Start` 队列残留清理测试：
   - `test_stream` 新增 `T23`，验证 Runtime 重启后不会执行旧 ready/timer 残留任务。
2. 并发订阅上限竞争测试：
   - `test_scheduler_mutation_guard` 新增并发 `AddSubscriber` 场景，验证 `max_subscribers` 硬上限在竞争下仍严格生效。
3. Open 失败回滚测试：
   - `test_stream` 新增 `T24/T25`，覆盖 `FanOut` 和 `StreamHub` 中途失败后的资源回滚与状态一致性。

## 验证证据

已执行并通过：

1. `cmake --build build -j6`
2. `ctest --output-on-failure -R "test_stream|test_scheduler_mutation_guard"`
3. `./build/output/test_task`
4. `./build/output/test_scheduler_e2e`

## 最终结论

1. 评审识别的问题已闭环，原 2 个残留风险全部关闭。
2. 本轮修复对既有能力无回归，核心流式链路与调度链路验证通过。
3. Sprint 18 相关流式改造可进入后续迭代基线。

