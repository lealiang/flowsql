# Sprint 19 Baseline 检视记录

## 评审信息

- 评审日期：2026-04-23
- 评审对象：
  - `src/plugins/baseline`
  - `tasks/sprints/sprint19-baseline/design.md`
- 审查范围：
  - baseline 插件代码是否按功能设计文档实现
  - 重点检查 `T1 / T2 / T3` 的核心算法语义是否真正落地
- 审查结论口径：
  - 本次不是做“代码是否能运行”的检查，而是做“代码是否符合设计”的一致性检查

## 本轮结论

1. 本轮迭代失败。
2. 当前产出不是 `design.md` 的实现，而是一个“主流程可运行、但核心算法能力大面积缺失”的空壳子。
3. 现有实现更接近“baseline 插件工程骨架 + 占位型模型”，尚不能视为设计定稿后的正式落地版本。
4. 已通过的编译和测试，只能证明代码框架可运行，不能证明 `design.md` 中定义的算法能力已经实现。

## 关键偏差

### D-01：`T1 / T2` 正式模型被实现为常数截距模型，未落地 `Core / monthpos / event`

- 优先级：P0
- 设计要求：
  - `design.md` 第 `6.5 / 6.6 / 7.4` 节明确要求正式模型满足：
    - `μ_t = μ_core(t) + h_monthpos(t) + h_event(t)`
    - `Core` 至少包含线性趋势、日周期、周周期
    - 训练采用 staged fit
    - 统一块求解器为 `weighted_huber_ridge_irls`
- 实际实现：
  - `formal_predictor.cpp` 明确写明：`v1` 正式模型先落为“常数截距项”
  - `formal_model_trainer.cpp` 对 `T1 / T2` 都只调用 `FitWeightedIntercept`
  - `formal_model.h` 中正式模型结构仅有 `intercept_x / intercept_ratio`，没有趋势、季节、月位置、事件项参数
- 影响：
  - `T1 / T2` 当前不具备设计定义的正式基线能力
  - `DST / tz / local wall clock` 等季节相位语义无法真正生效
  - `core_no_month_ready / full_ready` 等状态语义缺乏真实模型支撑

### D-02：`T2` 没有按设计走 `m0 + alpha0 / beta0 + logit` 数学路径

- 优先级：P0
- 设计要求：
  - `design.md` 第 `7.3 ~ 7.5` 节要求：
    - 先基于训练窗口计算 `m0(feature)`
    - 再派生 `alpha0(feature)`、`beta0(feature)`
    - 构造平滑比例 `p_t^smooth`
    - 在 `logit` 空间训练正式模型
    - 在线阶段先得到 `mu_hat_t`，再恢复 `p_hat_t = sigmoid(mu_hat_t)`
- 实际实现：
  - `ratio_detector_core.h/cpp` 中 `s_prior` 仅保存在 profile 中
  - 全代码未看到 `m0 / alpha0 / beta0 / logit / sigmoid / eps_logit` 的正式实现链路
  - `formal_model_trainer.cpp` 训练时直接对 `numerator / denominator` 做加权截距拟合
  - `ratio_detector_core.cpp` 在线评分时直接把 formal prediction 当作概率基线 `p_hat`
- 影响：
  - `T2` 最关键的边界稳定、低分母收缩、边界比例平滑，当前都没有落地
  - `rate_core / ratio_bursty` 的 profile 差异只剩分母门槛和 `phi_over`，数学语义明显缩水

### D-03：`EventCalendarSpec` 只做了元数据校验，没有实现 `h_event(t)` 事件层

- 优先级：P0
- 设计要求：
  - `design.md` 第 `6.5` 节明确：
    - `h_event(t) = Σ_e β_e * I(t ∈ W_e)`
    - 训练与预测必须使用同一 `calendar_id + calendar_version`
    - 事件层应作为正式模型的一部分参与训练和预测
- 实际实现：
  - `config_parser.cpp` 仅完成 `EventCalendarSpec` 的 JSON 解析和字段校验
  - `formal_model_trainer.cpp` 只把 `calendar_id / calendar_version` 写入 metadata
  - `formal_predictor.cpp` 只根据日历版本是否匹配返回 `event_status / event_enabled`
  - 预测值本身没有任何事件偏移项参与
