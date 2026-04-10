# Sprint 17 设计-实现-测试一致性对账

> 基于 `tasks/sprints/_templates/design_implementation_consistency_template.md` 实例化。  
> 对账日期：2026-04-10

---

## 1. 关键设计条款索引（MUST）

| 条款 ID | 设计文件位置 | 条款内容（MUST） | 优先级 |
|---|---|---|---|
| D17-001 | `tasks/sprints/sprint17/design.md:167` | group 仅支持 `group_mode=dag`；group 多 SQL 语句数必须 `>=2`；single 仅允许单条 stream SQL | P0 |
| D17-002 | `tasks/sprints/sprint17/design.md:246` | 节点类型判定必须支持 `stream/batch`；同一语句混用 stream 与非 stream source 必须拒绝 | P0 |
| D17-003 | `tasks/sprints/sprint17/design.md:276` | 非 stream sink（`dataframe/database`）同一 sink_key 仅允许单 writer，违反即构建期失败 | P0 |
| D17-004 | `tasks/sprints/sprint17/design.md:285` | share set 自动构建覆盖全部 stream 节点（含 non-root），并采用 fixed + coordinated_drop | P1 |
| D17-005 | `tasks/sprints/sprint17/design.md:200` | `status/list` 必须返回 `node_id/sql_index/phase/error_*` 等结构化运行态错误信息 | P0 |
| D17-006 | `tasks/sprints/sprint17/design.md:405` | group 节点快照必须包含 `node_kind/sql_index/phase/error_code/error_message` | P0 |

---

## 2. 设计到代码映射（Traceability Matrix）

| 条款 ID | 实现状态 | 代码文件 | 关键函数/分支 | 代码定位 | 备注 |
|---|---|---|---|---|---|
| D17-001 | 已完成 | `src/services/task/task_plugin.cpp` | single/group 入参契约校验 | `src/services/task/task_plugin.cpp:1428` | 覆盖 single 与 group 请求护栏 |
| D17-001 | 已完成 | `src/services/scheduler/scheduler_stream_group.cpp` | group_mode/sql_text/sql_count 校验 | `src/services/scheduler/scheduler_stream_group.cpp:258` | 服务端最终防线 |
| D17-002 | 已完成 | `src/services/scheduler/scheduler_stream_group.cpp` | 节点 kind 判定与 mixed source 拒绝 | `src/services/scheduler/scheduler_stream_group.cpp:476` | 返回 `STREAM_GROUP_NODE_KIND_INVALID` |
| D17-003 | 已完成 | `src/services/scheduler/scheduler_stream_group.cpp` | 非 stream sink 多 writer 拒绝 | `src/services/scheduler/scheduler_stream_group.cpp:640` | 返回 `STREAM_GROUP_NON_STREAM_SINK_MULTI_WRITER` |
| D17-004 | 已完成 | `src/services/scheduler/scheduler_stream_group.cpp` | 全 stream 节点分组 + share set 构建 | `src/services/scheduler/scheduler_stream_group.cpp:693` | 已移除 root-only 限制 |
| D17-004 | 已完成 | `src/services/scheduler/scheduler_stream_group.cpp` | fixed + coordinated_drop 选项 | `src/services/scheduler/scheduler_stream_group.cpp:870` | `SharedSourceHub(kFixed)` |
| D17-005 | 已完成 | `src/services/scheduler/scheduler_json_codec.cpp` | group `nodes[]` 运行态字段输出 | `src/services/scheduler/scheduler_json_codec.cpp:247` | 包含 `phase/error_*` |
| D17-006 | 已完成 | `src/services/scheduler/scheduler_json_codec.cpp` | `node_kind/sql_index` 输出 | `src/services/scheduler/scheduler_json_codec.cpp:255` | 前端可直接展示 |
| D17-006 | 已完成 | `src/framework/core/error_contract.cpp` | 新增错误码字符串映射 | `src/framework/core/error_contract.cpp:37` | 包含 `NODE_KIND_INVALID/NODE_EXECUTION_FAILED/NON_STREAM_SINK_MULTI_WRITER` |

---

## 3. 测试门禁矩阵（正向 + 反向）

| 条款 ID | 正向用例（应成功） | 反向用例（应失败） | 测试文件 | 状态 |
|---|---|---|---|---|
| D17-001 | `T53/T54` group 正常执行 | `T52` 契约护栏（legacy 冲突、single 多 SQL、group 少于 2 条） | `src/tests/test_scheduler_e2e/test_scheduler_e2e.cpp` | 已完成 |
| D17-002 | `T53/T54` mixed DAG 节点执行（含 `batch/stream`） | `T52` 非法 SQL/解析失败返回结构化错误 | `src/tests/test_scheduler_e2e/test_scheduler_e2e.cpp` | 已完成 |
| D17-003 | N/A（该条款为限制项） | `T50.1` 非 stream sink 多 writer 拒绝并返回错误码 | `src/tests/test_scheduler_e2e/test_scheduler_e2e.cpp` | 已完成 |
| D17-004 | `T49/T49.1` root 同源 share set；`T54.1` non-root share set | `T66/T67` 同签名共享成功、异签名显式拒绝 | `src/tests/test_scheduler_e2e/test_scheduler_e2e.cpp` | 已完成 |
| D17-005 | `T53/T54` 校验 `nodes[]` 结构化字段可见 | `T51` timeout 失败返回 `error_code/error_message` | `src/tests/test_scheduler_e2e/test_scheduler_e2e.cpp` | 已完成 |
| D17-006 | `T53/T54` 断言 `node_kind/sql_index/phase` 字段 | `T52` 断言错误返回包含 `sql_index` | `src/tests/test_scheduler_e2e/test_scheduler_e2e.cpp` | 已完成 |

---

## 4. Review 检查结果

- [x] 所有 MUST 条款已编号（D17-001 ~ D17-006）。
- [x] 所有条款已映射到代码 `file:line`。
- [x] 所有条款已映射到自动化测试（含反向约束）。
- [x] 非 root share set 偏差已修复并补回归（`T54.1`）。
- [x] 评审偏差与根因已记录到 `tasks/sprints/sprint17/review.md`。

---

## 5. 验证记录（2026-04-10）

执行命令：

1. `cmake --build build --target test_scheduler_e2e -j4`
2. `./build/output/test_scheduler_e2e`
3. `./build/output/test_scheduler_mutation_guard`
4. `./build/output/test_task`
5. `ctest --test-dir build --output-on-failure`
6. `npm --prefix src/frontend run build`

结果：全部通过。
