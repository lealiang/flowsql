# Sprint 19 Baseline 回顾总结

日期：2026-04-23
范围：围绕 baseline 迭代全过程复盘，重点分析 `design.md -> plugin-design.md -> planning.md -> code` 这条落地链路为何失真。

## 1. 结论

Sprint 19 baseline 迭代失败。

这次失败的主要问题，不在前期算法方案设计，而在后续落地阶段的失真。前期的功能设计文档 [design.md](/mnt/d/working/flowSQL/tasks/sprints/sprint19-baseline/design.md) 是细致且完整的，问题集中出现在后续两个环节：

1. [plugin-design.md](/mnt/d/working/flowSQL/tasks/sprints/sprint19-baseline/plugin-design.md) 不完整，没有严格作为 `design.md` 的代码映射文档来写。
2. [planning.md](/mnt/d/working/flowSQL/tasks/sprints/sprint19-baseline/planning.md) 产出草率，没有按设计能力闭环拆解，后期多次临时增补，反映出前期计划约束不足。

最终结果是：工程骨架和主流程被较快搭了起来，但核心算法能力没有按 `design.md` 正式落地，形成了“看似正确、实则空心”的产出。

## 2. 主问题定位

### 2.1 `design.md` 不是问题源头

本轮 baseline 方案设计阶段做得比较扎实，`design.md` 对以下内容已经给出了完整约束：

- `T1 / T2 / T3` 的数学对象与输入规格
- 正式模型结构
- 训练目标与 staged fit
- `shadow baseline / rebuild / EvalBasis / Baseline Source`
- 事件层、月位置层、关系模式层的边界

从后续代码审视结果看，问题不是“设计不清晰”，而是“设计没有被后续文档和实施链路完整承接”。

### 2.2 问题发生在落地链路的后两层

真正失控的地方是：

1. `design.md -> plugin-design.md`
2. `plugin-design.md -> planning.md`

也就是说，问题不是“不会设计算法”，而是“没有把算法设计正确转换成代码设计和实施计划”。

## 3. `plugin-design.md` 为什么会出问题

### 3.1 直接原因

`plugin-design.md` 更像一份“插件架构设计”，而不是一份“算法设计到代码实体的映射设计”。

它比较完整地回答了这些问题：

- 插件叫什么
- interface 怎么定义
- task 颗粒度是什么
- history reader 怎么接
- 生命周期和重建框架如何组织

但没有完整回答这些更关键的问题：

- `T1 Core` 由哪些类和模块承载
- `monthpos` 的设计矩阵在哪里构造
- `event` 的 indicator 在哪里生成
- `T2` 的 `m0 / alpha0 / beta0 / logit` 在哪一层实现
- `T3` 的模式融合层由哪些对象承载
- 哪些对象只是接口预留，哪些是正式算法实现

结果就是：插件外壳被设计出来了，但算法内部设计没有被完整下钻到代码结构级别。

### 3.2 根因分析

#### 原因 1：文档定位偏了

我把 `plugin-design.md` 写成了“工程架构设计”，没有写成“算法约束到代码实体的映射设计”。

对于 baseline 这种任务，接口设计只是第一层，真正关键的是内部算法对象的代码化设计，例如：

- feature builder
- design matrix builder
- staged trainer
- block solver contract
- predictor
- formal model schema
- validation / replay semantics

这些内容没有被完整写透，导致编码阶段自然会倾向于“先把接口和主流程跑通”。

#### 原因 2：默认接受了“骨架先行”的推进方式

在没有明确声明的情况下，我默认采用了“先搭插件骨架，再逐步补算法”的策略。

如果这是一种明确约定的分阶段策略，本身可以接受；但这次的问题在于：

- 没有提前向你明确说明这是“阶段性占位实现”
- 没有在文档里明确标出哪些部分是占位
- 没有把“占位实现”单独列为受控范围

所以后续从文档表面看，像是在“按设计推进”；但从代码本质看，实际在做“骨架优先”。

#### 原因 3：缺少 `design -> code` 映射表

我没有建立一张强约束映射表，去逐条回答：

- 设计条款是什么
- 对应代码实体是什么
- 当前状态是已实现、占位实现，还是未实现

没有这张表，很多能力就容易滑向以下几种“伪完成”状态：

- 结构体有了
- metadata 有了
- 调用链有了
- 状态机有了

但算法本体没有实现。

#### 原因 4：内部代码设计下钻不够

我在早期没有把 `T1 / T2 / T3` 的内部 routed detector core、formal trainer、feature matrix、pattern fusion 这些对象一次性定义清楚。

结果是：

- 外部接口越写越清晰
- 内部算法对象却一直依赖后续临时补充

这直接削弱了 `plugin-design.md` 作为编码约束文档的作用。

## 4. `planning.md` 为什么会出问题

### 4.1 直接原因

`planning.md` 不是按“设计能力闭环”拆的，而更像按“工程推进感觉”拆的。

因此，计划早期优先完成了很多工程基础项，例如：

- 插件接口
- task 注册
- runtime
- rebuild queue
- history reader
- snapshot
- relation routed detector 骨架

这些内容当然重要，但它们不是算法能力本身。于是就出现了一种错觉：

- 计划完成度看起来很高
- 代码也一直在推进
- 但真正对应 `design.md` 的核心算法能力并没有同步落地

### 4.2 根因分析

#### 原因 1：Story 不是按设计能力拆的

一个高质量的 Story，应该能够明确回答一句话：

“这个 Story 完成后，`design.md` 中哪一段能力被正式实现了？”

本轮很多 Story 更像是在回答：

