# Sprint 14 RCA：两次段错误复盘与防复发方案

## 1. 背景与范围

本报告针对 Sprint 14 开发过程中出现的两次段错误进行复盘：

1. `PluginLoader::Load` 传参不一致导致越界读取
2. `fnRouterHandler` 在插件 `Unload` 后仍被持有，触发悬挂回调

目标是总结共同根因，并形成下轮迭代（重构）可执行输入。

---

## 2. 事件复盘

### 2.1 事件 A：`libs/opts/count` 不一致

现象：

1. 测试新增插件后，`libs` 数量变化，但 `opts` 未同步
2. `PluginLoader::Load(path, relapath[], option[], count)` 按 `count` 索引 `option[pos]`
3. 触发越界读取并导致崩溃

直接原因：

1. 接口是“并行数组 + 手动 count”，调用侧可表达非法状态
2. 缺少长度一致性保护

修复方式：

1. 补齐 `opts` 与 `libs` 的一一对应
2. 调整 `count` 与数组真实长度一致

---

### 2.2 事件 B：`fnRouterHandler` 跨 `Unload` 生命周期

现象：

1. 测试通过 `FindRouteHandler` 拿到 `fnRouterHandler` 并持有
2. 执行 `loader->StopAll(); loader->Unload();`
3. 未清空全部 handler 时，析构/调用路径触达已卸载插件代码段，触发段错误

直接原因：

1. `fnRouterHandler` 是 owning 的 `std::function`，可捕获插件内部 lambda
2. 生命周期契约未显式约束“Unload 前必须释放外部缓存回调”

修复方式：

1. 在 `Unload` 前统一将测试中缓存的 `fnRouterHandler` 置空

---

## 3. 共同根因（核心）

两次问题本质一致：**跨边界资源的生命周期没有被接口建模约束，依赖人工同步维护**。

具体表现：

1. API 可表达非法状态  
   - 并行数组 + count  
   - 跨 so 长持有回调
2. 所有权/失效点不明确  
   - 谁负责释放，何时失效，失效后还能否调用/析构，不在类型层表达
3. 验证盲区  
   - 功能路径通过，但“卸载路径/生命周期边界”未作为强制验证

---

## 4. 影响评估

1. 风险等级：高（直接进程崩溃）
2. 触发条件：中等（插件增减、路由新增、e2e 句柄缓存都可能触发）
3. 波及范围：
   - 插件加载/卸载
   - 管理面路由测试
   - 后续所有新增插件与路由用例

---

## 5. 已完成修复（本迭代内）

1. 修复 `libs/opts/count` 不一致问题
2. 修复 `fnRouterHandler` 在 `Unload` 前未清空的问题
3. 回归验证：
   - `test_scheduler_e2e` 全通过且退出码 `0`

---

## 6. 防复发方案（下轮重构输入）

### 6.1 接口层（优先级 P0）

1. 收敛 `PluginLoader::Load` 调用形态，废弃“并行数组 + 手动 count”入口
2. 新增单结构体加载入口（示例）：
   - `struct PluginSpec { std::string so; std::string option; };`
   - `Load(path, std::vector<PluginSpec>)`
3. 保留旧接口时增加硬校验：
   - `relapath == nullptr`/`option == nullptr`/`count <= 0` 直接失败
   - 调试模式下增加 assert 与日志

### 6.2 生命周期层（优先级 P0）

1. 制定约束：所有跨插件缓存的 `fnRouterHandler` 必须在 `Unload` 前释放
2. 在测试基类引入统一清理器（RAII），避免手工逐个置空遗漏
3. 中长期重构方向：路由调用改为“路由键 + 调度器查询执行”，避免跨 so 持有 owning 回调对象

### 6.3 测试与工具层（优先级 P1）

1. 新增“卸载安全”回归：
   - 获取路由句柄 -> StopAll/Unload -> 验证无崩溃
2. 增加 Loader 参数一致性单测：
   - 数量不一致时返回错误而非崩溃
3. 将 ASAN/UBSAN 用例纳入 nightly（至少覆盖 plugin load/unload + e2e）

### 6.4 评审流程层（优先级 P1）

新增 CR 检查项：

1. 是否引入并行数组 + 外部 count
2. 是否跨 `Unload` 缓存插件内回调/对象
3. 是否覆盖 `StopAll/Unload` 收敛路径

---

## 7. 经验总结

1. 对插件系统，功能正确只是第一层，**生命周期正确**才是稳定性关键
2. “人工保持一致”的接口一定会在演进中失效
3. 需要把约束前移到类型和接口，而不是依赖测试末尾补救

---

## 8. 建议的下轮迭代任务（草案）

1. Story：PluginLoader 安全接口重构（移除并行数组风格）
2. Story：路由句柄生命周期治理（Unload 安全）
3. Story：插件生命周期专项测试集（含 ASAN/UBSAN）

该草案可作为下一轮“稳定性重构”输入。

