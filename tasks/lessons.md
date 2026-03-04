# 经验教训

## L1: 不要为了探测元数据而消费数据流

**来源**：DatabaseChannel Phase 7 — SqliteDriver::CreateReader bug 修复

**问题**：`CreateReader` 中为推断列的 Arrow 类型，先调用 `sqlite3_step` 读取第一行判断类型，再 `sqlite3_reset` 重置。导致两个 bug：
1. `sqlite3_reset` 后带 WHERE 的查询返回空结果集
2. `sqlite3_step` 在 SQLITE_DONE 后自动 reset 重新执行，数据被重复读取

**原则**：当需要从有状态的游标/迭代器中获取元数据（Schema、列类型等）时，使用声明式 API（如 `sqlite3_column_decltype`）而非命令式 API（如 step 一行再 reset）。后者会改变游标状态，而状态恢复在不同查询条件下行为不一致，是隐蔽 bug 的温床。

**推广**：有状态对象（游标、流、迭代器）到达终态后，不要假设"再次调用会安全返回终态"。必须在应用层显式跟踪终态（如 `done_` 标志）。

---

## L2: 接口设计要充分考虑职责分离

**来源**：Sprint 1 回顾 — Pipeline 和 IChannel 接口重构

**问题**：
1. Pipeline 职责过重：负责 Channel ↔ DataFrame 转换，代码复杂度高
2. IChannel 接口混杂：生命周期、元数据和数据操作混在一起，难以扩展

**原则**：
- **单一职责原则**：每个接口只负责一件事
- **接口分层**：生命周期、元数据和数据操作分离
- **职责下沉**：通用逻辑上提，特定逻辑下沉

**解决方案**：
- Sprint 2 中将 Pipeline 重构为纯连接器，数据转换职责下沉
- IChannel 基类只保留生命周期和元数据，数据读写下沉子类（IDataFrameChannel/IDatabaseChannel）

---

## L3: 静态库单例问题

**来源**：Sprint 2 回顾 — PluginRegistry 单例失效

**问题**：libflowsql_framework.a 静态库链接到多个 .so（主程序和 bridge.so）时，每个 .so 有独立的 PluginRegistry 单例实例，导致插件注册失效。

**原则**：
- **避免静态库单例**：静态库链接到多个 .so 时单例失效
- **优先使用共享库**：或回归纯插件架构

**解决方案**：
- Sprint 3 中删除 PluginRegistry 和 libflowsql_framework.so，回归纯 PluginLoader 架构
- Pipeline/ChannelAdapter 移入 scheduler.so，避免跨 .so 共享状态

---

## L4: 测试要与功能开发同步

**来源**：Sprint 1 回顾 — 测试覆盖率不足

**问题**：初期聚焦功能实现，忽略测试，导致代码质量无法保证。

**原则**：
- **TDD（测试驱动开发）**：先写测试，再写实现
- **测试覆盖率**：单元测试覆盖率 > 80%，集成测试覆盖核心路径
- **测试金字塔**：单元测试 > 集成测试 > 端到端测试

**改进措施**：
- 每个 Story 完成后立即补充测试
- 使用工具辅助（valgrind、AddressSanitizer）

---

## L5: 第三方依赖要隔离

**来源**：Sprint 1 回顾 — Arrow 依赖管理

**问题**：第三方依赖混在 build 目录中，每次清理重建耗时长。

**原则**：
- **依赖隔离**：第三方依赖隔离到 build 目录外
- **缓存编译产物**：避免重复编译
- **三级回退**：pyarrow/缓存/源码编译

**解决方案**：
- 将 .thirdparts_installed 和 .thirdparts_prefix 移到项目根目录
- arrow-config.cmake 支持三级回退

---

## L6: 资源泄漏要及时发现

**来源**：Sprint 2 回顾 — Python Worker 资源泄漏

**问题**：Python Worker 资源泄漏（文件描述符、socket、线程）在后期才发现，增加返工成本。

**原则**：
- **每个 Story 完成后立即进行代码审查**
- **重点关注资源泄漏、线程安全、错误处理**
- **使用工具辅助**（valgrind、AddressSanitizer、lsof）

**改进措施**：
- 建立代码审查流程
- 使用 RAII 管理资源（智能指针、守卫类）

---

## L7: 架构问题要及早发现和解决

**来源**：Sprint 3 回顾 — PluginRegistry 单例问题

**问题**：PluginRegistry 单例问题在 Sprint 2 就存在，但直到 Sprint 3 才解决，增加返工成本。

**原则**：
- **及早发现架构问题**：代码审查时重点关注架构设计
- **果断重构**：发现架构问题时，不要犹豫，果断重构
- **避免技术债务累积**：每个 Sprint 都要清理技术债务

**改进措施**：
- 在 Sprint Planning 时识别架构风险
- 在 Sprint Review 时识别技术债务
- 在 Sprint Retrospective 时制定改进计划

---

## L8: 文档要与代码同步

**来源**：Sprint 3 回顾 — 文档更新滞后

**问题**：文档更新滞后，与实际代码不一致，新人上手困难。

**原则**：
- **文档与代码同步**：每个 Story 完成后立即更新文档
- **文档要准确**：确保文档与实际代码一致
- **文档要完整**：用户手册、开发者指南、API 文档都要完善

**改进措施**：
- 将文档更新纳入 Story 的验收标准
- 在 Sprint Review 时检查文档完整性

