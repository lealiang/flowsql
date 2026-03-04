# Sprint 2 规划

## Sprint 信息
- **Sprint 周期**: Stage 2 阶段
- **Sprint 目标**: 实现 C++ ↔ Python 桥接和 Web 管理系统

## Sprint 目标
1. 增强插件系统（三阶段加载、动态注册）
2. 实现 C++ ↔ Python 桥接（PythonOperatorBridge + Python Worker）
3. 重构 IChannel 接口（数据读写下沉子类）
4. 实现 SQL 解析器和 Pipeline 重构
5. 实现 Web 后端 API 和 Vue.js 前端
6. 端到端集成测试

## 选入的 Story
- Story 2.1: 插件系统增强（模块 P）
- Story 2.2: C++ ↔ Python 桥接（模块 A）
- Story 2.3: IChannel 重构
- Story 2.4: SQL 解析器 + Pipeline 重构
- Story 2.5: Web 后端 API
- Story 2.6: Vue.js 前端
- Story 2.7: 端到端集成测试

详细任务分解参见 product_backlog.md

## 设计决策

### 决策 1: IChannel 接口分层
**理由**: 生命周期、元数据和数据操作分离，提升接口清晰度
**影响**: 需要重构所有现有插件

### 决策 2: IOperator::Work 签名改为 Work(IChannel*, IChannel*)
**理由**: 算子内部 dynamic_cast 到所需通道子类型，灵活性更高
**影响**: 算子需要处理类型转换失败的情况

### 决策 3: Python Worker 使用 FastAPI
**理由**: 异步高性能，生态完善，易于开发
**影响**: 需要引入 FastAPI 和 uvicorn 依赖

### 决策 4: Arrow IPC 作为 C++ ↔ Python 数据交换格式
**理由**: 零拷贝传输，性能优异
**影响**: 需要实现 ArrowIpcSerializer

## 技术债务
1. **单元测试覆盖率不足**: 需要补充单元测试（已识别，待 Sprint 3 解决）
2. **PluginRegistry 单例问题**: 静态库链接到多个 .so 时单例失效（已识别，待 Sprint 3 解决）

## 风险和缓解措施

### 风险 1: Python Worker 进程管理复杂
**影响**: 进程启动、停止、重启、僵尸进程等问题
**缓解措施**: 使用 PythonProcessManager 统一管理，实现健康检查和自动重启

### 风险 2: C++ ↔ Python 数据序列化性能
**影响**: 大数据量场景性能瓶颈
**缓解措施**: 使用 Arrow IPC 零拷贝传输，避免 JSON 序列化

## Sprint 回顾
（在 Sprint 2 完成后填写，参见 sprint2/review.md 和 sprint2/retrospective.md）
