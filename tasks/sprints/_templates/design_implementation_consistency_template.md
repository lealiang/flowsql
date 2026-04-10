# Sprint 设计-实现一致性模板

> 用途：防止“设计已明确但编码遗漏”。  
> 使用时机：每个 Sprint 的 `design/planning/review` 编写与验收阶段。

---

## 1. 关键设计条款索引（Design MUST Index）

> 说明：仅收录带强约束语义的条款（`必须` / `不得` / `仅允许` / `必须失败`）。

| 条款 ID | 设计文件位置 | 条款内容（MUST） | 优先级 |
|---|---|---|---|
| DXX-001 | `tasks/sprints/sprintXX/design.md:#L` | 示例：non-root 节点也必须自动构建 share set | P1 |
| DXX-002 | `tasks/sprints/sprintXX/design.md:#L` | 示例：single 模式仅允许 1 条 stream SQL | P0 |
| DXX-003 | `tasks/sprints/sprintXX/design.md:#L` | 示例：违反约束必须返回结构化错误（含 `error_code/sql_index`） | P0 |

---

## 2. 设计到代码映射表（Traceability Matrix）

> 说明：每个条款 ID 必须映射到明确的代码位置。  
> 不允许出现“逻辑上已支持但无代码定位”。

| 条款 ID | 实现状态 | 代码文件 | 关键函数/分支 | 代码定位 | 备注 |
|---|---|---|---|---|---|
| DXX-001 | 未开始/开发中/已完成 | `src/...` | `FunctionName` | `src/...:123` |  |
| DXX-002 | 未开始/开发中/已完成 | `src/...` | `FunctionName` | `src/...:456` |  |
| DXX-003 | 未开始/开发中/已完成 | `src/...` | `FunctionName` | `src/...:789` |  |

---

## 3. 测试门禁矩阵（Test Gate Matrix）

> 规则：每个关键条款至少 2 条用例。  
> 1 条正向能力用例 + 1 条反向约束用例。

| 条款 ID | 正向用例（应成功） | 反向用例（应失败） | 测试文件 | 状态 |
|---|---|---|---|---|
| DXX-001 | `Txx: non-root 自动 share_set` | `Txx: source 不一致报错` | `src/tests/...` | 未开始/开发中/已完成 |
| DXX-002 | `Txx: single 单条 stream SQL` | `Txx: single + 多 SQL 拒绝` | `src/tests/...` | 未开始/开发中/已完成 |
| DXX-003 | `Txx: 正常返回字段完整` | `Txx: 非法请求返回 error_code/sql_index` | `src/tests/...` | 未开始/开发中/已完成 |

---

## 4. 评审检查清单（Code Review Checklist）

### 4.1 设计一致性检查

- [ ] 所有 MUST 条款均已分配 `条款 ID`。
- [ ] 所有 `条款 ID` 均有明确代码定位（`file:line`）。
- [ ] 所有 `条款 ID` 均有正向 + 反向测试。
- [ ] 不存在“设计写了，但代码分支条件缺失”的情况。
- [ ] 若未实现，已在 `review.md` 记录原因与后续计划。

### 4.2 质量与风险检查

- [ ] 错误码与错误字段与设计一致（`error_code/error_stage/sql_index`）。
- [ ] 并发场景有对应稳定性回归（如 stop/timeout/retention）。
- [ ] 变更未引入隐式兼容分支或语义兜底。

---

## 5. Sprint 收尾对账（Design/Planning/Review 三方一致）

> 关闭 Sprint 前，必须完成以下对账：

- [ ] `design.md` 的关键条款与 `planning.md` 验收项一一对应。
- [ ] `planning.md` 验收项与测试用例名一一对应。
- [ ] `review.md` 记录本 Sprint 偏差与根因（如有）。
- [ ] 验证命令输出已留痕（构建、单测、集成测试、前端构建）。

建议记录模板：

```text
验证日期：YYYY-MM-DD
命令：
1) ...
2) ...
结果：全部通过 / 部分失败（附失败项）
```

---

## 6. 使用建议

1. 在 Sprint 启动时创建本模板副本：`tasks/sprints/sprintXX/traceability.md`。
2. 开发过程中随代码同步更新“实现状态”和“测试状态”。
3. 评审时先过本模板，再决定是否标记 Sprint 完成。
