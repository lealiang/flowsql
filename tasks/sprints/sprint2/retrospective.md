# Sprint 2 回顾

## Sprint 信息
- **Sprint 周期**: Stage 2 阶段
- **回顾日期**: Stage 2 完成时

## 做得好的地方 (Keep)
1. **IChannel 接口重构**: 接口分层清晰，职责明确
2. **Arrow IPC 零拷贝**: C++ ↔ Python 数据传输性能优异
3. **Vue.js 前端**: 用户界面友好，交互流畅
4. **端到端测试**: 覆盖核心路径，质量有保障

## 需要改进的地方 (Problem)
1. **PluginRegistry 单例问题**: 静态库链接到多个 .so 时单例失效，导致插件注册失效
2. **Python Worker 资源泄漏**: 文件描述符、socket、线程等资源泄漏
3. **代码审查不及时**: 多个问题在后期才发现，增加返工成本

## 行动项 (Action Items)
1. **回归纯插件架构**: Sprint 3 中删除 PluginRegistry，回归纯 PluginLoader（✅ 已完成）
2. **代码审查修复**: Sprint 3 中修复 Python Worker 资源泄漏（✅ 已完成）
3. **建立代码审查流程**: 每个 Story 完成后立即进行代码审查

## 经验教训总结

### 教训 1: 静态库单例问题
**描述**: 静态库链接到多个 .so 时，每个 .so 有独立的单例实例
**固化到 CLAUDE.md**:
```markdown
## 架构原则
- 避免静态库单例：静态库链接到多个 .so 时单例失效
- 优先使用共享库：或回归纯插件架构
```

### 教训 2: 资源泄漏要及时发现
**描述**: Python Worker 资源泄漏在后期才发现，增加返工成本
**固化到 CLAUDE.md**:
```markdown
## 代码审查原则
- 每个 Story 完成后立即进行代码审查
- 重点关注资源泄漏、线程安全、错误处理
- 使用工具辅助（valgrind、AddressSanitizer）
```

## 团队士气
**评分**: 8/10
**反馈**: Sprint 2 目标全部达成，但发现了一些技术债务，需要在 Sprint 3 中解决。