- 做一个模块
- 接一个链路
- 先把重建框架跑通
- 先把 relation routed detector 跑通

这类 Story 适合工程骨架推进，但不适合算法实现验收。

#### 原因 2：Story 缺少设计验收条款

本轮很多 Story 没有在计划阶段写清楚以下几类信息：

- 设计来源：对应 `design.md` 哪几节
- 必须实现：哪些公式、状态、字段、行为
- 非目标：本 Story 明确不做什么
- 禁止事项：哪些占位做法不允许

因为缺少这些约束，后续才会不断出现 `18.10A / 18.12A` 这类临时增补，计划不断“补洞”，说明一开始的 Story 拆解并不闭合。

#### 原因 3：没有把“占位实现”视为受控异常

如果计划阶段明确规定：

- 不允许只落 metadata 不落算法
- 不允许只落接口不落数学流程
- 不允许用 intercept 占位替代正式模型，除非单独立 Story 并显式标注为临时

那么本轮很多偏差在计划评审时就会被拦下来。

现在的问题是，占位实现实际上存在，但没有被计划层显式暴露和控制。

#### 原因 4：缺少阶段门禁

这轮实施过程中，缺少以下 3 个强制门禁：

1. `design -> plugin-design` 映射评审
2. `plugin-design -> planning` Story 对齐评审
3. 编码中途的设计一致性回看

结果就是，问题没有在文档阶段被拦下，而是一路滑到代码审视时才集中暴露。

## 5. 责任判断

这两个问题的主要责任在我。

核心责任点有 3 个：

1. 我把“设计落地”错误地转成了“骨架先行”的工程推进方式，而且没有先明确征得你的同意。
2. 我没有把 `plugin-design.md` 写成真正的“算法到代码实体映射文档”。
3. 我没有把 `planning.md` 写成真正的“按设计能力验收的实施合同”。

## 6. 如何改进

### 6.1 增加一层正式的“设计映射表”

后续类似 baseline 这种重算法任务，不能直接从 `design.md` 跳到 `plugin-design.md`。

中间必须增加一层正式映射，至少包含：

- 设计条款编号
- 算法能力名
- 代码承载模块
- 输入结构
- 中间状态
- 输出模型字段
- 在线评分入口
- 是否允许占位
- 当前状态：未实现 / 占位实现 / 正式实现

这张表是后续所有代码设计和计划拆解的基础。

### 6.2 重写 `plugin-design.md` 的定位

以后 `plugin-design.md` 必须拆成两部分：

1. 插件外部设计
   - 命名
   - interface
   - task 颗粒度
   - history reader
   - 生命周期

2. 算法内部设计
   - `T1 / T2 / T3` 各自的 code path
   - feature builder / matrix builder / trainer / predictor / model
   - staged fit 的代码分层
   - `shadow / rebuild / validation / source` 如何接入

第二部分没写完，就不能进入编码。

### 6.3 重写 `planning.md` 的拆解方法

以后 Story 必须按“能力闭环”拆，而不是按“工程模块”拆。

更合理的拆法应类似：

- Story A：`T1 Core` 正式训练与预测闭环
- Story B：`T1 monthpos` 正式训练与预测闭环
- Story C：`T1 event` 正式训练与预测闭环
- Story D：`T2 m0 / alpha0 / beta0 / logit` 闭环
- Story E：`T2` 方差层与在线评分闭环
- Story F：`T3` 摘要提取闭环
- Story G：`T3` 模式融合闭环
- Story H：`FusionResult` 输出闭环

每个 Story 都必须能直接映射到 `design.md` 的一段正式能力。

### 6.4 给每个 Story 强制补齐 4 类字段

以后每个 Story 都必须明确写出：

- 设计来源：对应 `design.md` 哪几节
- 必须实现：哪些公式、状态、字段、行为必须出现
- 明确不做：本 Story 不覆盖哪些内容
- 禁止事项：哪些占位做法不允许

这样做的目的，是防止“主流程能跑”掩盖“算法没落地”。

### 6.5 增加 3 个强制评审点

后续类似任务，至少固定 3 次评审：

1. `design -> plugin-design` 映射评审
2. `plugin-design -> planning` Story 对齐评审
3. 实施中期的设计一致性评审

不能等到迭代结束才第一次做设计一致性审查。

### 6.6 强制区分 3 种实现状态

以后所有文档和计划都必须清楚区分：

- 正式实现
- 占位实现
- 仅接口预留

这三种状态不能混写，更不能默认把占位实现当成正式能力。

## 7. 本次迭代沉淀的教训

1. 对于重算法任务，`design.md` 之后不能直接进入编码，必须先完成“算法到代码实体”的映射设计。
2. `plugin-design.md` 如果只写接口和架构，不写算法内部对象设计，就不足以支撑实现。
3. `planning.md` 如果不按设计能力闭环拆解，后期必然不断补洞，最终计划完成度会失真。
4. 编译通过、测试通过、主流程跑通，只能证明工程骨架可运行，不能证明算法已经按设计实现。
5. 对于这类任务，“骨架先行”不是不能做，但必须事先明示、单独建档、明确验收边界，不能伪装成“正式实现”。

## 8. 总结

这轮 baseline 迭代失败，不是因为前期算法方案设计不够细，而是因为我后面把“按设计落地”错误地转换成了“骨架优先的工程推进”，又没有用代码设计文档和计划文档把这种转换显式化、约束化、验收化。

后续真正要改的，不是某一个 Story，而是整个 `design.md -> plugin-design.md -> planning.md -> code` 的落地方法。
