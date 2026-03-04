# Sprint 3 规划

## Sprint 信息
- **Sprint 周期**: Stage 3 阶段（部分完成）
- **Sprint 目标**: 实现数据库闭环能力，清理架构债务

## Sprint 目标
1. 回归纯插件架构（删除 PluginRegistry 和 libflowsql_framework.so）
2. 接口解耦（IChannel/IOperator 去掉 IPlugin 继承）
3. 实现 IBridge 接口（替代 dynamic_cast hack）
4. 实现数据库通道能力（IDatabaseFactory + DatabasePlugin + SQLite 驱动）
5. 实现 SQL WHERE 解析和 DataFrame Filter
6. 实现 ChannelAdapter 自动适配
7. 代码审查和修复
8. 端到端测试

## 选入的 Story
- Story 3.1: 架构重构 - 纯插件架构回归
- Story 3.2: 接口解耦
- Story 3.3: IBridge 接口
- Story 3.4: arrow_codec.py 统一转换层
- Story 3.5: IDatabaseFactory 工厂接口 + DatabasePlugin
- Story 3.6: IDbDriver 驱动抽象 + SQLite 驱动
- Story 3.7: DatabaseChannel 通道实现
- Story 3.8: SQL WHERE 解析 + DataFrame Filter
- Story 3.9: 安全基线
- Story 3.10: Scheduler 集成
- Story 3.11: ChannelAdapter 自动适配
- Story 3.12: 端到端测试
- Story 3.13: 代码审查修复
- Story 3.14: 清理任务

详细任务分解参见 product_backlog.md

## 设计决策

### 决策 1: 回归纯插件架构
**理由**: 解决 PluginRegistry 单例问题，简化架构
**影响**: 需要删除 libflowsql_framework.so，Pipeline/ChannelAdapter 移入 scheduler.so

### 决策 2: IPlugin::Load 签名改为 Load(IQuerier*)
**理由**: 插件通过 IQuerier 查询其他插件，避免 IRegister 的注册职责
**影响**: 所有插件需要适配新签名

### 决策 3: IDatabaseFactory 工厂模式
**理由**: 支持多数据库实例管理，懒加载获取通道
**影响**: 需要实现配置解析和环境变量替换

### 决策 4: IDbDriver 驱动抽象
**理由**: 支持多种数据库驱动（SQLite/MySQL/PostgreSQL）
**影响**: 需要为每种数据库实现驱动

### 决策 5: ChannelAdapter 自动适配
**理由**: 去掉显式存储/提取算子，Pipeline/Scheduler 层自动感知通道类型差异
**影响**: 需要实现 4 种无算子路径和有算子时的自动适配

## 技术债务
1. **单元测试覆盖率不足**: 需要补充单元测试（待后续 Sprint 解决）
2. **MySQL/PostgreSQL 驱动**: 需要实现（待后续 Sprint 解决）

## 风险和缓解措施

### 风险 1: 架构重构影响范围大
**影响**: 可能引入新的 bug
**缓解措施**: 充分测试，回归测试现有功能

### 风险 2: SQLite 驱动性能
**影响**: 大数据量场景性能瓶颈
**缓解措施**: 使用 WAL 模式、批量写入、流式读取

## Sprint 回顾
（在 Sprint 3 完成后填写，参见 sprint3/review.md 和 sprint3/retrospective.md）
