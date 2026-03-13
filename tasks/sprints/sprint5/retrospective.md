# Sprint 5 迭代回顾

回顾日期：2026-03-13
参与者：Claude Code（资深测试专家视角）、Claude Code（资深软件专家视角）

---

## 一、做得好的地方（Keep）

### K1：并发测试从设计阶段纳入，不再是事后补丁

Sprint 4 的 P2 问题是"多线程并发测试完全缺失"，并发测试是在 Sprint 结束后才补充的。Sprint 5 在 Story 验收标准中就包含了并发测试要求，`test_database_manager.cpp` 的 M9-ext 在初始版本就有并发 Add/Remove 测试，`test_clickhouse.cpp` 的 T12/T13 在初始版本就有并发读写测试。

这说明 Sprint 4 的 L12 教训被吸收了。并发不再是"事后想到的"，而是设计约束的一部分。

### K2：测试分层结构完整，三层覆盖清晰

Sprint 5 延续并强化了 Sprint 4 建立的三层测试体系：

- **驱动层**：`test_clickhouse.cpp`（T1-T21）— ClickHouse 驱动功能、类型矩阵、并发、边界
- **单元层**：`test_database_manager.cpp`（20 个测试）— DatabasePlugin 生命周期、持久化、并发管理
- **插件层**：`test_plugin_e2e.cpp`（E1-E7、P1-P4、E5b）— 完整生产路径 PluginLoader → IDatabaseFactory → IDatabaseChannel

每层职责清晰，没有越层替代（如用驱动层测试代替插件层测试）。

### K3：接口能力检测模式（dynamic_cast）设计干净

`ClickHouseSession` 同时实现 `IArrowReadable` + `IArrowWritable`，`DatabaseChannel::CreateArrowReader/Writer` 通过 `dynamic_cast` 检查能力，不支持的驱动返回 -1。这个设计让接口扩展对现有驱动零侵入，MySQL/SQLite 不需要改动任何代码。

E5b 测试（`test_mysql_arrow_interface_unsupported`）验证了这个接缝的双向性，是接口设计正确性的有力证明。

### K4：重构顺带消除大量重复代码

提取 `relation_adapters.h`（`RelationBatchReader` + `RelationBatchWriterBase`），消除 MySQL/SQLite 重复代码约 400 行，删除死代码 `row_based_db_driver_base.h/cpp`。这是在交付新功能的同时主动偿还技术债务，而不是让债务继续累积。

### K5：测试完备性审查流程有效运转

Sprint 5 的测试完备性审查（review.md 第五节）系统性发现了 15 个缺口（TQ-C1~C7、TQ-B1~B5、TQ-F1~F3），全部在同一天修复完毕。这说明"独立的测试完备性审查"作为流程已经稳定，能够在 Sprint 结束时发挥质量兜底作用。

---

## 二、需要改进的地方（Problem）

### P1：TQ-B4 初版用 SQLite 测试密码认证路径——L14 教训未能预防

**现象**：`test_password_restart_decrypt()` 初版使用 SQLite `:memory:` 测试"带密码通道重启后解密验证"。SQLite 完全忽略密码字段，`Get()` 成功只证明 SQLite 连接成功，与 AES-256-GCM 解密无关。这条测试制造了虚假覆盖，是 Sprint 5 最严重的测试质量问题。

**根因分析**：

这个问题的根因不是"不知道 SQLite 不校验密码"，而是**测试设计时没有问"这个替代品对被测特性是否有感知"**。开发者选择 SQLite 的动机是降低测试依赖（不需要 MySQL/ClickHouse 环境），但没有意识到这个选择同时让测试对被测特性（密码认证）完全无感知。

更深层的问题是：**Epic 6 的核心价值是"密码加密存储 → 重启 → 解密 → 连接成功"这条路径**，但这条路径在整个 Sprint 的开发过程中从未被端到端验证过。如果不是测试完备性审查发现了这个缺口，这条核心路径会以"已测试"的状态进入下一个 Sprint。

**改进**：
- 凡是涉及密码、认证、加解密的测试，必须使用真实校验密码的数据库（MySQL 或 ClickHouse）
- 在 Story 验收标准中明确"密码认证路径必须用真实数据库验证"，不允许用 SQLite 替代
- 已固化为 L14

---

### P2：SaveToYaml 并发写竞态——并发设计遗漏了文件 IO 层

