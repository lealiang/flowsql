# 经验教训

## L1: 不要为了探测元数据而消费数据流

**来源**：DatabaseChannel Phase 7 — SqliteDriver::CreateReader bug 修复

**问题**：`CreateReader` 中为推断列的 Arrow 类型，先调用 `sqlite3_step` 读取第一行判断类型，再 `sqlite3_reset` 重置。导致两个 bug：
1. `sqlite3_reset` 后带 WHERE 的查询返回空结果集
2. `sqlite3_step` 在 SQLITE_DONE 后自动 reset 重新执行，数据被重复读取

**原则**：当需要从有状态的游标/迭代器中获取元数据（Schema、列类型等）时，使用声明式 API（如 `sqlite3_column_decltype`）而非命令式 API（如 step 一行再 reset）。后者会改变游标状态，而状态恢复在不同查询条件下行为不一致，是隐蔽 bug 的温床。

**推广**：有状态对象（游标、流、迭代器）到达终态后，不要假设"再次调用会安全返回终态"。必须在应用层显式跟踪终态（如 `done_` 标志）。
