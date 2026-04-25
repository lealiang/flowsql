## Sprint 20 BaselineA 复审状态

### 1. 复审范围

- 设计文档：
  - `tasks/sprints/sprint20-baselineA/code-design.md`
  - `tasks/sprints/sprint20-baselineA/planning.md`
  - `tasks/sprints/sprint19-baseline/design.md`
- 代码边界：
  - `src/plugins/baseline/*`
  - `src/tests/test_baseline/*`
- 本轮收口文档：
  - `tasks/sprints/sprint20-baselineA/review-fix-plan.md`

本次复审聚焦 `Story 18.20：测试收口、死代码清理与最终一致性审查`，重点同步“截至当前已经完成并验证”的整改状态，不把尚未执行的后续动作写成已完成。

### 2. 当前结论

截至当前会话，`review-fix-plan.md` 中列出的整改任务已经全部落地并完成验证：

- `P0-1`：`RelationTask::SubmitBlock()` 返回结果生命周期已修复。
- `P0-2`：`KeyRiskFusion` 与 `FusionResult` 已收口为固定上限、低分配表示。
- `P0-3`：业务时区语义已与进程系统时区解耦，`TimezoneMutex` 已移除。
- `P1-1`：relation routed runtime 已改为锁外物化、锁内回写。
- `P1-2`：`ValueDetectorCore / RatioDetectorCore` 已收口为固定 shard 锁，不存在 per-key mutex。
- `P1-3`：高基数运行时状态已补轻量 idle prune 与可观测性。
- `P2-1`：`14.4` 要求的 `building / built / validating` 中间态已真实写入，`candidate` 在线服务死分支已删除。
- `P2-2`：`SeedMetricBasisForTesting` 已下沉为 test-only seam，不再污染生产 ABI。

### 3. 已完成验证

本轮已重新编译并执行以下 baseline 测试二进制，结果全部通过：

- `/mnt/d/working/flowSQL/build-codex/output/test_baseline`
- `/mnt/d/working/flowSQL/build-codex/output/test_baseline_value_task`
- `/mnt/d/working/flowSQL/build-codex/output/test_baseline_ratio_task`
- `/mnt/d/working/flowSQL/build-codex/output/test_baseline_relation_task`
- `/mnt/d/working/flowSQL/build-codex/output/test_baseline_concurrency`
- `/mnt/d/working/flowSQL/build-codex/output/test_baseline_model_helpers`
- `/mnt/d/working/flowSQL/build-codex/output/test_baseline_task_headers`
- `/mnt/d/working/flowSQL/build-codex/output/test_baseline_rebuild`

其中：

- `test_baseline_value_task` 已覆盖 `ValueDetectorCore does not serve candidate model`
- `test_baseline` 已覆盖：
  - `shadow -> formal` 切换中的 `candidate_state / switch_state`
  - `stage_seen_building / built / validating`
  - relation `candidate_fail`
  - relation `new lineage`
  - 高基数 runtime 与 key fusion idle prune
- `test_baseline_concurrency` 已再次验证多 key 并发提交与关闭路径

### 4. 文档同步状态

当前文档状态如下：

- 已同步：
  - `tasks/sprints/sprint20-baselineA/review-fix-plan.md`
  - 本文件 `tasks/sprints/sprint20-baselineA/review.md`
- 按用户要求未修改：
  - `tasks/sprints/sprint20-baselineA/planning.md`

因此，当前应以 `review-fix-plan.md` 和本文件作为本轮复审进度与完成状态的最新记录；`planning.md` 中若仍残留旧文件名或旧阶段表述，属于尚未回填的历史引用，不代表代码状态回退。

### 5. 14.4 章节状态

`code-design.md 14.4` 当前已具备以下证据闭环：

- `candidate_state / switch_state` 已统一为设计词汇。
- `building / built / validating` 已有真实写入点，并可在 snapshot 中观察。
- `candidate_fail`、`new lineage`、`insufficient_data / unavailable` 语义均有测试覆盖。
- `RemoveTaskContributions(task_id)` 与高基数 prune 的组合行为已复核通过。

结论：`14.4` 相关实现当前处于“已落地并已验证”状态。