**现象**：`SaveToYaml()` 在 `mutex_` 之外执行文件写入。当多个线程并发调用 `RemoveChannel`（各自持有 `mutex_` 完成内存操作后释放），再各自调用 `SaveToYaml()`，最后写入的线程可能持有的是旧快照，导致已删除的通道重新出现在 YAML 文件中。

**根因分析**：

`mutex_` 保护了内存状态（`channels_`、`configs_`），但没有保护文件状态。开发者在设计并发保护时，思维边界停在了内存层，没有延伸到持久化层。这是一个典型的**保护范围不完整**问题：锁住了数据结构，但没有锁住数据结构的持久化操作。

`SaveToYaml` 是一个复合操作（读取内存快照 → 序列化 → 写文件），这三步必须作为一个原子单元执行，否则快照和写入之间的窗口会导致状态不一致。

**修复**：引入 `save_mutex_` 串行化文件写入，并在 `save_mutex_` 内部重新快照 `configs_`，确保写入的是最新状态。

**改进**：
- 持久化操作（文件写入、数据库写入）与内存操作需要独立的并发保护分析
- 复合操作（读-改-写）必须识别为原子单元，并在设计阶段明确其保护范围

---

### P3：TQ-C3/C4 揭示的并发测试设计不够深入

**现象**：M9-ext 的并发测试是"串行两阶段"——先全部 Add，再全部 Remove，两个阶段之间没有交叉。这不是真正的混合并发，无法触发 Add/Remove 同一通道时的竞态条件。同样，`UpdateChannel` 的并发测试完全缺失，而 UpdateChannel 是一个复合操作（Remove + Add），存在 TOCTOU 窗口。

**根因分析**：

并发测试的设计需要主动思考"哪些操作会在时间上交叉"，而不是简单地"把操作放到多个线程里跑"。M9-ext 的设计者想到了"多线程"，但没有想到"同一资源的竞争"。这是并发测试设计能力的问题，而不是执行问题。

**改进**：
- 并发测试设计清单：① 同一资源的并发读写；② 同一资源的并发写写；③ 复合操作（CRUD 组合）的并发；④ 遍历时的并发修改
- 在 Story 验收标准中，并发测试用例必须覆盖以上四类，不能只覆盖"多线程跑同一操作"

---

### P4：config.cpp 漏解析 option 字段——集成路径测试覆盖不足

**现象**：`gateway.yaml` 中插件以 Map 格式配置 `option` 时，`LoadConfig` 未读取该字段，导致 `config_file` 参数未传给 database 插件，`AddChannel` 报 `config_file not set`。这个 bug 在 Sprint 5 主体开发完成后才被发现。

**根因分析**：

`config.cpp` 的解析逻辑支持两种插件配置格式（字符串格式和 Map 格式），但 Map 格式的 `option` 字段解析被遗漏了。这是一个典型的**格式分支覆盖不完整**问题，与 Sprint 4 的 INT32 类型遗漏（L10）是同一模式。

更值得关注的是：这个 bug 在 `test_plugin_e2e.cpp` 中没有被发现，因为 E2E 测试直接调用 `DatabasePlugin::Option()`，绕过了 `config.cpp` 的解析路径。这说明**配置解析路径**是一个独立的集成测试盲区。

**改进**：
- 配置解析路径（`config.cpp` → `Option()` → 插件初始化）需要独立的集成测试
- 多格式配置（字符串/Map/列表）的解析，必须每种格式都有测试用例

---

## 三、尝试的新做法（Try）

| 做法 | 评估 | 结论 |
|------|------|------|
| 并发测试纳入 Story 验收标准 | 有效，Sprint 5 初始版本就有并发测试，优于 Sprint 4 事后补充 | 继续保持，作为标准验收条件 |
| `save_mutex_` 双锁分离（内存锁 vs 文件锁） | 有效，解决了 SaveToYaml 并发写竞态 | 继续保持，持久化操作独立加锁是正确模式 |
| ClickHouse 不可达时自动 SKIP | 有效，测试在无 ClickHouse 环境时不阻塞 CI | 继续保持，外部依赖测试必须有 SKIP 机制 |
| 测试完备性审查（独立于功能审查） | 有效，发现 15 个缺口，全部修复 | 继续保持，与代码审查同步进行 |
| `relation_adapters.h` 提取公共适配器 | 有效，消除 400 行重复代码，后续新驱动可直接复用 | 继续保持，驱动层公共逻辑应提取到共享头文件 |

