# FlowSQL 产品待办列表

## Epic 1: C++ 框架核心能力
**优先级**: P0 | **状态**: ✅ 已完成 (Sprint 1)
**价值**: 建立插件式进程框架基础，支持 C++ 和 Python 算子统一数据交换

### Story 1.1: 统一数据接口设计
**状态**: ✅ 已完成 (Sprint 1)
**验收标准**: IDataEntity 和 IDataFrame 接口完成，支持 JSON 和 Arrow IPC 序列化

<details>
<summary>任务分解（点击展开）</summary>

- ✅ 定义 DataType 枚举和 FieldValue variant
- ✅ 实现 IDataEntity 接口（GetSchema/GetFieldValue/SetFieldValue/ToJson/FromJson/Clone）
- ✅ 实现 IDataFrame 接口（行列操作/Arrow 互操作/序列化）
- ✅ 实现 DataFrame 类（两阶段模式：构建期 builders_ → 读取期 batch_）
</details>

---

### Story 1.2: 通道和算子接口
**状态**: ✅ 已完成 (Sprint 1)
**验收标准**: IChannel 和 IOperator 接口定义完成，支持插件化架构

<details>
<summary>任务分解（点击展开）</summary>

- ✅ 定义 IChannel 接口（继承 IPlugin，含 Catelog/Name/Open/Close/Put/Get/Flush）
- ✅ 定义 IOperator 接口（继承 IPlugin，含 Work/Configure/Position）
- ✅ 定义 OperatorPosition 枚举（STORAGE/DATA）
- ✅ 实现注册宏（framework/macros.h）
</details>

---

### Story 1.3: 核心框架实现
**状态**: ✅ 已完成 (Sprint 1)
**验收标准**: PluginRegistry、Pipeline 和 Service 核心框架实现完成

<details>
<summary>任务分解（点击展开）</summary>

- ✅ 实现 PluginRegistry（LoadPlugin/UnloadAll/GetChannel/GetOperator/Traverse）
- ✅ 实现 Pipeline（Run 循环：批量读取 → Work → 批量写入）
- ✅ 实现 PipelineBuilder（链式构建：AddSource/SetOperator/SetSink/SetBatchSize/Build）
- ✅ 实现 Service 主框架
- ✅ Pipeline 状态机（IDLE/RUNNING/STOPPED/FAILED）
</details>

---

### Story 1.4: 示例插件和测试
**状态**: ✅ 已完成 (Sprint 1)
**验收标准**: MemoryChannel 和 PassthroughOperator 实现，test_framework 5 个用例全部通过

<details>
<summary>任务分解（点击展开）</summary>

- ✅ 实现 MemoryChannel（std::queue 存储，Put/Get/Open/Close）
- ✅ 实现 PassthroughOperator（直接复制输入到输出）
- ✅ 编写框架集成测试（插件加载/DataFrame 操作/Pipeline 数据流通）
- ✅ 验证现有 NPI 插件不受影响
</details>

---

### Story 1.5: Apache Arrow 集成
**状态**: ✅ 已完成 (Sprint 1)
**验收标准**: Arrow 依赖集成完成，支持 IPC 和 JSON 模块

<details>
<summary>任务分解（点击展开）</summary>

- ✅ 编写 arrow-config.cmake（ExternalProject_Add）
- ✅ 配置编译选项（ARROW_BUILD_STATIC/ARROW_IPC/ARROW_JSON）
- ✅ CMakeLists.txt 集成（add_thirddepen 宏）
- ✅ 验证编译和链接
</details>

---

## Epic 2: Python 算子桥接与 Web 管理
**优先级**: P0 | **状态**: ✅ 已完成 (Sprint 2)
**价值**: 实现 C++ ↔ Python 桥接，提供 Web 管理界面，支持 Python 算子动态上传和执行

### Story 2.1: 插件系统增强（模块 P）
**状态**: ✅ 已完成 (Sprint 2)
**验收标准**: 三阶段加载、Start 失败回滚、动态注册/注销、单例模式

<details>
<summary>任务分解（点击展开）</summary>

- ✅ PluginLoader 改造（GetInterfaces/StartModules/StopModules/Unload 清理）
- ✅ PluginRegistry 重构（单例/双层索引/统一 Traverse/动态注册）
- ✅ 适配 Service 和 test_framework
- ✅ 新增 test_dynamic_register 验证
</details>

---

### Story 2.2: C++ ↔ Python 桥接（模块 A）
**状态**: ✅ 已完成 (Sprint 2)
**验收标准**: PythonOperatorBridge 实现，Python Worker 启动，算子自动发现和注册

<details>
<summary>任务分解（点击展开）</summary>

- ✅ 实现 ArrowIpcSerializer（使用 arrow::ipc::MakeStreamWriter）
- ✅ 实现 PythonOperatorBridge（Work 方法：Read → Serialize → HTTP POST → Deserialize → Write）
- ✅ 实现 PythonProcessManager（Start/WaitReady/Stop/IsAlive）
- ✅ 实现 BridgePlugin（IPlugin + IModule，动态注册 Python 算子）
- ✅ 实现 Python Worker（FastAPI + uvicorn）
- ✅ 实现 Python 算子框架（OperatorBase/OperatorRegistry/arrow_codec.py）
- ✅ 编写 test_bridge 验证
</details>

---

### Story 2.3: IChannel 重构
**状态**: ✅ 已完成 (Sprint 2)
**验收标准**: IChannel 基类只保留生命周期和元数据，数据读写下沉子类

<details>
<summary>任务分解（点击展开）</summary>

- ✅ 重构 IChannel 接口（保留 Open/Close/Flush/Catelog/Name/Type/Schema）
- ✅ 定义 IDataFrameChannel 子接口
- ✅ 定义 IDatabaseChannel + IBatchReader/IBatchWriter 接口
- ✅ 实现 DataFrameChannel（std::shared_ptr<arrow::RecordBatch> + 互斥锁）
- ✅ 修改 IOperator::Work 签名为 Work(IChannel*, IChannel*)
- ✅ 适配所有现有插件和算子
- ✅ 删除 framework/macros.h（未使用的死代码）
</details>

---

### Story 2.4: SQL 解析器 + Pipeline 重构
**状态**: ✅ 已完成 (Sprint 2)
**验收标准**: SQL 解析器支持 SELECT...FROM...USING...WITH...INTO 语法

<details>
<summary>任务分解（点击展开）</summary>

- ✅ 实现 SqlParser（递归下降解析器）
- ✅ 支持 SELECT/FROM/USING/WITH/INTO 关键字解析
- ✅ 支持参数解析（key=value 格式）
- ✅ Pipeline 重构为纯连接器
- ✅ 集成测试验证
</details>

---

### Story 2.5: Web 后端 API
**状态**: ✅ 已完成 (Sprint 2)
**验收标准**: SQLite 数据库封装，WebServer 实现，全部 API 端点完成

<details>
<summary>任务分解（点击展开）</summary>

- ✅ 实现 SQLite 封装（Database 类：Open/Close/Execute/Query/Insert/ExecuteParams）
- ✅ 编写 schema.sql（建表 DDL）
- ✅ 实现 WebServer（httplib Server + 所有 Handler）
- ✅ 实现 GET /api/health
- ✅ 实现 GET /api/channels（从 PluginRegistry 同步）
- ✅ 实现 GET /api/operators
- ✅ 实现 POST /api/operators/upload（路径穿越防护）
- ✅ 实现 POST /api/operators/{name}/activate|deactivate
- ✅ 实现 GET /api/tasks
- ✅ 实现 POST /api/tasks（SQL 解析 → Pipeline 执行）
- ✅ 实现 GET /api/tasks/{id}/result
- ✅ 实现 flowsql_web 入口（main.cpp：加载插件 → 预填测试数据 → 启动 WebServer）
</details>

---

### Story 2.6: Vue.js 前端
**状态**: ✅ 已完成 (Sprint 2)
**验收标准**: Vue 3 + Vite + Element Plus，Dashboard/Channels/Operators/Tasks 页面完成

<details>
<summary>任务分解（点击展开）</summary>

- ✅ 项目脚手架搭建（Vue 3 + Vite + Element Plus）
- ✅ 实现 Dashboard.vue
- ✅ 实现 Channels.vue
- ✅ 实现 Operators.vue
- ✅ 实现 Tasks.vue
- ✅ 实现 Sidebar.vue 导航组件
- ✅ 实现 API 封装（api/index.js）
- ✅ 实现 Vue Router 配置
- ✅ 编写构建脚本和测试脚本
</details>

---

### Story 2.7: 端到端集成测试
**状态**: ✅ 已完成 (Sprint 2)
**验收标准**: Python 算子完整执行链路验证，算子上传激活功能验证

<details>
<summary>任务分解（点击展开）</summary>

- ✅ 编写端到端测试脚本
- ✅ 验证 Python 算子执行链路
- ✅ 验证多 SQL 串联
- ✅ 验证算子上传激活
- ✅ 回归测试现有功能
</details>

---

## Epic 3: 数据库闭环与架构清理
**优先级**: P0 | **状态**: ✅ 已完成 (Sprint 3)
**价值**: 补齐数据库读写闭环，清理架构债务，回归纯插件架构

### Story 3.1: 架构重构 - 纯插件架构回归
**状态**: ✅ 已完成 (Sprint 3)
**验收标准**: 删除 PluginRegistry 和 libflowsql_framework.so，回归纯 PluginLoader 架构

<details>
<summary>任务分解（点击展开）</summary>

- ✅ 拆分 common/iplugin.h（从 framework 拆出）
- ✅ 修改 common/loader.hpp（实现 IRegister + IQuerier）
- ✅ 新增 common/iquerier.hpp（插件查询接口）
- ✅ 删除 PluginRegistry 相关文件
- ✅ 移动 Pipeline/ChannelAdapter 到 scheduler.so
- ✅ 清理 framework 库依赖
</details>

---

### Story 3.2: 接口解耦
**状态**: ✅ 已完成 (Sprint 3)
**验收标准**: IChannel/IOperator 去掉 IPlugin 继承，纯接口设计

<details>
<summary>任务分解（点击展开）</summary>

- ✅ 修改 framework/interfaces/ichannel.h（去掉 IPlugin 继承）
- ✅ 修改 framework/interfaces/ioperator.h（去掉 IPlugin 继承）
- ✅ 适配所有实现类（显式多继承）
- ✅ 验证编译和测试
</details>

---

### Story 3.3: IBridge 接口
**状态**: ✅ 已完成 (Sprint 3)
**验收标准**: 替代 dynamic_cast<IRegister*> hack，提供 Python 算子查询和刷新能力

<details>
<summary>任务分解（点击展开）</summary>

- ✅ 定义 framework/interfaces/ibridge.h
- ✅ 修改 services/bridge/bridge_plugin.h（多继承 IBridge）
- ✅ 实现 FindOperator/TraverseOperators/Refresh 方法
- ✅ Scheduler 集成 IBridge 查询
- ✅ Web 端实现算子刷新 API
</details>

---

### Story 3.4: arrow_codec.py 统一转换层
**状态**: ✅ 已完成 (Sprint 3)
**验收标准**: 兼容 Polars/Pandas/Arrow Table，简化 Python 算子开发

<details>
<summary>任务分解（点击展开）</summary>

- ✅ 实现 _ensure_arrow_table() 函数
- ✅ 支持 Polars/Pandas/Arrow 自动检测和转换
- ✅ 集成到 Python Worker
- ✅ 更新示例算子
</details>

---

### Story 3.5: IDatabaseFactory 工厂接口 + DatabasePlugin
**状态**: ✅ 已完成 (Sprint 3)
**验收标准**: 数据库通道工厂实现，支持多数据库实例管理

<details>
<summary>任务分解（点击展开）</summary>

- ✅ 定义 framework/interfaces/idatabase_factory.h
- ✅ 实现 services/database/database_plugin.h/.cpp
- ✅ 实现配置解析（Option() 方法）
- ✅ 实现环境变量替换
- ✅ 实现 Get/List/Release 方法
</details>

---