- 影响：
  - 事件日历目前只是“元数据存在”，不是“事件层已实现”
  - 设计中关于特殊日期、外生事件的正式建模能力当前不存在

### D-04：`T3` 只落地了摘要特征路由，未落地模式融合层与 `FusionResult`

- 优先级：P0
- 设计要求：
  - `design.md` 第 `9.2` 节明确要求 `T3 v1` 为：
    - 关系分布摘要特征层
    - 模式证据计算
    - 跨指标模式合成
    - Key 级风险合成
    - `FusionResult` 输出，其中包含 `dominant_pattern`
  - 同时定义了：
    - `oppose_P`
    - `λ_opp`
    - 4 类局部模式
    - `Risk_T1T2 / Risk_T3 / Risk(Key,t)`
- 实际实现：
  - relation 任务已实现 `ServiceBasis / EvalBasis / summary_policy / support_policy`
  - `relation_router.cpp` 已把摘要特征路由到 `T1 / T2` 检测器
  - `baseline_task_base.cpp` 仅对 routed feature 的候选损失做平均，并据此决定 `formal_apply / direct_apply`
  - 代码中未看到 `oppose_P / λ_opp / dominant_pattern / FusionResult` 的实现
  - 接口层仍只有单特征 `DetectorResult` 输出
- 影响：
  - 当前 `T3` 不是设计中的“关系模式基线”，而只是“关系摘要 -> 单特征检测器”的半成品
  - 设计中最有解释力的模式层和融合层尚未落地

## 影响判断

1. `T1` 的趋势、周期、月位置、事件建模未落地，意味着当前基线不具备设计要求的时序解释能力。
2. `T2` 的平滑比例建模未落地，意味着当前比例类基线不具备设计要求的统计鲁棒性。
3. `T3` 的模式层未落地，意味着当前关系分布基线只完成了输入整理和摘要路由，没有完成设计定义的异常语义抽象。
4. 因此，当前 baseline 插件虽然具备：
   - 任务接口
   - 历史回放
   - `shadow baseline`
   - `Baseline Source`
   - `EvalBasis`
   - 重建切换骨架
   但这些能力承载的仍是占位型模型，不足以支撑本轮算法设计的交付目标。

## 根因复盘

### R-01：实现范围与设计范围失配

- 设计文档讨论已经收敛到正式方案，但实现阶段实际落地的是“先把工程骨架跑通”的占位版本。
- 这个“占位实现”没有在计划和验收标准里被明示为“阶段性子集”，导致过程上看似在持续推进，结果上却偏离了目标。

### R-02：缺少“设计条款 -> 代码实现”的逐条验收清单

- 本轮缺少一份严格的映射表，去核对：
  - 哪些设计条款已经实现
  - 哪些只是接口预留
  - 哪些仍是占位
- 结果就是很多能力只完成了：
  - 结构体
  - metadata
  - 调用链
  - 状态机骨架
  但没有完成算法本体。

### R-03：测试验证的是“流程可运行”，不是“设计能力已落地”

- 当前测试通过，主要证明了：
  - 接口可调用
  - 重建链路可执行
  - 状态切换不崩
  - relation routed detector 主流程可跑
- 但没有证明：
  - 趋势项存在
  - 季节项存在
  - `monthpos` 存在
  - `event` 存在
  - `T2 logit` 建模存在
  - `T3` 模式融合存在

## 后续要求

1. 后续继续 baseline 开发前，必须先建立“设计条款到代码实体”的映射清单。
2. 对每一项核心算法能力，必须明确标记其状态：
   - 已实现
   - 仅接口预留
   - 仅元数据预留
   - 未实现
3. 后续验收不能再以“编译通过、测试通过、主流程跑通”替代“算法已按设计实现”。
4. 在重新进入实现前，应先把 `planning.md` 的 story 粒度回收到“可直接对应设计能力”的层次，避免再次出现“骨架完成但算法未落地”的偏差。

## 当前状态

- 本轮 baseline 迭代：失败
- 当前代码状态：工程骨架已形成，但核心算法实现与 `design.md` 存在重大偏差
- 后续处理建议：以本评审结论为准，重新梳理实施范围与验收标准后再继续推进
