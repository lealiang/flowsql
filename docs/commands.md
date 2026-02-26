# 命令记录

| 日期 | 命令 |
|------|------|
| 2026-02-25 | CLAUDE.md 增加规则：将用户发起的每一条命令记录到 docs/commands.md |
| 2026-02-25 | 讨论 stage1.md 方案：IDataFrame 默认实现效率问题，要求提供高性能方案 |
| 2026-02-25 | 讨论 stage1.md：IDataFrame 默认实现效率问题，列举高效方案供选择 |
- 2026-02-25: 头文件防止重复包含的宏定义全部调整为 _FLOWSQL_当前目录_文件名_H_ 格式
- 2026-02-25: Stage 1 设计变更：DataFrame 改用 Arrow RecordBatch 存储，通信协议改为 REST + Arrow IPC
- 2026-02-25: 实现 Stage 1 全部代码
- 2026-02-25: 第三方依赖构建隔离：将 thirdparts 编译产物移到 build 目录外，避免每次清理重建
- 2026-02-25: 将 .thirdparts_installed 和 .thirdparts_prefix 加到 .gitignore
- 2026-02-25: 列举 stage1 已实现的能力
- 2026-02-25: 更新 docs/stage1.md 实施清单，标记已完成项
- 2026-02-25: 分析 docs/stage2.md 设计方案
- 2026-02-25: 更新 docs/stage2.md，新增模块 P（插件系统增强：三阶段加载 + 动态注册/注销）
- 2026-02-25: 更新 docs/stage2.md，PluginRegistry 重构为通用 IID 动态注册接口
- 2026-02-25: 提交上传代码（新增 stage2.md 设计文档）
- 2026-02-26: 清理 src/common/ 下 28 个未引用的头文件和源文件（死代码删除）
- 2026-02-26: 修复 test_npi 引用旧库名 libfast_npi2.0.so 的问题，改为 libflowsql_npi.so，并添加空指针防御
- 2026-02-26: CLAUDE.md 添加编程规则：尽量使用成熟的开源方案，不重复造轮子
- 2026-02-26: 安装 gh CLI (GitHub CLI) v2.87.3
- 2026-02-26: 更新 CLAUDE.md 架构章节，使其与实际代码一致
- 2026-02-26: 聚焦模块 P 分析问题和改进方案（7 个问题：Start 回滚、fntraverse 限制、类型安全、悬空指针、多 IPlugin Load、单例、继承链）
- 2026-02-26: 实现模块 P（插件系统增强）：loader.hpp 三阶段加载 + Start 回滚 + Unload 清理；PluginRegistry 单例 + 双层索引 + 动态注册/注销 + 统一 Traverse；适配 Service 和 test_framework + 新增动态注册测试