### Story 3.6: IDbDriver 驱动抽象 + SQLite 驱动
**状态**: ✅ 已完成 (Sprint 3)
**验收标准**: 数据库驱动抽象层和 SQLite 驱动实现完成

<details>
<summary>任务分解（点击展开）</summary>

- ✅ 定义 services/database/idb_driver.h
- ✅ 实现 services/database/drivers/sqlite_driver.h/.cpp
- ✅ 实现 SqliteBatchReader（流式读取 + Arrow IPC 序列化）
- ✅ 实现 SqliteBatchWriter（IPC 反序列化 + 自动建表 + 批量写入）
- ✅ 支持只读模式和 WAL 模式
</details>

---

### Story 3.7: DatabaseChannel 通道实现
**状态**: ✅ 已完成 (Sprint 3)
**验收标准**: IDatabaseChannel 接口实现，委托所有数据库操作给 IDbDriver

<details>
<summary>任务分解（点击展开）</summary>

- ✅ 实现 services/database/database_channel.h/.cpp
- ✅ 实现 Open/Close/IsOpened 方法
- ✅ 实现 CreateReader/CreateWriter 委托
- ✅ 实现元数据方法（Type/Catelog/Name）
</details>

---

### Story 3.8: SQL WHERE 解析 + DataFrame Filter
**状态**: ✅ 已完成 (Sprint 3)
**验收标准**: WHERE 子句解析和 DataFrame 过滤能力实现

<details>
<summary>任务分解（点击展开）</summary>

- ✅ SqlParser 新增 WHERE 解析
- ✅ 实现 ValidateWhereClause()（SQL 注入防护）
- ✅ 实现 DataFrame::Filter()（支持 6 种操作符）
- ✅ 支持多种数据类型比较
- ✅ 集成测试验证
</details>

---

### Story 3.9: 安全基线
**状态**: ✅ 已完成 (Sprint 3)
**验收标准**: SQL 注入防护、只读模式和环境变量替换实现

<details>
<summary>任务分解（点击展开）</summary>

- ✅ 实现 WHERE 子句注入防护
- ✅ 实现 SQLite 只读模式
- ✅ 实现环境变量替换
- ✅ 安全测试验证
</details>

---

### Story 3.10: Scheduler 集成
**状态**: ✅ 已完成 (Sprint 3)
**验收标准**: Scheduler 集成数据库通道，支持四层查找和 SQL 生成

<details>
<summary>任务分解（点击展开）</summary>

- ✅ 实现 FindChannel() 四层查找
- ✅ 支持三段式和两段式通道引用
- ✅ 实现 BuildQuery()（SQL 生成）
- ✅ 集成测试验证
</details>

---

### Story 3.11: ChannelAdapter 自动适配
**状态**: ✅ 已完成 (Sprint 3)
**验收标准**: 去掉显式存储/提取算子，Pipeline/Scheduler 层自动感知通道类型差异

<details>
<summary>任务分解（点击展开）</summary>

- ✅ 实现 ChannelAdapter 工具类
- ✅ 实现 4 种无算子路径
- ✅ 实现有算子时的自动适配
- ✅ 修改 SQL 解析器（USING 可选）
- ✅ 集成测试验证
</details>

---

### Story 3.12: 端到端测试
**状态**: ✅ 已完成 (Sprint 3)
**验收标准**: 14 项端到端测试全部通过

<details>
<summary>任务分解（点击展开）</summary>

- ✅ 编写配置解析测试
- ✅ 编写 SQLite 连接测试
- ✅ 编写 Reader/Writer 测试
- ✅ 编写 SQL 解析器测试
- ✅ 编写 DataFrame Filter 测试
- ✅ 编写安全基线测试
- ✅ 编写 6 个 E2E 场景测试
</details>

---

### Story 3.13: 代码审查修复
**状态**: ✅ 已完成 (Sprint 3)
**验收标准**: 修复代码审查中发现的 9 个问题（P1×6 + P2×3）

<details>
<summary>任务分解（点击展开）</summary>

- ✅ 修复 P1 问题（6 项）
- ✅ 修复 P2 问题（3 项）
- ✅ 回归测试验证
</details>

---

### Story 3.14: 清理任务
**状态**: ✅ 已完成 (Sprint 3)
**验收标准**: 删除 IDataEntity 相关死代码

<details>
<summary>任务分解（点击展开）</summary>

- ✅ 删除 idata_entity.h
- ✅ DataType/FieldValue/Field 移入 idataframe.h
- ✅ 清理 DataFrame 中 AppendEntity/GetEntity
- ✅ 清理 test_bridge 无用 include
- ✅ 更新 stage3.md 文档
</details>

---

## Epic 4: 多数据库支持与 SQL 增强
**优先级**: P1 | **状态**: ✅ 已完成（4.1/4.3/4.4：Sprint 4；4.2：Sprint 11）
**价值**: 扩展数据库支持，实现 MySQL 驱动和连接池，增强 SQL 能力

### Story 4.1: MySQL 驱动支持
**状态**: ✅ 已完成 (Sprint 4)
**验收标准**:
- 实现 MysqlDriver（基于 libmysqlclient）✅
- 支持连接池管理 ✅
- 支持事务控制（COMMIT/ROLLBACK）✅
- 端到端测试通过（19 个测试用例）✅

---

### Story 4.2: PostgreSQL 驱动支持
**状态**: ✅ 已完成（Sprint 11，2026-03-26）
**验收标准**:
- 实现 PostgresDriver（基于 libpq）✅
- 支持连接池管理 ✅
- 使用 `PQexec` 简单执行路径（prepared statement 不作为本 Story 前置）✅
- 支持事务控制 ✅
- 支持 Web browse `schema` 参数（默认 `public`）✅
- 驱动测试 + 插件层 E2E 通过（含并发）✅

---

### Story 4.3: 数据库连接池基础实现
**状态**: ✅ 已完成 (Sprint 4)
**验收标准**:
- ConnectionPool<T> 泛型连接池 ✅
- 支持连接复用和空闲超时回收 ✅
- 支持最大连接数限制 ✅
- 支持健康检查（心跳机制）✅
- 连接池单元测试通过（7 个测试用例）✅

---

### Story 4.4: SQL 高级特性
**状态**: ✅ 已完成 (Sprint 4)
**验收标准**:
- 支持 GROUP BY 和聚合函数 ✅
- 支持 ORDER BY 和 LIMIT ✅
- 支持子查询（透传给数据库引擎）✅
- sql_part 提取与 BuildQuery 替换逻辑 ✅

---

## Epic 5: ClickHouse 数据库通道
**优先级**: P1 | **状态**: ✅ 已完成（2026-03-12）
**价值**: 支持列式数据库 ClickHouse，走 Arrow 原生路径实现高效批量读写

### Story 5.1: ClickHouseDriver 核心实现
**状态**: ✅ 已完成（2026-03-12）
**验收标准**:
- 实现 ClickHouseDriver（基于 httplib，HTTP 8123 端口，零新依赖）
- 实现 ClickHouseSession，同时实现 IArrowReadable + IArrowWritable
- 查询走 `FORMAT ArrowStream`，写入走 `INSERT INTO table FORMAT ArrowStream`
- 认证：`X-ClickHouse-User` / `X-ClickHouse-Key` header
- 配置格式：`type=clickhouse;name=ch1;host=...;port=8123;user=...;password=...;database=...`

---

### Story 5.2: DatabasePlugin 集成 ClickHouse
**状态**: ✅ 已完成（2026-03-12）
**验收标准**:
- `CreateDriver()` 增加 clickhouse 分支
- `DatabaseChannel::CreateArrowReader/CreateArrowWriter` 从返回 -1 改为 dynamic_cast 检查 IArrowReadable/IArrowWritable
- 现有 SQLite/MySQL 测试不受影响

---

### Story 5.3: 端到端测试
**状态**: ✅ 已完成（2026-03-12）
**验收标准**:
- 新增 test_clickhouse.cpp，覆盖连接、DDL、写入、读取、Arrow 类型矩阵
- ClickHouse 不可达时自动 SKIP
- 所有现有测试回归通过（T1-T16 全部通过）
- test_plugin_e2e.cpp 补充 E1-E7 插件层 E2E（PluginLoader → IDatabaseFactory → IDatabaseChannel 完整路径），全部通过

---

## Epic 6: Web 管理数据库通道
**优先级**: P1 | **状态**: ✅ 已完成（2026-03-12）
**价值**: 将数据库通道配置从 gateway.yaml 静态配置迁移到 Web 动态管理，支持运行时增删改

### 设计决策
- **配置权威方**：DatabasePlugin 自持久化（写自己的 SQLite 文件），Web 服务只是操作入口
- **密码存储**：AES-256-GCM 加密后存储，密钥从环境变量 `FLOWSQL_SECRET_KEY` 读取
- **跨进程通信**：Web → Gateway → Scheduler → DatabasePlugin，与现有架构一致
- **废弃**：`gateway.yaml` 中的 `databases:` 数组

### Story 6.1: DatabasePlugin 持久化与动态管理
**状态**: ✅ 已完成（2026-03-12）
**验收标准**:
- 新增 `IDatabaseManager` 接口：`AddChannel` / `RemoveChannel` / `UpdateChannel` / `ListChannels`
- `Start()` 从 SQLite 文件加载已保存的通道配置
- 密码字段 AES-256-GCM 加密存储，读取时解密
- `AddChannel()` 后无需重启即可使用新通道

---

### Story 6.2: Scheduler 新增管理端点
**状态**: ✅ 已完成（2026-03-12）
**验收标准**:
- 新增 `POST /db-channels/add`、`/db-channels/remove`、`/db-channels/update`、`GET /db-channels`
- 通过 IQuerier 找到 IDatabaseManager 并调用对应方法

---

### Story 6.3: Web 服务 CRUD API
**状态**: ✅ 已完成（2026-03-12）
**验收标准**:
- 新增 `GET/POST/PUT/DELETE /api/db-channels` 端点
- 密码字段前端展示脱敏（显示 `****`）
- 操作后通过 Gateway 通知 Scheduler

---

### Story 6.4: 废弃 gateway.yaml 静态配置
**状态**: ✅ 已完成（2026-03-12）
**验收标准**:
- 删除 `gateway.yaml` 中的 `databases:` 数组
- DatabasePlugin option 改为 `db_path=/tmp/flowsql_db_channels.db`
- 向后兼容：旧配置格式保留解析能力但不再生成

---

### Story 6.5: 前端通道管理 UI
**状态**: ✅ 已完成（2026-03-12）
**验收标准**:
- `Channels.vue` 新增数据库通道增删改对话框
- 支持 SQLite / MySQL / ClickHouse 三种类型，动态显示对应字段
- 密码字段 `type="password"`，展示时脱敏

---

### Story 6.6: 端到端测试
**状态**: ✅ 已完成（2026-03-12）
**验收标准**:
- Web UI 新增通道 → Scheduler 立即可用 → 重启后配置持久化
- 删除通道后 SQL 执行返回 "channel not found"

---

### Story 6.7: 数据库错误信息透传架构改造
**状态**: ✅ 已完成 (Sprint 6)
**优先级**: P1
**背景**: `IBatchReadable::CreateReader` 接口无 `error*` 参数，底层驱动（MySQL/ClickHouse）的错误信息（如 `No database selected`、`Table doesn't exist`）在 `RelationDbSessionBase::CreateReader` 中被捕获后丢弃，调用方只能看到 `CreateReader failed`，调试成本高。
**验收标准**:
- `IBatchReadable::CreateReader` 接口增加错误输出参数（或等效机制），将底层错误字符串透传给调用方
- MySQL / ClickHouse / SQLite 三个驱动均实现透传
- Scheduler 层将具体错误信息返回给 HTTP 调用方（而非通用失败消息）
- 新增测试：构造"数据库不存在"、"表不存在"场景，断言错误消息包含具体原因

<details>
<summary>设计要点（点击展开）</summary>

- 方案 A：`CreateReader(error_out*)` 参数扩展——接口侵入性最小，但需修改所有实现
- 方案 B：`thread_local` 错误槽（类似 `errno`）——零接口变更，但跨线程语义需谨慎
- 方案 C：返回 `Result<Reader, Error>` 包装类型——最符合现代 C++ 风格，但改动面最大
- 推荐在规划时评估三种方案的影响范围后决策