---

## 四、行动项（Action Items）

| 优先级 | 行动项 | 负责人 | 截止 |
|--------|-------|--------|------|
| P0 | 密码/认证相关 Story，验收标准中明确"必须用真实校验密码的数据库测试" | Claude | Sprint 6 Planning |
| P0 | 持久化操作（SaveToYaml 类）的并发保护，在设计阶段单独分析，不能与内存操作混为一谈 | Claude | Sprint 6 每个 Story |
| P1 | 并发测试设计清单（同资源读写/写写/复合操作/遍历时修改）纳入 Story 验收标准模板 | Claude | Sprint 6 Planning |
| P1 | 配置解析路径（config.cpp → Option() → 插件初始化）补充集成测试，覆盖所有配置格式 | Claude | Sprint 6 开始前 |
| P2 | Story 6.7（MySQL 错误信息透传）纳入 Sprint 6 规划，接口改造影响面评估 | Claude | Sprint 6 Planning |
| P2 | Scheduler `/db-channels/*` 端点补充认证机制 | Claude | Sprint 6 |

---

## 五、经验教训固化

以下教训已提炼并更新到 `tasks/lessons.md`：

- **L14**：密码/认证相关测试禁止用 SQLite 替代，必须使用真实校验密码的数据库

以下教训在 Sprint 5 得到验证，继续有效：

- **L9**：集成测试必须走完整生产路径（config.cpp 漏解析 option 字段的 bug 再次印证）
- **L10**：测试数据必须覆盖所有声明支持的类型（config.cpp 多格式解析是同一模式）
- **L12**：涉及共享对象的接口必须有多线程并发测试（TQ-C3/C4 是并发测试设计不够深入的体现）

---

## 六、专家视角：Sprint 5 相对 Sprint 4 的进步与残留问题

### 进步

Sprint 4 的四个系统性缺陷，Sprint 5 有三个得到了实质性改善：

**L11（NDEBUG 禁用 assert）**：Sprint 5 的测试目标已显式加 `-UNDEBUG`，关键路径改用 `if` + `printf` + `exit(1)`，这个问题没有再出现。

**L12（并发测试缺失）**：Sprint 5 初始版本就有并发测试，不再是事后补充。这是工程习惯的真实改变，而不只是补了几个测试用例。

**L9（绕过插件层）**：Sprint 5 的所有新测试都走完整生产路径，没有出现直接 `#include` 驱动头文件的情况。

### 残留问题

**L14（替代品对被测特性无感知）**是 Sprint 5 新暴露的问题，是 Sprint 4 "模式二"（测试为了通过而非发现问题）的变体。Sprint 4 的表现是用 SQLite 替代 MySQL 测试写入路径；Sprint 5 的表现是用 SQLite 替代 MySQL/ClickHouse 测试密码认证路径。

两者的共同模式是：**选择了一个"更方便"的替代品，但这个替代品对被测的核心特性无感知，导致测试通过不能证明任何有意义的结论**。

这个模式的危险在于它很难被自动发现——测试会通过，CI 会绿，只有人工审查才能识别出"这个测试根本没有验证它声称要验证的东西"。

**根本对策**：在写测试之前，先问"如果被测特性出了 bug，这个测试会失败吗？"如果答案是"不会"，那这个测试是无效的，无论它通过多少次。

---

## 七、Sprint 5 质量数据

| 指标 | 数值 |
|------|------|
| 交付 Story 数 | 8（5.1、5.2、6.1~6.6） |
| 测试完备性审查发现缺口数 | 15 个（TQ-C1~C7、TQ-B1~B5、TQ-F1~F3） |
| 其中误判数 | 1 个（TQ-C6） |
| 实际修复缺口数 | 14 个 |
| 新增测试用例数 | test_clickhouse +5（T17~T21）、test_database_manager +11、test_plugin_e2e +2（P4、E5b） |
| 最终测试用例总数 | test_clickhouse: 21、test_database_manager: 20、test_plugin_e2e: E1-E7+P1-P4+E5b |
| 发现并修复的生产 bug | 2 个（SaveToYaml 并发写竞态、config.cpp 漏解析 option 字段） |
| 发现并修复的测试质量问题 | 1 个（TQ-B4 SQLite 替代密码认证路径） |
| 所有测试最终状态 | ✅ 全部通过（外部依赖不可达时自动 SKIP） |
