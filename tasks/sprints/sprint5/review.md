# Sprint 5 迭代评审

**日期**：2026-03-12
**Sprint 目标**：实现 ClickHouse 数据库通道驱动，并将数据库通道配置从静态 gateway.yaml 迁移到 Web 动态管理

---

## 一、Story 验收结果

| Story | 描述 | 状态 | 备注 |
|-------|------|------|------|
| 5.1 | ClickHouseDriver 核心实现 | ✅ 通过 | HTTP + Arrow IPC，含 relation_adapters.h 重构 |
| 5.2 | ClickHouse 端到端测试 | ✅ 通过 | T1-T16，ClickHouse 不可达时自动 SKIP |
| 6.1 | DatabasePlugin 持久化与动态管理 | ✅ 通过 | YAML + AES-256-GCM，M1-M9 全部通过 |
| 6.2 | Scheduler 新增通道管理端点 | ✅ 通过 | 4 个 /db-channels/* 端点 |
| 6.3 | Web 服务 CRUD API | ✅ 通过 | 转发到 Scheduler |
| 6.4 | 废弃 gateway.yaml 静态配置 | ✅ 通过 | 迁移到 config/flowsql.yml |
| 6.5 | 前端通道管理 UI | ✅ 通过 | 新增/删除对话框，动态表单 |
| 6.6 | 端到端测试 | ✅ 通过 | test_database_manager 20/20（含并发、边界、功能扩展测试） |

---

## 二、功能演示

### Epic 5：ClickHouse 驱动

- `ClickHouseDriver` 基于 httplib HTTP 协议，无连接池，每次 `CreateSession()` 创建新实例
- `ClickHouseSession` 同时实现 `IArrowReadable` + `IArrowWritable`，走 Arrow IPC Stream 原生路径
- `DatabaseChannel::CreateArrowReader/Writer` 激活，通过 `dynamic_cast<IArrowReadable/IArrowWritable>` 检查能力
- 顺带重构：提取 `relation_adapters.h`（`RelationBatchReader` + `RelationBatchWriterBase`），消除 MySQL/SQLite 重复代码约 400 行；删除死代码 `row_based_db_driver_base.h/cpp`

### Epic 6：Web 动态管理数据库通道

- `IDatabaseFactory` 扩展 `AddChannel/RemoveChannel/UpdateChannel`，`List` 增加 `config_json` 脱敏参数
- `DatabasePlugin::Start()` 从 `flowsql.yml` 加载通道配置，重启自动恢复
- 密码 AES-256-GCM 加密存储，密钥从 `FLOWSQL_SECRET_KEY` 环境变量读取
- Scheduler 新增 4 个端点，Web 层转发，前端 Channels.vue 支持新增/删除通道

---

## 三、测试结果

| 测试套件 | 结果 |
|---------|------|
| test_sqlite | ✅ 全部通过 |
| test_clickhouse | ✅ T2/T3 通过，T1/T4-T16 SKIP（无 ClickHouse 环境） |
| test_database_manager | ✅ 20/20 通过（含并发、边界、功能扩展） |
| test_database (E2E) | ✅ 全部通过 |
| test_connection_pool | ✅ 全部通过 |
| 全量编译 | ✅ 无错误 |

---

## 四、技术债务

| 项目 | 优先级 | 说明 |
|------|--------|------|
| `arrow_db_session.h` 占位符 | 低 | `ClickHouseTraits::ConnectionType = void*` 已无实际使用，可清理 |
| Scheduler 端点缺少认证 | 中 | `/db-channels/*` 端点无鉴权，生产环境需补充 |
| 前端缺少编辑通道功能 | 低 | 当前只有新增/删除，编辑需 UpdateChannel 支持 |
| ~~**前端添加通道表单缺少必填校验**~~ | ~~高~~ | ~~MySQL/ClickHouse 通道的 `database` 字段未标记为必填，用户留空时连接建立成功但执行 SQL 报 `No database selected`。~~ ✅ 已修复（2026-03-13）：前端 `database` 字段加 `required` 标记，`handleAdd` 补充 MySQL/ClickHouse 类型必填校验。 |
| **底层 MySQL 错误信息未透传** | 中 | `RelationDbSessionBase::CreateReader` 捕获了 MySQL 错误字符串，但 `IBatchReadable::CreateReader` 接口无 `error*` 参数，错误被丢弃。调用方只能看到 `CreateReader failed`，无法得知具体原因（如 `No database selected`、`Table doesn't exist` 等），增加调试成本。已登记为 Story 6.7，待后续 Sprint 规划。 |
| `config.cpp` 插件 Map 节点漏解析 `option` 字段 | 高 | `gateway.yaml` 中插件以 Map 格式配置 `option` 时，`LoadConfig` 未读取该字段，导致 `config_file` 参数未传给 database 插件，`AddChannel` 报 `config_file not set`。已修复。 |

---

## 五、测试完备性审查（2026-03-13）

审查人：资深测试专家视角
审查范围：`test_clickhouse.cpp`（T1-T16）、`test_database_manager.cpp`（M1-M9+扩展）、`test_plugin_e2e.cpp`（E1-E7、P1-P3）

### 总体评价

测试分层结构合理，并发测试从设计阶段就纳入，优于 Sprint 4。但存在若干真实覆盖缺口，尤其集中在并发混合场景和关键功能路径上。

### 并发测试缺口

| 编号 | 严重度 | 位置 | 问题 | 状态 |
|------|--------|------|------|------|
| TQ-C1 | P3 | `test_clickhouse.cpp` | T12/T13 是纯读或纯写，没有读写混合并发测试 | ✅ 已修复（2026-03-13）：新增 T17 `test_concurrent_read_write_mixed()`，8 读 + 4 写混合，21/21 通过 |
| TQ-C2 | P2 | `test_clickhouse.cpp` | 多线程同时触发连接失败时 `last_error_` 存在数据竞争风险，未验证 | ✅ 已修复（2026-03-13）：新增 T18 `test_concurrent_connect_failure()`，10 线程并发连接不可达地址，无崩溃 |
| TQ-C3 | P1 | `test_database_manager.cpp` | M9-ext 是串行两阶段（先全 Add 再全 Remove），未测试 Add/Remove 同一通道的真正混合并发 | ✅ 已修复（2026-03-13）：新增 `test_concurrent_add_remove_same_channel()`，验证无崩溃且内存/YAML 状态一致 |
| TQ-C4 | P1 | `test_database_manager.cpp` | 并发 `UpdateChannel` 完全缺失（UpdateChannel 是复合操作，存在 TOCTOU 窗口） | ✅ 已修复（2026-03-13）：新增 `test_concurrent_update_channel()`，验证最终配置为合法更新结果且 YAML 一致 |
| TQ-C5 | P2 | `test_database_manager.cpp` | `ListChannels` 遍历时并发 Add/Remove 是否会迭代器失效，未测试 | ✅ 已修复（2026-03-13）：新增 `test_concurrent_list_with_add_remove()`，List 线程持续遍历，5 个线程各 10 轮 Add/Remove，16/16 通过 |
| TQ-C6 | ~~P2~~ | ~~`test_plugin_e2e.cpp`~~ | ~~E6/E7 末尾没有 `g_passed++`，测试失败不影响最终返回码，统计失真~~ | ✅ 误判：test_plugin_e2e.cpp 用 assert 控制失败，无 g_passed 机制，E6/E7 失败会直接 abort |
| TQ-C7 | P3 | `test_plugin_e2e.cpp` | P1-P3 无并发 AddChannel 测试（多线程同时添加不同通道） | ✅ 已修复（2026-03-13）：新增 P4 `test_p4_concurrent_add_channels()`，8 线程并发添加，验证全部可用，P1-P4 通过 |

### 边界条件缺口

| 编号 | 严重度 | 位置 | 问题 | 状态 |
|------|--------|------|------|------|
| TQ-B1 | P3 | `test_clickhouse.cpp` T7 | 类型矩阵只测非 NULL 值，Nullable 列的 NULL 处理未验证 | ✅ 已修复（2026-03-13）：新增 T19 `test_nullable_null_handling()`，验证 NULL 写入/回读正确保留 |
| TQ-B2 | P2 | `test_clickhouse.cpp` T8 | 大批量只测单个 batch，多 batch 写入路径（`{batch1, batch2}`）未测试 | ✅ 已修复（2026-03-13）：新增 T20 `test_multi_batch_write()`，3 batch × 100 行，总行数验证正确 |
| TQ-B3 | P2 | `test_database_manager.cpp` M8 | 重启恢复只验证通道数量，未验证配置字段值（host/port/database）是否正确恢复 | ✅ 已修复（2026-03-13）：新增 `test_restart_recovery_field_values()`，验证 host/port/database/user 各字段值正确恢复 |
| TQ-B4 | P1 | `test_database_manager.cpp` | 带密码通道重启后解密验证缺失：AddChannel 含密码 → 重启 → Get() → Ping() 完整路径未测试 | ✅ 已修复（2026-03-13）：原 SQLite 版本无效（违反 L14），替换为 `test_password_restart_decrypt_mysql()` + `test_password_restart_decrypt_clickhouse()`，走真实认证路径，MySQL/ClickHouse 不可达时自动 SKIP |
| TQ-B5 | P2 | `test_clickhouse.cpp` T14 | 注入测试断言过弱：只断言 `rc != 0`，未区分"表不存在"和"语法错误"（两者都是 -1，但含义不同） | ✅ 已修复（2026-03-13）：新增 T21 `test_injection_error_distinction()`，断言表不存在错误 ≠ 语法错误 |

### 功能覆盖缺口

| 编号 | 严重度 | 位置 | 问题 | 状态 |
|------|--------|------|------|------|
| TQ-F1 | P3 | `test_database_manager.cpp` | UpdateChannel 后重启恢复未测试（Update → 重启 → 验证新配置生效） | ✅ 已修复（2026-03-13）：新增 `test_update_then_restart_recovery()`，验证新路径恢复、旧路径不存在 |
| TQ-F2 | P2 | `test_plugin_e2e.cpp` | E5 只验证 ClickHouse 不支持行式接口，未验证 MySQL 不支持列式接口（接缝测试应双向） | ✅ 已修复（2026-03-13）：新增 E5b `test_mysql_arrow_interface_unsupported()`，验证 MySQL CreateArrowWriter/Reader 均返回 -1 |
| TQ-F3 | P3 | `test_database_manager.cpp` | 单元层没有验证 AddChannel 后 `Get()` 返回有效通道（只在插件层 P1 测试，单元层缺失） | ✅ 已修复（2026-03-13）：新增 `test_add_channel_then_get()`，验证 Get() 返回有效通道且 IsConnected=true，不存在通道返回 nullptr |

### 最高优先级修复项

**TQ-B4**（Epic 6 核心路径）：密码加密存储 → 重启 → 解密 → 连接成功，这条路径是 Epic 6 的核心价值，目前完全没有端到端覆盖。

---

## 六、产品待办更新

- Epic 5 ✅ 已完成
- Epic 6 ✅ 已完成
- Epic 7（Pipeline 增强）、Epic 8（流式架构）待规划

---

## 六、下一个 Sprint 建议目标

- Epic 7：Pipeline 增强（算子链、条件分支）
- 或 Epic 8：流式架构（Arrow Flight / 共享内存数据面）