</details>

---

## Epic 7: 路由代理与服务对等化架构改造
**优先级**: P1 | **状态**: ✅ 已完成 (Sprint 6)
**价值**: 解耦插件路由、实现服务对等、统一 API 设计，为后续扩展奠定架构基础
**设计文档**: `docs/design_router_agency.md`

### Story 7.1: 基础设施 — IRouterHandle 接口 + 错误码 + PluginLoader 批次调用
**状态**: ✅ 已完成 (Sprint 6)
**验收标准**:
- 定义 `irouter_handle.h`（IRouterHandle + RouteItem + fnRouterHandler）
- 定义 `error_code.h`（6 个业务错误码 + HttpStatus 映射）
- 修正 `main.cpp` RunService 为两阶段加载（Load 全部 → StartAll）

<details>
<summary>任务分解（点击展开）</summary>

- 📋 新增 `src/framework/interfaces/irouter_handle.h`：IRouterHandle 接口 + RouteItem + fnRouterHandler 签名
- 📋 新增 `src/common/error_code.h`：OK/BAD_REQUEST/NOT_FOUND/CONFLICT/INTERNAL_ERROR/UNAVAILABLE
- 📋 修正 `src/app/main.cpp` RunService：分离 Load 阶段和 Start 阶段，确保所有插件 Load() 完成后再统一 StartAll()
- 📋 单元测试：验证两阶段加载顺序正确
</details>

---

### Story 7.2: RouterAgencyPlugin 实现
**状态**: ✅ 已完成 (Sprint 6)
**验收标准**:
- 实现 libflowsql_router.so（RouteCollector + HttpServer + GatewayRegistrar + ErrorMapper）
- 路由收集（Traverse IRouterHandle）+ 冲突检测（先到先得 + 日志告警）
- HTTP Dispatch + CORS 统一处理 + 错误码→HTTP 状态码映射
- KeepAlive 线程（定期向 Gateway 注册路由前缀，幂等）

<details>
<summary>任务分解（点击展开）</summary>

- 📋 新增 `src/services/router/` 目录：router_agency_plugin.h/cpp + plugin_register.cpp + CMakeLists.txt
- 📋 实现 RouteCollector：Traverse(IID_ROUTER_HANDLE) 收集路由 + 冲突检测 + 前缀提取
- 📋 实现 HttpServer：httplib::Server + catch-all Dispatch + CORS 统一处理
- 📋 实现 GatewayRegistrar：KeepAlive 线程 + POST /gateway/register
- 📋 实现 ErrorMapper：业务错误码 → HTTP 状态码映射
- 📋 集成测试：路由收集、分发、CORS、错误码映射
</details>

---

### Story 7.3: Gateway 改造 — 字典树路由 + 过期清理 + 瘦身
**状态**: ✅ 已完成 (Sprint 6)
**验收标准**:
- RouteTable 改为字典树（Trie）匹配，废弃 ExtractPrefix/StripPrefix
- HandleForward 不再剥离前缀，转发完整 URI
- 增加路由过期清理线程（CleanupThread），移除超过 3 倍 KeepAlive 间隔未更新的路由
- 删除 ServiceManager、HeartbeatThread、HandleHeartbeat
- 删除 ServiceClient

<details>
<summary>任务分解（点击展开）</summary>

- 📋 重写 `route_table.h/cpp`：字典树实现（Insert/Match/RemoveExpired），RouteEntry 增加 last_seen_ms
- 📋 修改 `gateway_plugin.cpp`：HandleForward 转发完整 URI，不剥离前缀
- 📋 新增 CleanupThread：定期移除过期路由条目
- 📋 新增 `/gateway/register`、`/gateway/unregister`、`/gateway/routes` 端点
- 📋 删除 ServiceManager、HeartbeatThread、HandleHeartbeat、ServiceClient
- 📋 测试：字典树匹配、路由注册/过期清理、转发完整 URI
</details>

---

