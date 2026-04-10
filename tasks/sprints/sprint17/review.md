# Sprint 17 检视记录

## 评审信息

- 评审日期：2026-04-10
- 评审范围：
  - Story 14.14（Hybrid DAG）设计与实现一致性
  - share set 自动构建（fixed）在 non-root 场景的落地情况
- 评审目标：
  - 设计约束是否完整落地到代码
  - 偏差根因与过程改进项是否可追踪

## 本轮结论

1. Sprint 17 主链路已可用，关键回归通过。
2. 发现并修复 1 个“设计已明确但实现偏差”的问题：
   - 设计要求：non-root 节点也可自动建 share set。
   - 实际实现：初版仅 root 节点参与自动建 share set。
3. 已完成修复与回归补强：
   - 代码修复：share set 自动分组改为遍历全部 stream 节点。
   - 测试补强：新增 `T54.1` 覆盖 non-root 自动建 share set。

## 设计落地偏差复盘

### D-01：share set 自动构建被误实现为 root-only（已解决）

- 优先级：P1
- 设计要求：
  - `tasks/sprints/sprint17/design.md` 第 6.6 节明确“非 root 节点同样可建”。
- 偏差表现：
  - `scheduler_stream_group.cpp` 初版实现仅对 `depends_on.empty()` 的 root stream 节点分组。
- 影响：
  - 中间通道分叉（`n1 -> mid`, `n2/n3 <- mid`）缺少自动一致性保护。
  - 在 SPSC 或高压丢弃场景下，分支结果可能不完整或不一致。

### 根因分析（为什么会被忽视）

1. 历史实现惯性：早期 share set 逻辑按“root 广播”建模，迁移到 Hybrid 时沿用了 root 分组骨架，未彻底重审约束边界。
2. 验收粒度不足：计划文档里“share set 一致性”是总项，没有拆到“root/non-root”两个显式子项。
3. 测试盲区：已有用例覆盖 root 同源分支（T49/T49.1），但缺少 non-root 场景，导致偏差未被自动化测试拦截。
4. 代码评审映射不完整：评审关注点偏向“能力存在性”，未逐条映射设计关键句到代码分支条件。

## 修复与验证

1. 代码修复：
   - share set 自动分组改为“全部 stream 节点（含 root/non-root）”。
2. 测试补齐：
   - 新增 `T54.1`：`n1 -> stream.mid`，`n2/n3 <- stream.mid`，断言 `share_set_count=1` 且双分支完整收敛。
3. 验证结果：
   - `test_scheduler_e2e` 通过（含 `T54.1`）。
   - `test_scheduler_mutation_guard`、`test_task`、`ctest` 全量通过。

## 过程改进动作（防复发）

1. 设计到代码映射清单化：
   - 对“必须/不得/仅限”类设计句，编码前生成逐条映射清单并在评审时逐条勾验。
2. 验收条目细分：
   - 对同一能力的不同语义边界（如 root vs non-root）拆成独立验收项，避免笼统“已支持”掩盖缺口。
3. 测试门禁补齐：
   - 每个关键能力至少 1 条“非典型路径”测试（本例即 non-root 分支）。
4. 评审模板补充：
   - 增加“设计关键条件是否出现在代码条件分支中”的专门检查行。

## 当前状态

- D-01：已修复，已补测试，已纳入文档。
