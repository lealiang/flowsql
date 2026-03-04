# Sprint 1 规划

## Sprint 信息
- **Sprint 周期**: Stage 1 阶段
- **Sprint 目标**: 建立 C++ 框架核心能力，实现插件式进程框架基础

## Sprint 目标
实现 FlowSQL 的核心框架，包括：
1. 统一数据接口（IDataEntity/IDataFrame）
2. 插件化架构（IChannel/IOperator）
3. 核心框架（PluginRegistry/Pipeline/Service）
4. Apache Arrow 集成
5. 示例插件和测试验证

## 选入的 Story

### Story 1.1: 统一数据接口设计
**优先级**: P0
**估算**: 3 天
**验收标准**: IDataEntity 和 IDataFrame 接口完成，支持 JSON 和 Arrow IPC 序列化

**任务分解**:
- [x] Task 1.1.1: 定义 DataType 枚举和 FieldValue variant
- [x] Task 1.1.2: 实现 IDataEntity 接口（GetSchema/GetFieldValue/SetFieldValue/ToJson/FromJson/Clone）
- [x] Task 1.1.3: 实现 IDataFrame 接口（行列操作/Arrow 互操作/序列化）
- [x] Task 1.1.4: 实现 DataFrame 类（两阶段模式：构建期 builders_ → 读取期 batch_）

---

### Story 1.2: 通道和算子接口
**优先级**: P0
**估算**: 2 天
**验收标准**: IChannel 和 IOperator 接口定义完成，支持插件化架构

**任务分解**:
- [x] Task 1.2.1: 定义 IChannel 接口（继承 IPlugin，含 Catelog/Name/Open/Close/Put/Get/Flush）
- [x] Task 1.2.2: 定义 IOperator 接口（继承 IPlugin，含 Work/Configure/Position）
- [x] Task 1.2.3: 定义 OperatorPosition 枚举（STORAGE/DATA）
- [x] Task 1.2.4: 实现注册宏（framework/macros.h）

---

### Story 1.3: 核心框架实现
**优先级**: P0
**估算**: 5 天
**验收标准**: PluginRegistry、Pipeline 和 Service 核心框架实现完成

**任务分解**:
- [x] Task 1.3.1: 实现 PluginRegistry（LoadPlugin/UnloadAll/GetChannel/GetOperator/Traverse）
- [x] Task 1.3.2: 实现 Pipeline（Run 循环：批量读取 → Work → 批量写入）
- [x] Task 1.3.3: 实现 PipelineBuilder（链式构建：AddSource/SetOperator/SetSink/SetBatchSize/Build）
- [x] Task 1.3.4: 实现 Service 主框架
- [x] Task 1.3.5: Pipeline 状态机（IDLE/RUNNING/STOPPED/FAILED）

---

### Story 1.4: 示例插件和测试
**优先级**: P0
**估算**: 2 天
**验收标准**: MemoryChannel 和 PassthroughOperator 实现，test_framework 5 个用例全部通过

**任务分解**:
- [x] Task 1.4.1: 实现 MemoryChannel（std::queue 存储，Put/Get/Open/Close）
- [x] Task 1.4.2: 实现 PassthroughOperator（直接复制输入到输出）
- [x] Task 1.4.3: 编写框架集成测试（插件加载/DataFrame 操作/Pipeline 数据流通）
- [x] Task 1.4.4: 验证现有 NPI 插件不受影响

---

### Story 1.5: Apache Arrow 集成
**优先级**: P0
**估算**: 2 天
**验收标准**: Arrow 依赖集成完成，支持 IPC 和 JSON 模块

**任务分解**:
- [x] Task 1.5.1: 编写 arrow-config.cmake（ExternalProject_Add）
- [x] Task 1.5.2: 配置编译选项（ARROW_BUILD_STATIC/ARROW_IPC/ARROW_JSON）
- [x] Task 1.5.3: CMakeLists.txt 集成（add_thirddepen 宏）
- [x] Task 1.5.4: 验证编译和链接

---

## 设计决策

### 决策 1: 使用 Apache Arrow 作为数据交换格式
**理由**:
- 零拷贝传输，性能优异
- 跨语言支持（C++/Python/Java 等）
- 列式存储，适合分析场景
- 社区活跃，生态完善

**影响**:
- 需要引入 Arrow 依赖（约 100MB 编译产物）
- 学习曲线较陡

---

### 决策 2: 两阶段 DataFrame 模式
**理由**:
- 构建期使用 ArrayBuilder，支持动态追加
- 读取期使用 RecordBatch，零拷贝传递
- 避免频繁的内存分配和拷贝

**影响**:
- Finalize() 后不可再追加数据
- 需要明确构建期和读取期的边界

---

### 决策 3: Pipeline 负责 Channel ↔ DataFrame 转换
**理由**:
- 算子只操作 DataFrame，简化算子开发
- Pipeline 统一处理批量读写和格式转换
- 支持批量处理优化（batch_size 可配置）

**影响**:
- Pipeline 职责较重
- 后续需要重构（Stage 2 中 Pipeline 简化为纯连接器）

---

### 决策 4: 插件注册宏简化开发
**理由**:
- 减少样板代码
- 统一注册模式
- 降低插件开发门槛

**影响**:
- 宏展开后代码可读性下降
- 调试困难

---

## 技术债务
1. **Pipeline 职责过重**: 后续需要重构为纯连接器（已在 Sprint 2 中解决）
2. **IChannel 接口混杂**: 生命周期、元数据和数据操作混在一起（已在 Sprint 2 中解决）
3. **PluginRegistry 单例问题**: 静态库链接到多个 .so 时单例失效（已在 Sprint 3 中解决）

---

## 风险和缓解措施

### 风险 1: Arrow 编译时间过长
**影响**: 开发效率降低
**缓解措施**:
- 使用 pyarrow 提供的预编译库
- 缓存编译产物到 .thirdparts_prefix
- 第三方依赖隔离到 build 目录外

---

### 风险 2: DataFrame 性能不足
**影响**: 大数据量场景性能瓶颈
**缓解措施**:
- 使用 Arrow RecordBatch 零拷贝传递
- 批量处理优化（batch_size 可配置）
- ReserveRows 预分配内存

---

## Sprint 回顾
（在 Sprint 1 完成后填写，参见 sprint1/review.md 和 sprint1/retrospective.md）