### Story 7.4: 业务插件迁移 — SchedulerPlugin
**状态**: ✅ 已完成 (Sprint 6)
**验收标准**:
- 实现 IRouterHandle，声明 /channels/dataframe/*、/operators/*、/tasks/instant/*
- 删除内部 httplib::Server 和 RegisterRoutes()
- 删除 /db-channels/* 相关 Handler（移交 DatabasePlugin）

<details>
<summary>任务分解（点击展开）</summary>

- 📋 SchedulerPlugin 多继承 IRouterHandle，实现 EnumRoutes()
- 📋 将现有 Handler 改为 fnRouterHandler 签名（uri, req_json, rsp_json）
- 📋 删除 httplib::Server server_ 成员和 RegisterRoutes()
- 📋 删除 /db-channels/* 相关 Handler
- 📋 plugin_register.cpp 增加 IID_ROUTER_HANDLE 注册
- 📋 回归测试：所有现有 API 功能不受影响
</details>

---

### Story 7.5: 业务插件迁移 — DatabasePlugin
**状态**: ✅ 已完成 (Sprint 6)
**验收标准**:
- 实现 IRouterHandle，声明 /channels/database/*
- Handler 逻辑从 SchedulerPlugin 迁移过来

<details>
<summary>任务分解（点击展开）</summary>

- 📋 DatabasePlugin 多继承 IRouterHandle，实现 EnumRoutes()
- 📋 实现 HandleAdd/HandleRemove/HandleModify/HandleQuery
- 📋 plugin_register.cpp 增加 IID_ROUTER_HANDLE 注册
- 📋 测试：数据库通道 CRUD 通过 RouterAgencyPlugin 分发正常工作
</details>

---

### Story 7.6: 业务插件迁移 — WebPlugin
**状态**: ✅ 已完成 (Sprint 6)
**验收标准**:
- 实现 IRouterHandle（管理 API 部分）
- 保留 Web 服务器（静态文件），管理 API 走 RouterAgencyPlugin 内部端口

<details>
<summary>任务分解（点击展开）</summary>

- 📋 WebPlugin 多继承 IRouterHandle，实现 EnumRoutes()（管理 API）
- 📋 保留 httplib::Server 用于静态文件服务
- 📋 管理 API Handler 改为 fnRouterHandler 签名
- 📋 测试：Web UI 正常访问，管理 API 通过 RouterAgencyPlugin 正常工作
</details>

---

### Story 7.7: 进程管理改造 — fork 守护 + docker-compose
**状态**: ✅ 已完成 (Sprint 6)
**验收标准**:
- main.cpp RunGateway → RunGuardian（极简 fork + waitpid + respawn）
- 编写 Dockerfile + docker-compose.yml + docker-compose.full.yml
- 更新 gateway.yaml 配置格式（per-plugin option）

<details>
<summary>任务分解（点击展开）</summary>

- 📋 重写 main.cpp RunGateway 为 RunGuardian：fork + waitpid + respawn，零业务逻辑
- 📋 编写 Dockerfile（基于 ubuntu:22.04，同一镜像多角色）
- 📋 编写 docker-compose.yml（gateway + web + scheduler + pyworker）
- 📋 编写 docker-compose.full.yml（含 MySQL + ClickHouse）
- 📋 更新 gateway.yaml 配置格式
- 📋 测试：fork 守护进程管理、docker-compose 部署验证
</details>

---

### Story 7.8: 端到端验证
**状态**: ✅ 已完成 (Sprint 6)
**验收标准**:
- 路由收集和分发测试
- KeepAlive + Gateway 故障恢复测试
- 资源导向 URI 全路由回归测试
- docker-compose 部署验证

<details>
<summary>任务分解（点击展开）</summary>

- 📋 路由收集测试：多插件路由收集、冲突检测、前缀提取
- 📋 路由分发测试：完整 URI 转发、错误码映射、CORS
- 📋 KeepAlive 测试：正常注册、Gateway 重启恢复、服务崩溃路由过期
- 📋 全路由回归：channels/database/*、channels/dataframe/*、operators/*、tasks/*
- 📋 docker-compose 部署：多容器启动、服务发现、故障重启
</details>

---

## Epic 8: Web UI 专业化改造
**优先级**: P1 | **状态**: ✅ 已完成（Sprint 7，2026-03-19）
**价值**: 提升产品专业感，全屏自适应布局，支持深色/浅色主题切换
**设计文档**: `tasks/sprints/sprint7/design_frontend_ui.md`

### Story 8.1: 全屏布局 + 主题切换
**状态**: ✅ 已完成 (Sprint 7)
**验收标准**:
- 页面铺满全屏，移除 Vite 默认模板的居中限制
- 固定深色侧边栏（VS Code / Grafana 风格），不随主题切换
- 顶部 Header 右侧提供深色/浅色主题切换按钮
- 主题状态通过 `localStorage` 持久化，刷新后保持
- 所有 Element Plus 组件随主题自动响应

<details>
<summary>任务分解（点击展开）</summary>

- 📋 重写 `style.css`：移除居中样式，定义 CSS 变量体系（`:root` 浅色 + `.dark` 深色）
- 📋 `main.js` 引入 `element-plus/theme-chalk/dark/css-vars.css`
- 📋 重写 `App.vue`：三层结构（侧边栏 + Header + 内容区），主题切换逻辑
- 📋 重写 `Sidebar.vue`：CSS 变量替换硬编码颜色，active 状态左侧高亮条
- 📋 各 View 文件：移除硬编码颜色和 `max-width` 限制
</details>

---

### Story 8.2: 侧边栏底部状态栏
**状态**: ✅ 已完成 (Sprint 7)（迁移至顶部导航栏右侧状态区）
**验收标准**:
- 侧边栏底部显示：当前用户（暂时固定 admin）、Gateway 连接状态（在线/离线）、版本号
- Gateway 状态每 30 秒轮询 `GET /api/health`，绿点/红点显示

<details>
<summary>任务分解（点击展开）</summary>

- 📋 `Sidebar.vue` 底部区域：用户名 + 状态指示点 + 版本号
- 📋 轮询 `/api/health`，30s 间隔，绿点/红点显示
- 📋 版本号从 `package.json` 的 `version` 字段读取
</details>

---

### Story 8.3: 数据库通道浏览器
**状态**: ✅ 已完成 (Sprint 7)
**验收标准**:
- 通道列表页数据库通道行新增"浏览"按钮
- 点击后从右侧滑出 Drawer，左侧显示表列表，右侧 Tab 切换表结构/数据预览
- 表结构显示列名、类型、是否可空、是否主键
- 数据预览固定返回前 100 条，以表格形式展示
- 支持 SQLite / MySQL / ClickHouse 三种数据库
**设计文档**: `tasks/design_db_browser.md`

<details>
<summary>任务分解（点击展开）</summary>

- 📋 `database_plugin.cpp` 新增 `HandleTables`：按数据库类型执行元数据 SQL，返回表名列表
- 📋 `database_plugin.cpp` 新增 `HandleDescribe`：执行 `PRAGMA table_info` / `DESCRIBE`，返回列定义
- 📋 `database_plugin.cpp` 新增 `HandlePreview`：执行 `SELECT * FROM <table> LIMIT 100`，返回数据
- 📋 `database_plugin.h` 新增 3 个 Handler 声明，`EnumRoutes` 注册 3 条路由
- 📋 `api/index.js` 新增 `listDbTables` / `describeDbTable` / `previewDbTable`
- 📋 `Channels.vue` 新增"浏览"按钮 + `el-drawer` 组件（左侧表列表 + 右侧 Tab）
</details>

---

## Epic 9: 内置通道与算子注册中心（CatalogPlugin）
**优先级**: P1 | **状态**: ✅ 已完成 (Sprint 8)
**价值**: 清理架构债务，建立 DataFrame 通道和内置算子的统一注册/发现机制，支持具名 DataFrame 通道跨 Pipeline 共享
**设计文档**: `tasks/sprints/sprint8/design.md`

### Story 9.1: 清理 plugins/example 和 plugins/testdata
**状态**: ✅ 已完成 (Sprint 8)
**验收标准**:
- 删除 `plugins/example/` 目录（MemoryChannel + PassthroughOperator）
- 删除 `plugins/testdata/` 目录
- `MemoryChannel` 移入 `src/framework/core/`，保留为公共类
- `PassthroughOperator` 移入 `src/framework/core/`
- 所有引用这两个插件的测试和代码更新为直接构造，编译通过
- `config/deploy-single.yaml` 和 `config/deploy-multi.yaml` 删除旧插件条目（待 Story 9.2 完成后替换为 `libflowsql_catalog.so`）

---

### Story 9.2: IChannelRegistry 接口 + CatalogPlugin 骨架
**状态**: ✅ 已完成 (Sprint 8)
**验收标准**:
- 新增 `src/framework/interfaces/ichannel_registry.h`（IChannelRegistry 接口，shared_ptr 语义，含 Register/Get/Unregister/Rename/List）
- 新增 `src/framework/interfaces/ioperator_registry.h`（IOperatorRegistry 接口）
- 新增 `src/services/catalog/` 目录，实现 CatalogPlugin（多继承 IPlugin + IChannelRegistry + IOperatorRegistry + IRouterHandle）
- `Option()` 支持 `data_dir` 配置项（默认 `./dataframes/`）
- `Register` 自动将通道数据序列化为 `data_dir/<name>.csv`（具名即持久化）
- `Unregister` 同步删除磁盘文件；`Rename` 同步重命名磁盘文件
- `Start()` 扫描 `data_dir` 目录，自动恢复所有具名通道（进程重启后无需重新导入）
- `Load()` 阶段注册内置算子类型（passthrough）
- 编译通过，单元测试验证：注册/查找/注销/重命名/重启恢复/并发安全

---

### Story 9.3: Scheduler 集成 — dataframe. 通道寻址
**状态**: ✅ 已完成 (Sprint 8)
**验收标准**:
- Scheduler `FindChannel()` 新增 `dataframe.` 分支，走 `IChannelRegistry::Get`
- SQL `INTO dataframe.<name>` 执行后自动调用 `IChannelRegistry::Register`
- SQL `FROM dataframe.<name>` 可读取已注册的具名通道
- 端到端测试：`INTO dataframe.result` → `FROM dataframe.result INTO sqlite.mydb.output` 链路通过

---

### Story 9.4: HTTP 端点 + Web UI 展示
**状态**: ✅ 已完成 (Sprint 8)
**验收标准**:
- CatalogPlugin 实现 `GET /channels/dataframe`（列出具名通道，含 name/rows/schema）
- CatalogPlugin 实现 `POST /channels/dataframe/import`（multipart 上传 CSV，自动推断类型，名称冲突时追加时间戳）
- CatalogPlugin 实现 `POST /channels/dataframe/preview`（预览指定通道前 100 行，格式对齐 DatabasePlugin）
- CatalogPlugin 实现 `POST /channels/dataframe/rename`（body: `{"name":"x","new_name":"y"}`，new_name 已存在返回 409）
- CatalogPlugin 实现 `POST /channels/dataframe/delete`（body: `{"name":"x"}`，注销通道）
- `web_server.cpp` 双通道注册（Init() httplib 代理 + EnumApiRoutes() IRouterHandle 代理）
- `Channels.vue` 新增 DataFrame 通道分组展示，显示通道名、行数、列定义（列名 + 类型）
- 通道列表页顶部新增"导入 CSV"按钮，上传成功后刷新列表并高亮新通道
- 每行操作列：预览（Drawer 展示前 100 行）| 重命名（inline 编辑，回车确认）| 删除（确认后注销）

---

## Epic 10: 算子目录与状态下沉 CatalogPlugin（架构收敛）
**优先级**: P1 | **状态**: ✅ 已完成（Sprint 9，2026-03-23）
**价值**: 消除 Web/Bridge/Scheduler 三处分裂状态，建立算子目录与激活状态唯一真相，提升一致性与可扩展性
**设计文档**: `tasks/sprints/sprint9/design.md`

### Story 10.1: CatalogPlugin 成为算子目录唯一来源
**状态**: ✅ 已完成（Sprint 9，2026-03-23）
**验收标准**:
- CatalogPlugin 提供统一算子目录接口（列表/详情/激活/去激活/更新）
- 算子状态（active）由 CatalogPlugin 持久化并对外查询
- Web 不再直接作为算子目录持久化主路径

---

### Story 10.2: BridgePlugin 同步 Python 算子到 Catalog
**状态**: ✅ 已完成（Sprint 9，2026-03-23）
**验收标准**:
- BridgePlugin 在 `Start/Refresh` 后批量 upsert Python 算子元信息到 Catalog
- upsert 不覆盖用户状态字段（如 active）
- 同步静态目录信息（name/type/source/description/position），不引入运行态字段

---

### Story 10.3: Web/Scheduler 读写路径切换到 Catalog
**状态**: ✅ 已完成（Sprint 9，2026-03-23）
**验收标准**:
- Web `/api/operators`、`/api/operators/detail`、激活/去激活接口内部改为转发 Catalog
- Scheduler 执行前以 Catalog 的 active 状态作为准入判断
- `/operators/query` 由 Catalog 提供统一查询入口，并移除 `/operators/native/query` 及其调用方引用

---

## Epic 11: Pipeline 增强与异步任务
**优先级**: P1 | **状态**: ✅ 已完成（11.1/11.2：Sprint 9；11.3/11.4：Sprint 10，2026-03-24）
**价值**: 增强 Pipeline 编排能力，支持异步任务执行，提升系统易用性
**设计文档**: `tasks/sprints/sprint9/design.md`、`tasks/sprints/sprint10/design.md`

### Story 11.1: 多算子 Pipeline（MVP）
**状态**: ✅ 已完成（MVP）
**验收标准**:
- 支持基础链式调用（`USING op1 THEN op2`，串行执行）
- 支持算子间数据传递
- 至少 2 条端到端自动化用例通过（成功链路 + 失败链路）

---

### Story 11.2: 异步任务执行（MVP）
**状态**: ✅ 已完成（MVP）
**验收标准**:
- 任务队列实现（基于线程池）
- 任务状态跟踪（`pending/running/completed/failed/cancelled/timeout`）
- 提供任务提交与轮询查询接口（不依赖 WebSocket）
- Web 任务列表支持状态展示与操作列“查看结果/删除”
- “查看结果”语义：
  - `failed`：展示错误信息（错误码、错误消息、失败阶段）
  - `completed`：仅展示结果摘要（生成记录条目数、输出通道名/目标）
- 历史任务支持删除（删除任务元数据、摘要与错误信息，不影响已写入的业务数据）
- 不持久化完整执行结果数据集（仅保存摘要与错误信息）

---

### Story 11.3: 多算子 Pipeline（增强）
**状态**: ✅ 已完成（Sprint 10，2026-03-24）
**验收标准**:

**1. 多 SQL 任务**
- 一个任务支持提交多条 SQL 语句，顺序执行，共享同一任务 ID 和状态
- 任务内产生的 `dataframe.<name>` 中间通道为任务私有，任务结束后自动清理
- 任意一条 SQL 失败则任务整体标记为 `failed`，后续语句不再执行

**2. IOperator 多输入接口扩展**
- `IOperator` 新增多输入重载，单输入版本保持纯虚（现有算子零改动）：
  ```cpp
  // 单输入（纯虚，现有算子必须实现）
  virtual int Work(IChannel* in, IChannel* out) = 0;
  // 多输入（默认实现：转发到 inputs[0]，多输入算子按需覆盖）
  virtual int Work(Span<IChannel*> inputs, IChannel* out);
  ```
- Scheduler 统一调多输入版本，单输入算子通过默认实现自动降级
- SQL 语法支持多源输入：`FROM ch1, ch2 USING <op> INTO out`

**3. 内置合并算子**
- `concat`：多个 DataFrame 按行合并，要求 schema 兼容（列名和类型一致）
- `hstack`：多个 DataFrame 按列合并，要求行数一致
- 不兼容时返回明确错误信息（schema 不匹配 / 行数不一致）

---

### Story 11.4: 异步任务执行（增强）
**状态**: ✅ 已完成（Sprint 10，2026-03-24）
**验收标准**:

**1. 任务取消与超时控制**
- 支持对 `pending` / `running` 状态的任务发起取消请求
- 支持任务级超时配置（提交时通过参数指定，单位秒），超时后任务标记为 `timeout`
- 取消和超时的粒度为"SQL 语句间"（每条 SQL 执行前检查），不中断单条 SQL 内部执行

**2. 前端任务状态感知**
- 前端轮询间隔从 3s 调整为 2s，任务进入终态（completed / failed / cancelled / timeout）后自动停止轮询
- 说明：WebPlugin 与 TaskPlugin 跨进程部署（web 进程 vs scheduler 进程），无共享内存，现有 HTTP 架构不支持跨进程长连接推送，故采用轮询方案

**3. 结构化诊断信息**
- 任务完成后（含失败）记录结构化诊断快照，存入 `task_diagnostics` 表：
  - 每条 SQL 的执行耗时（ms）
  - 每个阶段的读写行数（source 读取行数、sink 写入行数）
  - 经过的算子名称列表
- 通过 `POST /tasks/diagnostics` 查询，格式为 JSON

**4. 任务记录保留与清理策略**
- 支持两种可配置策略（均可独立启用，同时启用时取更严格的条件）：
  - **按时间**：超过 N 天的终态任务记录自动删除（默认 7 天）
  - **按数量**：终态任务记录超过 M 条时，按完成时间从旧到新删除（默认 1000 条）
- 策略参数通过 `gateway.yaml` 的 TaskPlugin option 配置
- 清理仅删除任务元数据、摘要与诊断信息，不影响已写入的业务数据通道

---

## Epic 12: C++ 算子插件
**优先级**: P1 | **状态**: ✅ 已完成（Sprint 11，2026-03-28）
**价值**: 让用户能够用 C++ 编写高性能算子插件，并在运行时动态加载/卸载，无需重启服务

### Story 12.1: C++ 算子插件编译工程 Sample
**状态**: ✅ 已完成（Sprint 11，2026-03-28）
**验收标准**:
- 提供独立的 C++ 算子插件 sample 工程（`samples/cpp_operator/`）
- Sample 实现一个完整的示例算子（如列统计算子）
- 包含 CMakeLists.txt，能独立编译出 .so 文件
- 文档说明如何实现 IOperator 接口并打包为插件
- 编译产物可被系统正确加载和执行

---

### Story 12.2: C++ 算子插件动态激活与去激活
**状态**: ✅ 已完成（Sprint 11，2026-03-28）
**验收标准**:
- 新增 `BinAddonHostPlugin`（`libflowsql_binaddon.so`），管理 C++ 算子 `.so` 的上传、激活、去激活、删除与详情查询
- 统一走 `/operators/*` 入口（由 `CatalogPlugin` 委派 cpp 分支），不单独引入 `/operators/cpp/*` URI
- 提供统一 API：
  - `POST /operators/upload`（`type=cpp`）
  - `POST /operators/activate` / `POST /operators/deactivate`（按 `plugin_id`）
  - `POST /operators/delete`（按 `plugin_id`）
  - `POST /operators/detail`（按 `plugin_id`）
  - `POST /operators/list`（`type=cpp`，插件维度返回）
- 激活后算子注册到 `IOperatorRegistry`，并同步写入 `operator_catalog`（`type=cpp`，`plugin_id` 关联），可被调度系统调用
- 去激活时安全卸载（执行中 `active_count > 0` 时拒绝去激活，避免 `dlclose` 风险）
- 激活失败需记录结构化错误状态（ABI 不匹配、符号缺失、名称冲突等），可通过列表/详情接口查看
- 与 `builtin/python` 管理体验保持一致（同一前端页面、同一 API 风格）

---

## Epic 14: 流式架构
**优先级**: P2 | **状态**: ✅ 已完成（路径 A 与控制面，Sprint 14，2026-04-06）
**价值**: 支持流式数据处理，满足网络性能分析等实时场景

**当前迭代边界（Sprint 14）**:
- In Scope：Story 14.11 ~ 14.13（统一加载、具名 Stream Sink 产品化、同源并发消费）
- Out of Scope：跨任务共享 source 动态订阅、广播回放持久化、多主机分布式编排
- 已拆分至 Epic 15：原 Story 14.5 / 14.6（路径 B 数据面能力）
- 后续候选：Story 14.16（历史数据批处理补算）

**已在 Sprint 13 落地**:
- Story 14.7（路径 B 接口占位）已完成
- Story 14.8（跨进程流通道 + TaskPlugin 统一入口）已完成
- Story 14.9（Web 流式管理完整化）已完成
- Story 14.10（Ring 并发模式补齐）已完成（含 `MPSC/MPMC`）

**Sprint 14 落地项**:
- Story 14.11（内置通道/算子统一加载）去除硬编码分支，统一内置注册与装配机制
- Story 14.12（Stream 通道具名创建与 Sink 产品化）支持显式创建、状态观测与角色约束
- Story 14.13（流式 Group DAG 编排）支持串行链式、同源并发与串并组合执行

### Story 14.0: StreamPlugin 流式通道生命周期管理
**状态**: ✅ 已完成（Sprint 12，2026-04-02）
**验收标准**:
- 新增 `StreamPlugin`（`libflowsql_stream.so`），解析 `stream_channels` 配置并注册流式通道
- 定义 `IStreamFactory`，支持按 `type.name` 查找流式通道
- Scheduler `FindChannel()` 可通过 `IStreamFactory` 解析流式 source

---

### Story 14.1: IStreamChannel（路径 A）接口与通道实现
**状态**: ✅ 已完成（Sprint 12，2026-04-02）
**验收标准**:
- 定义 `IStreamChannel`、`StreamBatch`、`PollEvent` 协议
- 实现 `RingStreamChannel`（`Open/Put/PollNext/Cancel/Close`）
- 实现 `FanInStreamChannel` / `FanOutStreamChannel`（`ROUND_ROBIN` + `ROUTE_BY_PARTITION_ID`）
- `AtomicRing` 本 Sprint 实现 `SPSC/SPMC`；`MPSC/MPMC` 返回 `ENOTSUP`

---

### Story 14.2: IStreamOperator（路径 A）接口与算子生命周期
**状态**: ✅ 已完成（Sprint 12，2026-04-02）
**验收标准**:
- 定义 `IStreamOperator` 生命周期：`Configure/Init/OnSchemaReady/Process/Tick/Flush`
- 支持流式算子插件导出符号与 ABI 校验（`flowsql_stream_operator_*`）
- 提供内置示例流式算子，覆盖基础链路
- 内置算子目录规范统一：DataFrame 内置算子归 `src/framework/builtin/dataframe/`，流式内置算子归 `src/framework/builtin/stream/`

---

### Story 14.3: StreamRuntime 通用执行容器 + Scheduler 流式调度
**状态**: ✅ 已完成（Sprint 12，2026-04-02）
**验收标准**:
- 实现 `StreamTask + ShardRunner + StreamRuntime`（线程池 + timer）
- 支持三种执行场景：`NONE`、`STATELESS`、`KEYED`
- 提供流式任务管理接口：`/tasks/stream/execute|stop|status|list`
- `TaskSnapshot`、`Stop/Join`、状态机迁移满足测试计划（T1~T15）

---

### Story 14.4: Scheduler 流式调度（旧拆分项）
**状态**: 🔁 已并入 Story 14.3（2026-04-01）
**验收标准**:
- 本 Story 不再单列交付，统一按 Story 14.3 验收

---

### Story 14.7: 路径 B 接口占位（`IBlockStream*`）
**状态**: ✅ 已完成（Sprint 13，2026-04-04）
**验收标准**:
- 新增 `IBlockStreamChannel` / `IBlockStreamOperator` 接口头文件（仅契约占位，不含数据面实现）
- 明确 `block_stream` 的调度入口契约和生命周期边界
- 提供最小编译与加载验证，确保占位接口不会影响路径 A 现有能力

---

### Story 14.8: 跨进程流通道与 TaskPlugin 统一入口
**状态**: ✅ 已完成（Sprint 13，2026-04-04）
**验收标准**:
- 补齐跨进程流式任务提交与管理链路（Web → Gateway → Scheduler/TaskPlugin）
- 统一 Stream 任务的提交、停止、状态、列表入口，减少与批处理入口割裂
- 补齐跨进程错误透传与诊断字段，保证失败原因在 Web 端可定位
- 新增端到端回归测试，覆盖跨进程流式任务的 execute/stop/status/list 主链路

---

### Story 14.9: Web 流式管理页面完整化
**状态**: ✅ 已完成（Sprint 13，2026-04-04）
**验收标准**:
- 在现有只读基础上补齐 Stream 通道管理能力（增删改与配置编辑）
- 新增流式任务管理页面，支持 `execute/stop/status/list` 的可视化操作
- 展示流式任务核心指标与失败信息（如 `status/error/op_stats`），支持快速定位
- 补齐前后端联调与 E2E 冒烟用例，覆盖主流程

---

### Story 14.10: Ring 并发模式补齐（`MPSC/MPMC`）
**状态**: ✅ 已完成（Sprint 13，2026-04-04）
**验收标准**:
- 将 `ring_mode=mpsc/mpmc` 从 `ENOTSUP` 升级为可用实现
- 覆盖并发正确性测试（无丢失、无重复、可收敛）
- 覆盖 Stop/Cancel 场景下的稳定性测试
- 补充性能基线数据，与现有 `SPSC/SPMC` 路径对比

---

### Story 14.11: 内置通道/算子统一加载（去硬编码）
**状态**: ✅ 已完成（Sprint 14，2026-04-06）
**验收标准**:
- 统一内置注册中心：通道类型构建器与算子工厂通过统一注册机制装配，移除核心硬编码分支
- `StreamPlugin` 不再按 `if(type==...)` 选择构建逻辑，改为注册表查找并构建
- `CatalogPlugin` 内置算子不再手工逐条注册，改为基于注册表批量注册（含 `builtin.*` 别名）
- 新增内置项时无需改动核心调度与装配逻辑，仅新增注册声明与实现
- 补齐注册冲突、缺失注册、非法配置的错误处理与测试覆盖

---

### Story 14.12: Stream 通道具名创建与 Sink 通道产品化
**状态**: ✅ 已完成（Sprint 14，2026-04-06）
**验收标准**:
- 支持显式创建并管理具名 Stream 通道，作为 `INTO stream.<name>` 的稳定 sink 目标
- Stream 通道查询结果补齐运行态关键字段（容量、模式、状态、占用信息等），便于运维与排障
- 明确通道角色语义（source/sink/both）与校验策略，避免隐式误用
- 明确生命周期规则（创建、修改、删除、停止）与并发保护策略（含 in-use 约束）
- 完成前后端联动验证：创建后可作为 sink 写入并可在管理面观测状态变化

---

### Story 14.13: 流式 Group DAG 编排（同源并发 + 链式串行）
**状态**: ✅ 已完成（Sprint 14，2026-04-06）
**验收标准**:
- 复用现有流式执行 URI，通过 `execution_kind/group_mode` 显式区分 single 与 group
- `group_mode=dag` 同时支持串行链式、同源广播并发、串并组合三类拓扑
- 同源广播分支满足“source 读取一次、分支数据集合一致”（允许全分支一致丢弃，禁止分支间不一致可见）
- 明确慢分支/失败分支处理策略（背压、fail-fast、错误透传）并形成可观测指标
- `timeout_s` 与 `share_set_ready_timeout_s` 语义明确，超时路径可收敛并可观测
- `execute` 与 `modify/remove` 并发下无 TOCTOU 误判（版本校验 + 引用登记原子）
- 补齐 DAG 并发正确性与稳定性测试（环路校验、串并组合、Stop/Cancel、慢分支、异常分支）

---

### Story 14.14: Hybrid DAG（batch + stream 混合编排）
**状态**: ✅ 已完成（Sprint 17）
**验收标准**:
- 支持单任务内混合编排：`batch -> stream`、`stream -> batch`、以及多段组合拓扑
- 统一提交契约与状态观测：任务依赖、超时、错误码、节点状态可在单任务维度查询
- 首阶段约束：`source_share_sets` 仅允许 stream 节点；`stream -> batch` 仅允许 `on_finished`
- 首阶段约束：`batch -> stream` 仅支持一次性装填并在写入完成后 `CloseStream()`
- 明确 mixed DAG 的可执行边界：不满足约束的拓扑需在提交阶段被拒绝并返回结构化错误

---

### Story 14.15: 跨任务共享 source（late join）
**状态**: ✅ 已完成（Sprint 16）
**验收标准**:
- 支持多个 stream 任务并行消费同一 source，不再要求 source 独占租约
- 支持 `late join`：新任务在既有任务运行中加入后，从加入时刻开始消费（不补历史）
- 消费隔离：单任务 `stop/cancel/fail` 不影响其他共享消费者正常运行
- 背压策略可配置且可观测：慢消费者不阻塞全局，关键指标（丢弃/滞后/吞吐）可查询
- 管理面与状态接口返回共享消费拓扑信息（共享组、消费者数、每消费者状态）
- 实施结果：
  - 已完成：SharedSourceHub 核心运行时、跨任务同源并发、late join、stop 隔离、Group share set 迁移、BroadcastHub 下线
  - 已完成：`subscriber_stats.lag` 指标、TaskPlugin `/tasks/stream/status|list` 透传、前端任务详情共享订阅信息展示、README 能力矩阵同步
  - 已完成：新增慢消费者背压可观测回归用例（跨任务共享 source 场景）

---

### Story 14.16: 历史数据批处理补算（替代流内回放）
**状态**: 📋 待规划（后续 Sprint）
**验收标准**:
- 明确不在流式数据面实现“广播回放/持久化重放”；历史补算统一走 batch 任务链路
- 支持基于时间窗口/条件的历史数据重算与回写（面向已落地存储）
- 提供实时流与历史补算的边界约束（时间水位或 offset 边界），避免重复统计
- 定义补算结果幂等策略（去重键/覆盖策略），保障反复补算结果一致
- 提供标准化执行模板与运维指引（提交参数、失败重试、结果校验）

---

## Epic 15: 高性能实时采集与数据面加速
**优先级**: P1 | **状态**: 📋 待规划
**价值**: 构建路径 B 的高吞吐、低时延 packet 数据面，为 DPDK、AF_XDP 等实时采集通道提供统一加速能力，并复用上层 NPM 核心

> 本 Epic 聚焦高性能采集和零拷贝数据面，不承载 NPM 语义分析。NPM 基础分析拆分到 Epic 20，避免在采集通道中重复实现协议解析、会话聚合和检测逻辑。

### Story 15.1: DPDK 网卡采集通道（承接原 Story 14.5）
**状态**: 📋 待规划
**验收标准**:
- 实现 `netcard` / `dpdk` 采集通道，支持网卡数据包实时采集
- 支持多队列并行、RSS 分发和基础运行状态观测
- 输出统一 packet 逻辑契约，不向上层暴露 DPDK `mbuf` 细节
- 支持与路径 B 块式数据面衔接，避免热路径不必要拷贝

---

### Story 15.2: AF_XDP / 其他高性能采集通道适配
**状态**: 📋 待规划
**验收标准**:
- 预留 AF_XDP、PF_RING、netmap 等高性能采集通道的适配边界
- 明确不同采集后端到统一 packet 逻辑契约的映射规则
- 支持不同后端按能力声明零拷贝、批大小、队列模型和时间戳来源
- NPM 核心不依赖具体采集后端，新增采集通道不要求重写 NPM

---

### Story 15.3: 高性能采集通道复用 Packet 数据面契约
**状态**: 📋 待规划
**验收标准**:
- 复用 Epic 19 定义的 `IBlockPayload` / `PacketBatchView` / `packet.v1` 契约，不在高性能采集 Epic 中重复定义 packet 数据面基础模型
- 明确 DPDK `mbuf`、AF_XDP frame 等实时采集 buffer 到 `PacketBatchView` 的零拷贝映射规则
- 明确 `PollBlock()` / `ReleaseBlock()` 生命周期：下游处理完成并释放 block 前，底层 packet buffer 不得回收到采集内存池
- 支持不同实时采集后端声明批大小、队列模型、时间戳来源、零拷贝能力和 buffer 生命周期约束
- 为后续 DPDK / AF_XDP 采集通道提供性能测试和资源回收验证入口

---

## Epic 16: 平台增强与用户认证
**优先级**: P2 | **状态**: 📋 待规划
**价值**: 提升系统可观测性、可维护性、易用性和安全性

### Story 16.1: 用户认证与权限
**状态**: 📋 待规划
**验收标准**:
- 用户注册和登录（JWT Token）
- 基于角色的权限控制（RBAC）
- 通道和算子访问权限
- 操作审计日志
- Session 管理

---

### Story 16.2: 监控和告警
**状态**: 📋 待规划
**验收标准**:
- Prometheus 指标导出
- Grafana 仪表盘
- 告警规则配置
- 告警通知（邮件/钉钉/企业微信）

---

### Story 16.3: 日志聚合
**状态**: 📋 待规划
**验收标准**:
- 结构化日志输出（JSON 格式）
- 日志级别控制
- 日志轮转和归档
- ELK 集成

---

### Story 16.4: 配置中心
**状态**: 📋 待规划
**验收标准**:
- 配置热更新
- 配置版本管理
- 配置回滚
- 配置审计

---

### Story 16.5: 插件市场
**状态**: 📋 待规划
**验收标准**:
- 插件上传和下载
- 插件版本管理
- 插件依赖管理
- 插件评分和评论

---

### Story 16.6: 文档和示例
**状态**: 📋 待规划
**验收标准**:
- 用户手册
- 开发者指南
- API 文档
- 示例项目

---

## Epic 17: SQL 任务编排可视化与可视化编辑
**优先级**: P1 | **状态**: 🚧 进行中（阶段一已完成：Sprint 18；阶段二待规划）
**价值**: 以任务执行实例为中心，提供单画布 DAG 可视化与后续可视化编排编辑能力，降低多 SQL 编排理解与运维成本。

### Story 17.1: 运行时任务编排可视化（阶段一）
**状态**: ✅ 已完成（Sprint 18，2026-04-11）
**验收标准**:
- 任务可视化页以“执行实例（Runtime Instance）”为对象，不提供未提交 SQL 的预览态。
- SQL 工作台中所有任务均可进入可视化页，不再按 task kind 限制入口。
- 在单画布中展示完整任务 DAG：节点包含 `Channel`/`Operator`，同名通道节点去重复用。
- 边展示触发语义：`on_start` / `on_data` / `on_finish`，并可直观看到依赖关系。
- 节点与边展示运行快照：`status/phase/processed_rows/output_rows/error` 等关键指标。
- 支持多 SQL 串接场景正确连线（例如前一条 SQL 的 sink 作为后一条 SQL 的 source）。
- 可视化数据由后端统一输出图结构契约，前端不再机械拆 SQL 猜图。
- Runtime Graph 查询采用新增 URI 三层映射并冻结：`/api/tasks/runtime/graph/query`、`/tasks/runtime/graph/query`、`/scheduler/runtime/graph/query`（不引入兼容别名）。

---

### Story 17.2: 可视化任务编辑与提交（阶段二）
**状态**: 📋 待规划
**验收标准**:
- 提供可视化任务编辑页面，支持以卡片/连线方式构建通道与算子编排。
- 支持任务草稿保存、加载与版本化管理。
- 提交前由后端进行拓扑与约束校验（依赖、触发语义、通道/算子能力匹配）。
- 编辑态与运行态解耦：编辑页面不直接承载运行快照，运行快照仍由 Runtime 可视化页展示。
- 保持与 SQL 入口一致的执行语义与错误码契约。

---

## Epic 18: 通用基线检测插件
**优先级**: P1 | **状态**: ✅ 已完成（Sprint 21 BaselineB，B1-B8 已实现并验证）
**价值**: 将基线能力从上游业务中解耦，建设可复用的 `baseline` 通用插件，统一支撑数值指标、比例指标和关系分布的异常检测。当前主路径已从固定历史模型与异步重建迁移为 `Optional Bootstrap + Online Rolling Core + Maturity / Score Trust + Relation Fusion`，支持无历史流式启动、历史 seed 预热、基线 band 输出、关系模式融合、高基数状态治理和批量预测优化。

**历史说明**:
- Sprint 19 已完成统一算法设计，但实现迭代失败；评审结论以 `tasks/sprints/sprint19-baseline/review.md` 和 `tasks/sprints/sprint19-baseline/retrospective.md` 为准。
- 原 `18.1 ~ 18.9` 属于 Sprint 19 的第一版 Story 分解，现已归档，仅保留历史参考，不再作为当前实施入口。
- Sprint 20 BaselineA 已完成固定历史模型、正式重建与一致性整改收口；其状态以 `tasks/sprints/sprint20-baselineA/review.md` 为准。
- Sprint 21 BaselineB 将生命周期迁移为 stream-first、自成熟在线基线；`shadow baseline`、`candidate model`、正式重建和切换验证链路已退出在线主路径。
- 当前代码状态以 `src/plugins/baseline`、`src/framework/interfaces/ibaseline_service.h`、`src/framework/interfaces/ibaseline_types.h` 和 `src/tests/test_baseline` 为准。

**最近验证**（2026-06-06）:
- `cmake --build /mnt/d/working/flowSQL/build --target test_baseline test_baseline_rolling_feature_batch test_baseline_batch_prediction_perf`
- `/mnt/d/working/flowSQL/build/output/test_baseline`
- `/mnt/d/working/flowSQL/build/output/test_baseline_rolling_feature_batch`
- `/mnt/d/working/flowSQL/build/output/test_baseline_batch_prediction_perf`

> `18.10 ~ 18.20` 为 Sprint 20 BaselineA 的历史收口项；其中涉及 `shadow/candidate/rebuild` 的旧生命周期语义，已在 Sprint 21 BaselineB 中迁移为可选 bootstrap 与在线 rolling 主路径。

### Story 18.10: 统一协议与任务规格闭环
**状态**: ✅ 已完成（Sprint 20 BaselineA）
**验收标准**:
- 补齐 `DetectorResult / FusionResult / evidence` 正式输出协议。
- 补齐 `BaselineTaskSpec / RelationTaskSpec / RelationTaskClockSpec` 及 `HistoryReader / BaselineSourceResolver / QueryKeyFusionSnapshotJson` 接口。
- 所有 task 配置在创建阶段完成正式字段校验，`?` 字段的 absent 语义明确且一致。

---

### Story 18.11: 共享时间、事件与 readiness 基础层
**状态**: ✅ 已完成（Sprint 20 BaselineA）
**验收标准**:
- 建立 `profile_config / calendar_feature_helper / event_calendar_matcher / readiness_helper` 共享基础层。
- 统一 `DST`、`day/week` 相位、`monthpos`、事件命中与 `readiness` 计算口径。
- 共享主参数、`T1b` 参数、`T2` 参数及其派生值统一收口，不再散落在 detector / trainer 中。

---

### Story 18.12: 正式模型 schema 与 predictor 落地
**状态**: ✅ 已完成（Sprint 20 BaselineA）
**验收标准**:
- 正式模型可表达 `T1 / T2` 所需的 `Core / monthpos / event` 块结构与训练摘要元数据。
- predictor 可基于 `bucket_id + delta + tz + compiled events` 生成正式预测值。
- 模型对象持久化事件版本、readiness 元数据和训练 digest，支撑后续重建与验证。

---

### Story 18.13: 插件与 task 编排层重构
**状态**: ✅ 已完成（Sprint 20 BaselineA）
**验收标准**:
- `ValueTask / RatioTask / RelationTask` 三类 task 均支持 task-bound 注入、状态持有、submit 编排和 snapshot。
- `relation_task` 具备独立 runtime 结构，不再隐含在公共壳层中。
- task 壳层只负责编排与生命周期，不再承载求解器和算法目标函数。

---

### Story 18.14: `T1` 训练与正式重建慢路径
**状态**: ✅ 已完成（Sprint 20 BaselineA；Sprint 21 已迁移为 Optional Bootstrap 历史拟合能力）
**验收标准**:
- 实现 `WeightedHuberRidgeBlockSolver`、`TrainCore / MonthPos / Event`、`sigma` 估计与 `candidate vs incumbent` 验证。
- `formal_model_state` 收口 `candidate_state / switch_state`，`rebuild_worker` 完成正式重建闭环。
- `T1` 历史回放、训练、验证、切换状态全部可测试。

---

### Story 18.15: `T1` 在线评分、漂移证据与 `shadow baseline`
**状态**: ✅ 已完成（Sprint 20 BaselineA；Sprint 21 已迁移为在线 rolling、drift adapt 与 score trust）
**验收标准**:
- `T1a / T1b` 热路径正式接入 predictor、`ReadinessState`、`DriftState` 与 `ShadowState`。
- `ValueEvidence`、`RebuildIntent`、`sample_count -> gate / rho / sigma_eff` 等正式证据链路全部生效。
- `shadow baseline` 能激活、更新、退出，并与正式重建触发闭环。

---

### Story 18.16: `T2` 训练与在线评分闭环
**状态**: ✅ 已完成（Sprint 20 BaselineA；Sprint 21 已迁移为 ratio rolling 与 band 输出）
**验收标准**:
- `T2` 完整实现 `m0 / alpha0 / beta0 / p_smooth / logit / sigmoid / variance layer` 数学路径。
- `rate_core / ratio_bursty` 的 profile 差异、来源借用和正式重建全部落地。
- `RatioEvidence`、`baseline_source_key` 与比例类正式评分链路闭环成立。

---

### Story 18.17: `T3` basis、摘要特征与 routed detector
**状态**: ✅ 已完成（Sprint 20 BaselineA；Sprint 21 已迁移为 Relation routed rolling 与 stream basis）
**验收标准**:
- 补齐 `ServiceBasis / EvalBasis / lineage` 结构与兼容判断。
- 实现 `entropy_shannon / top1_share / headK_share / out_of_support_share / distinct_group_count / stable_g[i]_share / stable_headK_coverage / stable_headK_mix_drift` 摘要特征。
- routed detector 真实复用 `ValueDetectorCore / RatioDetectorCore`，并支持 relation snapshot 与 basis 可比较输出。

---

### Story 18.18: 模式融合层与 key 级风险合成
**状态**: ✅ 已完成（Sprint 20 BaselineA；Sprint 21 已迁移为 Relation pattern fusion）
**验收标准**:
- 实现 `relation_pattern_fusion` 与 `key_risk_fusion`，正式落地 `core_P / support_P / oppose_P` 和 `lambda_sup / lambda_opp / lambda_P(pattern)`。
- 输出 `Risk_T1T2 / Risk_single_T3 / Risk_pattern / Risk_T3 / Risk(Key,t)` 与 `FusionResult`。
- `QueryKeyFusionSnapshotJson`、`FusionSourceId` 与 `RemoveTaskContributions(task_id)` 全部闭环。

---

### Story 18.19: `T3` 正式重建、验证与 lineage 切换
**状态**: ✅ 已完成（Sprint 20 BaselineA；Sprint 21 已迁移为 stream basis refresh / handover）
**验收标准**:
- `T3` 正式重建同时产出 `CandidateServiceBasis / CandidateEvalBasis`。
- `candidate vs incumbent` 比较严格在共同 `EvalBasis` 上完成，并处理兼容与 `new lineage` 两条路径。
- `shadow baseline` 在 routed 序列与正式切换之间形成桥接闭环。

---

### Story 18.20: 测试收口、死代码清理与最终一致性审查
**状态**: ✅ 已完成（Sprint 20 BaselineA）
**验收标准**:
- 建立覆盖 `T1 / T2 / T3 / fusion / rebuild / relation snapshot / concurrency` 的完整测试矩阵。
- 删除旧的 `intercept-only`、常数预测和其他占位路径，清理无效死代码。
- 以 `design.md + code-design.md` 为基准完成最终一致性审查，确认不再出现“只有壳、没有算法”的偏差。

---

### Story 18.21: BaselineB 生命周期迁移与 Optional Bootstrap Engine
**状态**: ✅ 已完成（Sprint 21 BaselineB B1）
**验收标准**:
- 旧 `shadow/candidate/rebuild` 在线恢复链路退出主路径，历史拟合能力保留为可选 bootstrap。
- `Value / Ratio / Relation` 支持 bootstrap 训练、预测、artifact / seed 导出导入和兼容性校验。
- bootstrap 输出 `baseline_mu / baseline_lower / baseline_upper / band_width / confidence / uncertainty_source`，并可作为后续 rolling 初始化种子。

---

### Story 18.22: Online Rolling Core MVP
**状态**: ✅ 已完成（Sprint 21 BaselineB B2）
**验收标准**:
- `Value / Ratio` 在无历史时可直接流式启动并持续学习。
- 在线路径按 `predict -> band -> score -> gate_update -> update_state` 推进。
- 输出包含基线 band、置信度、更新门控和状态诊断；历史 seed 可预热但不是可用性前置条件。

---

### Story 18.23: Detection Trust、Band Calibration 与 Monthly Readiness
**状态**: ✅ 已完成（Sprint 21 BaselineB B3）
**验收标准**:
- `maturity_status` 与 `score_trust_status` 分离，冷启动和重校准阶段不输出高置信异常结论。
- 检测 band 支持残差尺度、成熟度不确定性、水平变化降级和月位置成熟。
- task / series snapshot 可观测 maturity、score trust、enabled components、component readiness 与 coverage。

---

### Story 18.24: Relation Routed Rolling and Stream Basis
**状态**: ✅ 已完成（Sprint 21 BaselineB B4）
**验收标准**:
- Relation 流式 block 可投影为 routed summary observations，并复用 `T1/T2` Online Rolling Core。
- 无历史时支持通用摘要流式学习；有 bootstrap seed 时支持 basis 与 routed summary warm-up。
- basis 统计有固定上限，basis refresh / handover 有版本和 evidence，旧 rebuild 链路不参与 Relation basis 成熟。

---

### Story 18.25: Relation Pattern Fusion and Risk Output
**状态**: ✅ 已完成（Sprint 21 BaselineB B5）
**验收标准**:
- `RelationFusionResult` 输出 `relation_risk`、主导单特征证据、主导结构模式和 pattern scores。
- `support_escape / head_concentration / legacy_head_dilution / stable_head_mix_shift` 均已实现并有测试覆盖。
- fusion 受 B3 score trust、basis gate、metric 缺测和跨 metric 饱和合成约束，不反向修改 routed rolling state。

---

### Story 18.26: Baseline 同 task 串行调用与锁优化
**状态**: ✅ 已完成（Sprint 21 BaselineB B6）
**验收标准**:
- `IBaselineTask` 明确同一 task 实例不支持重叠并发调用，跨线程串行 handoff 由上游建立 happens-before。
- `BaselineTaskBase` 不再暴露 task 级 mutex；不同 task 仍可并行调用，registry / plugin 外边界保留必要同步。
- 测试覆盖同 task 跨线程串行 handoff、Relation lifecycle 序列和不同 task 并行。

---

### Story 18.27: Relation Fusion Runtime State Cleanup
**状态**: ✅ 已完成（Sprint 21 BaselineB B7）
**验收标准**:
- Relation fusion source state 支持 TTL、容量上限、扫描游标和清理统计。
- evidence persistence key 有上限治理，清理指标可在 task snapshot 中观测。
- 配置模板、运行时配置解析和 TTL / capacity 回归测试均已覆盖。

---

### Story 18.28: 批量预测特征缓存与谐波递推优化
**状态**: ✅ 已完成（Sprint 21 BaselineB B8）
**验收标准**:
- `RollingFeatureBatch` 支持批量构造日 / 周谐波特征与本地日历特征 view。
- Fourier 递推在 `DST`、非连续 bucket 和重锚间隔边界自动 re-anchor，保持与单点特征等价。
- `Rolling / Bootstrap` 批量预测等价性通过测试，性能测试输出批量路径相对单点路径的加速结果。

---

## Epic 19: Packet 数据面契约与可复现 pcap 数据源
**优先级**: P1 | **状态**: 📋 待规划
**价值**: 建立 FlowSQL 内部统一 packet 数据形态和可复现 pcap 数据源，让 pcapfile、DPDK、AF_XDP 等通道都能输出一致的 `PacketBatchView`，并将 packet 上下文稳定交给后续 NPM、packet filter 和检测算子复用

### Story 19.1: `IBlockPayload` / `PacketBatchView` 与 `packet.v1` 契约
**状态**: 📋 待规划
**验收标准**:
- 将当前 `IBlockStreamChannel` 从 Arrow 专用块扩展为通用块式数据面契约，`BlockPollEvent` 承载 `IBlockPayload` 或等价抽象
- 保留 Arrow `RecordBatch` 作为可选 payload 类型或边界导出格式，不作为 packet 热路径的唯一表示
- 定义统一 `packet.v1` schema，至少包含 `ts_ns`、`source_id`、`link_type`、`cap_len`、`wire_len`、`rx_queue`、`packet_id`、`data`
- `packet.v1` 与采集来源解耦，pcapfile、DPDK、AF_XDP 等通道都映射到同一逻辑契约
- 定义 `PacketBatchView` 运行时视图，承载 packet 指针 / 长度 / 时间戳 / source / queue / packet_id、buffer 引用和可扩展 packet 上下文等数组化访问能力
- 明确 `packet.v1` 是逻辑语义契约，`PacketBatchView` 是 packet 热路径主执行形态；Arrow 仅用于边界导出、调试或与现有 DataFrame / Stream 生态互操作
- 明确 `PollBlock()` / `ReleaseBlock()` 生命周期：下游处理完成并释放 block 前，底层 packet buffer 不得回收
- 明确 schema 版本、字段兼容策略、上下文扩展策略和最小支持链路类型（MVP 先支持 Ethernet）
- 提供 schema / view / payload 文档和构造 / 校验 / 生命周期测试

---

### Story 19.2: `PacketLayerHints` 与 packet 上下文
**状态**: 📋 待规划
**价值**: 将分层识别结果写入 packet 上下文，使下游 NPM 可以复用统一层信息，同时为 Epic 22 的 `packet_filter.v1` 提供结构层过滤输入，避免 NPM 和 packet filter 在热路径重复解析 packet
**验收标准**:
- 将 `src/plugins/npi` 现有 `NetworkLayer::Layer()`、parser/dispatch 和 `protocol::Layers` 视为完整可用的基础分层能力；本 Story 直接复用，不重新盘点、认证或建设逐协议支持矩阵，也不引入第二套 layer parser
- 定义 `PacketLayerHints` 与 `protocol::Layers` 的映射关系，至少包含 layer count、layer type 数组、layer offset 数组、payload offset、top layer，以及派生的 L2 / L3 / L4 offset、IP version、L4 protocol、VLAN depth 和 tunnel depth
- `PacketLayerHints` 必须写入 `PacketBatchView` 或等价 packet 上下文，作为下游 NPM Core 的可复用输入；NPM 可以基于 hints 快速定位 L3 / L4 / payload，但仍保留必要校验能力
- `PacketLayerHints` 必须作为 `packet_filter.v1` 结构层过滤的输入，支撑 IP、端口、L4 protocol、VLAN、tunnel depth、parse status 等过滤条件，不要求执行应用层协议识别
- 提供轻量 adapter / classifier，使 `pcapfile`、DPDK、AF_XDP 等 packet 通道可以填充统一 layer hints；该 adapter 只消费 packet 指针、`cap_len` 和 `link_type`，不依赖 pcapfile 文件状态、buffer 生命周期或 wall-clock 时间
- 本 Story 只复用 / 提炼 NPI 的分层识别能力，不调用 `IProtocol::Identify()`，不加载应用层协议规则，不输出 HTTP / TLS / DNS 等应用层协议结果；应用层协议识别仍归属 Epic 20 或后续 NPI 算子
- 在复用现有 NPI parser/dispatch 的前提下增加轻量 checked façade 和解析终态传播：接口负责参数、固定最小头长度、consumed offset、`cap_len`、layer count、payload offset 和 LINKTYPE 边界；必要的 NPI 改动仅限于让现有解析路径暴露 `parse_status` / `truncated` / `malformed` 等结果，不进行完整 parser 重构或逐协议安全加固
- 适配接口必须可被后续 DPDK / AF_XDP 通道复用，保证实时采集与 `pcapfile` 对同一 packet 产生一致的 layer hints
- 提供最低契约所需的固定 packet 样本或构造型单元测试，覆盖 Ethernet + IPv4 + TCP、VLAN + IPv4 + UDP、IPv6 + TCP、截断包和至少一种隧道封装样本；这些样本用于验证 façade、终态传播和上下文映射，不用于重新认证 NPI 的完整协议能力

---

### Story 19.3: `pcapfile` 单文件块式流通道 MVP
**状态**: 📋 待规划
**验收标准**:
- 实现 `pcapfile` 通道，支持从单个 pcap / pcapng 文件读取 packet
- 通道实现 `IBlockStreamChannel`，通过 `PollBlock()` 输出 `PacketBatchView` block
- `PacketBatchView` 中必须标识 packet 来源文件、文件内 packet 序号和全局递增 `packet_id`，便于排障、去重和回归校验
- 每个 packet 必须保留原始事件时间，后续 NPM / Baseline 不依赖读取时的 wall-clock 时间
- 每个 block 持有 pcap buffer 生命周期，调用方完成处理并 `ReleaseBlock()` 后才能释放底层内存
- 支持文件 EOF、读取错误、文件元数据和基础运行统计
- 在 Story 19.2 已完成时，`pcapfile` 应填充 `PacketLayerHints`；未完成时必须保留可插拔的 layer hints 扩展点
- `pcapfile` 不执行 NPM 语义分析，不解析 TCP 会话、不聚合 flow

---

### Story 19.4: `pcapfile` 多文件集合顺序读取
**状态**: 📋 待规划
**验收标准**:
- 支持从多个 pcap / pcapng 文件组成的文件集合读取 packet
- 文件集合支持显式列表和目录扫描两类输入；目录扫描必须有稳定排序规则，避免同一批文件多次执行顺序不一致
- 默认按文件顺序串行读取 packet，并保留每个 packet 的原始事件时间；适用于同一采集源按大小或时间轮转切分出的 pcap 文件
- `PacketBatchView` 中必须标识 packet 来源文件、文件内 packet 序号和全局递增 `packet_id`
- 通道实现 `IBlockStreamChannel`，通过 `PollBlock()` 输出 `PacketBatchView` block
- 支持单文件 EOF、全局 EOF、读取错误、文件元数据和基础运行统计；单文件失败是否中断整体读取需由配置声明
- `pcapfile` 不执行 NPM 语义分析，不解析 TCP 会话、不聚合 flow

---

### Story 19.5: `pcapfile` 多文件时间戳归并读取
**状态**: 📋 待规划
**验收标准**:
- 当多个文件来自不同接口、不同采集点或存在时间范围重叠时，支持按 packet 时间戳做多文件全局归并读取
- 归并读取必须保持来源文件、文件内 packet 序号和全局递增 `packet_id` 可追踪
- 支持配置归并策略，默认不启用全局归并，避免轮转切分文件承担不必要的复杂度
- 归并读取应避免后续 NPM / Baseline 看到明显乱序的事件流
- 覆盖多文件时间重叠、空文件、单文件提前 EOF 和时间戳相同 packet 的排序回归

---

### Story 19.6: 固定样本与 `pcap_replay` 验证回归
**状态**: 📋 待规划
**验收标准**:
- 本 Story 不重复实现 pcap / pcapng 解析，复用 Story 19.3 / 19.4 / 19.5 的 `pcapfile` 读取能力和 `PacketBatchView`
- 提供固定小样本 pcap 和基准期望值，用于端到端测试、演示和后续回归
- 默认验证路径使用 packet 原始时间戳作为事件时间，不要求按 wall-clock sleep 回放
- 测试覆盖包数、字节数、事件时间戳、layer hints、EOF、错误路径和 `ReleaseBlock()` 生命周期
- 可选支持最快速度、固定倍率、按原始间隔 sleep 的 live-like 回放模式，用于压测和实时链路验证
- 回放控制能力可作为后续 NPM / Baseline / 模型算子的稳定输入源，但不阻塞 Story 19.3 / 19.4 的最小读取闭环

---

## Epic 20: NPM 基础分析算子与流量事实表
**优先级**: P1 | **状态**: 📋 待规划
**价值**: 将统一 packet 数据转化为可查询、可聚合、可建模的 flow / session 事实表，完成 `流量输入 → NPM 分析 → SQL 查询` 的最小闭环

### Story 20.1: NPM Core 与 packet 解析
**状态**: 📋 待规划
**验收标准**:
- 实现独立 `NpmCore`，直接消费 `PacketBatchView`，不依赖 `pcapfile`、DPDK 或 AF_XDP 具体实现
- 支持 Ethernet、IPv4 / IPv6、TCP、UDP、ICMP 的基础解析
- 对截断包、异常包、未知链路类型提供可观测错误计数
- 提供 parser 单元测试和固定 packet 样本回归

---

### Story 20.2: `npm.basic` 块式流算子
**状态**: 📋 待规划
**验收标准**:
- 实现 `npm.basic` 块式流算子，消费 `IBlockStreamChannel` 输出的 `PacketBatchView`
- 按五元组聚合基础 flow，输出 `flow.v1`
- 基础字段包含 `src_ip`、`dst_ip`、`src_port`、`dst_port`、`protocol`、`first_seen`、`last_seen`、`packets`、`bytes`、`duration`
- 算子不打开 pcap 文件，不依赖采集通道实现细节
- 算子在处理完成后释放输入 block，验证 `ReleaseBlock()` 生命周期闭环

---

### Story 20.3: TCP / UDP Session 基础事实表
**状态**: 📋 待规划
**验收标准**:
- 基于 `NpmCore` 输出 `tcp_session.v1` 和 `udp_session.v1` 基础事实
- TCP MVP 支持连接方向、起止时间、包数、字节数和基础状态计数
- UDP MVP 支持基于五元组和超时时间的会话归并
- 明确 session 超时、方向判定和重复 / 乱序包处理策略

---

### Story 20.4: NPM SQL 闭环与端到端验证
**状态**: 📋 待规划
**验收标准**:
- 支持类似 `npm.basic packet from pcapfile.sample into ts.npm` 的端到端执行路径
- SQL 执行路径优先走 `IBlockStreamChannel` + `PacketBatchView`，而不是先走 Arrow `RecordBatch` 再二次迁移
- NPM 结果可通过 SQL 查询 `flow` / `tcp_session` / `udp_session`
- 固定 pcap 样本验证 flow 数、包数、字节数和协议分布
- 文档同步说明 packet 通道、NPM 算子和结果事实表的边界

---

## Epic 21: 流量检测算子化与分析闭环
**优先级**: P1 | **状态**: 📋 待规划
**价值**: 将 NPM 事实数据接入 Baseline、模型、规则匹配等检测能力，把插件服务推进为 SQL 可编排的流量分析闭环

### Story 21.1: Baseline 算子适配层
**状态**: 📋 待规划
**验收标准**:
- 在保留 `IBaselineService` 进程内服务边界的前提下，提供 `baseline.value`、`baseline.ratio`、`baseline.relation` 算子适配层
- 算子从 NPM 事实表或流式 batch 中构造 baseline observation
- 输出 score、band、maturity、trust、risk 和诊断字段
- 适配层不复制 Baseline 核心算法，只负责 SQL / batch / stream 输入输出桥接

---

### Story 21.2: NPM 特征到检测输入的标准化
**状态**: 📋 待规划
**验收标准**:
- 定义 flow / session 到检测特征的标准映射，例如 bps、pps、连接数、失败率、协议占比、关系分布
- 支持 Value / Ratio / Relation 三类检测输入
- 明确窗口粒度、source key、route key、metric 名称和缺失值策略
- 提供固定 NPM 样本的特征构造测试

---

### Story 21.3: 检测结果事实表与告警输出
**状态**: 📋 待规划
**验收标准**:
- 定义 `anomaly_event.v1` 或同等检测结果事实表
- 支持将检测结果写入数据库、stream sink 或后续 SIEM / 告警通道
- 结果包含事件时间、主体、指标、score、risk、解释字段和关联 evidence
- 提供 NPM → Baseline → anomaly event 的端到端测试

---

## Epic 22: 通用 WHERE 绑定与通道过滤下推架构
**优先级**: P1 | **状态**: 📋 待规划
**价值**: 将 SQL `WHERE` 从原始字符串传递升级为可解析、可绑定、可下推的通用过滤架构，让不同通道声明自身过滤能力，由 planner 生成 typed filter IR 并下推执行，避免在高吞吐路径中引入独立过滤算子的额外开销

### Story 22.1: `WHERE` AST 与通用 `FilterExpr` / `FilterPlan`
**状态**: 📋 待规划
**验收标准**:
- `SqlParser` 将 `WHERE` 子句解析为通用 AST，保留原始 `where_clause` 仅用于日志、兼容和错误提示
- AST 覆盖字段引用、字面值、比较操作、范围、列表、AND / OR / NOT 等基础表达能力；解析阶段不判断字段是否属于某类通道
- 定义通用 `FilterExpr` / `FilterPlan` IR，用于承载经过绑定后的 typed predicate、字段规范名、值类型、操作符和逻辑结构
- `FilterPlan` 支持版本化和序列化，短期可用 JSON 作为插件边界载体，避免不同通道自行解析 SQL 字符串
- 现有 DataFrame / 数据库 `WHERE` 路径保持兼容，并逐步迁移到新 IR

---

### Story 22.2: 通道 `FilterCapabilities` 能力声明
**状态**: 📋 待规划
**验收标准**:
- 定义通用过滤能力描述，至少包含 `filter_schema`、字段规范名、字段别名、值类型、支持操作符、逻辑操作支持、是否要求 full match、可执行阶段和可选加速方式
- 提供 `IFilterableChannel` 或等价接口，使 `IStreamChannel`、`IBlockStreamChannel`、DataFrame / Database 通道都能声明过滤能力
- 将现有 `supports_filter_pushdown` / `filter_requires_full_match` 从布尔能力升级为可枚举、可诊断的能力描述
- 不支持过滤的通道必须返回空能力或明确能力状态，planner 可以据此生成可读错误
- 能力声明按通道实例生效，允许同一通道类型在不同后端、不同配置或不同运行模式下暴露不同过滤能力

---

### Story 22.3: `WHERE` Binder 与过滤计划校验
**状态**: 📋 待规划
**验收标准**:
- planner 先解析 `FROM` 并解析源通道，再基于通道 `FilterCapabilities` 绑定 `WHERE` AST
- Binder 将用户字段名和别名绑定到通道声明的规范字段，例如 `ipaddress` → `packet.endpoint_ip`、`port` → `packet.endpoint_port`
- 对歧义字段必须显式消歧，例如 `protocol` 需绑定到 `l4_protocol` 或 `app_protocol`，绑定失败时给出明确错误
- 对通道不支持的字段、值类型、操作符、逻辑结构或 partial pushdown 场景输出结构化 unsupported reason
- 多源输入场景必须明确策略：按通道分别绑定、取能力交集或拒绝执行；共享源场景必须保持 `WHERE` signature 一致
- 禁止通道直接解析 SQL 字符串，通道只接收已绑定的 `FilterPlan`

---

### Story 22.4: 统一过滤下推接口与执行生命周期
**状态**: 📋 待规划
**验收标准**:
- 统一 `SetFilter()` 语义，使通道接收 `FilterPlan` 或其 JSON 表示，而不是未绑定的原始 `WHERE` 字符串
- 补齐 `IBlockStreamChannel` 的过滤下推能力，并与现有 `IStreamChannel`、fan-in、fan-out、stream hub、shared source 过滤路径保持一致
- 过滤计划必须在通道开始生产数据前设置；已启动共享源禁止变更过滤条件，除非显式创建新的 source instance
- `unsupported_out` 或错误响应必须包含字段、操作符、原因和建议改写方式，便于前端和 API 用户定位问题
- 支持 full pushdown 优先；如未来引入 partial pushdown，剩余 predicate 仍必须在通道内执行，避免重新引入独立热路径过滤算子

---

### Story 22.5: `packet_filter.v1` 过滤契约
**状态**: 📋 待规划
**验收标准**:
- 定义 `packet_filter.v1` 字段和语义，覆盖时间、source、来源文件、packet 序号、长度、MAC、EtherType、VLAN、IP、端口、L4 protocol、tunnel depth、parse status 等结构层过滤字段
- 明确 endpoint 语义字段，例如 `ip` / `mac` / `port` 表示源或目的任一端匹配，`src_ip` / `dst_ip` / `src_port` / `dst_port` 表示方向性匹配
- 支持 IP 等值、列表、CIDR、范围；端口等值、列表、范围；协议枚举和长度范围等基础操作
- 明确 `l4_protocol` 与 `app_protocol` 的边界，Epic 22 只定义能力描述和绑定规则，不要求所有通道支持应用层协议过滤
- `packet_filter.v1` 必须能够复用 `PacketLayerHints`，为 `pcapfile`、DPDK、AF_XDP 等 packet 通道提供一致过滤语义

---

### Story 22.6: `pcapfile` 作为 `packet_filter.v1` 首个验证实现
**状态**: 📋 待规划
**验收标准**:
- `pcapfile` 作为 Epic 22 的首个 packet 通道验证实现，支持声明 `packet_filter.v1` 的 `FilterCapabilities`，但不改变 Epic 19 的 pcapfile 主线边界
- 支持 `packet_filter.v1` 的结构层过滤，至少覆盖时间、长度、MAC、IP、端口、L4 protocol、VLAN 和 source / 文件元数据
- 过滤执行复用 `PacketLayerHints`，不调用应用层协议识别，不支持 `app_protocol` / `application` 等需要 NPI / NPM 语义的条件
- 不匹配 packet 不进入 `PacketBatchView`；时间、长度和来源类条件应在可行时先于 layer 解析执行，降低无效解析成本
- 保留原始 packet 可追踪性：来源文件、文件内 packet 序号、输出 `packet_id`、扫描包数、命中包数和过滤丢弃包数都可观测
- 过滤计划必须在 `PollBlock()` 开始前设置；通道启动后变更过滤计划必须明确拒绝或创建新的 source instance
- 覆盖单文件、多文件顺序读取和时间戳归并读取下的过滤回归，确保过滤不破坏事件时间、来源追踪和输出顺序

---

### Story 22.7: 过滤架构兼容回归与文档收口
**状态**: 📋 待规划
**验收标准**:
- DataFrame / 数据库现有 `WHERE` 场景通过新 AST / IR 路径保持行为兼容，既有端到端测试继续通过
- 覆盖成功过滤、unsupported 字段、unsupported 操作符、歧义字段、partial pushdown 禁止场景、共享源 `WHERE` signature mismatch 和 block stream 过滤生命周期回归
- 错误响应必须包含可读的字段名、操作符、通道能力信息和失败原因，便于 Web / API 用户定位问题
- 验证过滤下推不会重新引入独立热路径过滤算子；通道自身负责执行完整 predicate 或明确拒绝执行
- 文档同步说明 SQL `WHERE`、通道能力声明、Filter IR 和通道执行过滤之间的职责边界

---

## 优先级说明
- **P0**: 核心功能，必须实现
- **P1**: 重要功能，近期规划
- **P2**: 增强功能，中长期规划
- **P3**: 可选功能，按需实现

## 状态说明
- ✅ **已完成**: Story 已实施并验证通过
- 🚧 **进行中**: Story 正在实施
- 📋 **待规划**: Story 已识别，待排入 Sprint
- 💡 **设计阶段**: Story 需求明确，设计方案待定
