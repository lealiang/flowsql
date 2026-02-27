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
- 2026-02-26: 整理核心接口说明（IPlugin/IModule/IChannel/IOperator/IDataFrame/IDataEntity），写入 docs/stage2.md 架构章节和 README.md
- 2026-02-26: 实现模块 A（C++ ↔ Python 桥接）：ArrowIpcSerializer、PythonOperatorBridge、PythonProcessManager、BridgePlugin（C++ 端）；FastAPI Worker、OperatorBase 装饰器注册、OperatorRegistry 自动发现（Python 端）；httplib 第三方依赖；test_bridge 测试；修复 DataFrame::Finalize() 中 InitBuilders() 覆盖 batch_ 的 bug
- 2026-02-27: 讨论模块 B（Web 管理系统）需求分解：缩减范围聚焦 Python 算子执行链路；IChannel 重构（基类只保留生命周期+身份+元数据，数据读写下沉子类，dynamic_cast）；IDataEntity 转为元数据声明角色；Pipeline 单算子执行模型；SQL 语法设计（SELECT...FROM...USING...WITH...INTO）；DataFrameChannel 设计；更新 stage2.md
- 2026-02-27: 设计修正：Python算子输入/输出通道不限于DataFrame，新增IDatabaseChannel接口设计（IBatchReader/IBatchWriter工厂模式，Arrow IPC交换格式）；新增存储算子(system.store)和提取算子(system.extract)概念，格式转换职责交给算子而非通道；WITH子句承载写入配置（参考Spark/Flink SQL模式）；更新stage2.md
- 2026-02-27: 设计共识：IOperator::Work 签名从 Work(IDataFrame*, IDataFrame*) 改为 Work(IChannel*, IChannel*)，算子内部自行 dynamic_cast 到所需通道子类型并完成数据读写；Pipeline 简化为纯连接器，不参与数据读写；更新 stage2.md（IOperator 描述、数据流通路径、Pipeline 架构、存储/提取算子工作流、PythonOperatorBridge、实施步骤）
- 2026-02-27: 全文审视 stage2.md 自洽性，修正5处问题：(1)核心接口体系IChannel描述同步为新版（移除Put/Get）；(2)设计理由中"Pipeline做dynamic_cast"改为"算子做"；(3)存储/提取算子明确本阶段只实现框架骨架；(4)关键文件清单补充ioperator.h和python_operator_bridge；(5)Pipeline执行流程补充省略INTO时创建临时DataFrameChannel作为sink
- 2026-02-27: 验收视角审视 stage2.md，修正4处：(1)验证方案中test_framework/test_bridge标注需适配新接口；(2)新增端到端验收场景（具体SQL+期望结果）；(3)新增flowsql_web启动序列描述；(4)新增预填测试数据Schema定义
- 2026-02-27: 补充2处验收细节：(1)DataFrameChannel Read()快照语义/Write()替换语义；(2)新增验收场景2——Python算子上传激活完整链路
- 2026-02-27: 实现步骤3（IChannel重构+IOperator接口调整）：ichannel.h移除Put/Get新增Type/Schema（返回const char*）；ioperator.h Work签名改为Work(IChannel*,IChannel*)；新建idataframe_channel.h（IDataFrameChannel子接口）和idatabase_channel.h（IDatabaseChannel+IBatchReader/IBatchWriter仅接口）；新建DataFrameChannel内存实现（快照Read/替换Write）；适配MemoryChannel→IDataFrameChannel、PassthroughOperator和PythonOperatorBridge→Work(IChannel*,IChannel*)+dynamic_cast；Pipeline简化为纯连接器；更新test_framework新增DataFrameChannel测试；全部编译通过+测试通过
- 2026-02-27: 实现步骤4-6（SQL解析器+SQLite数据库层+Web后端API）：SQL递归下降解析器；SQLite amalgamation依赖+Database封装+参数化查询；Web后端全部API端点实现并测试通过；代码审查修复3个问题（路径穿越防护、mkdir冲突、JSON安全构造）
- 2026-02-27: 更新 stage2.md 进度：步骤3-6标记为已完成，更新已完成文件清单和需修改/新建文件状态
- 2026-02-27: DataFrameChannel Read/Write 效率优化：逐行复制改为 Arrow RecordBatch 零拷贝传递，Schema() 改为 Write 时缓存
