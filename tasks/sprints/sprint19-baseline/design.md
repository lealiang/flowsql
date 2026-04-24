# Sprint 19 基线统一设计（Metric + Relation）

## 1. 设计目标

在同一套框架内统一解决两类问题：

1. 单指标基线：如 `bytes`、`conn_count`、`error_rate` 等时序异常。  
2. 连接关系基线：如 `entity_A <-> entity_B` 的关系结构异常（新关系、分布漂移、占比突变、扫描模式等）。

目标能力：

- 分钟级精度（1 min）；
- 同一套算法与训练机制；
- 支持通用实体，不绑定 `server_ip:port`；
- 面向高基数关系可扩展；
- 输出统一风险分与可解释证据。

---

## 2. 统一抽象

### 2.1 实体与关系

- 实体（Entity）：任意业务对象，如 `server_ip:port`、`service`、`tenant`、`app`、`zone`。  
- 关系（Relation）：`src_entity -> dst_entity` 或 `A <-> B` 的连接行为。

### 2.2 统一观测对象

统一输入对象定义为：

```text
Observation = {
  key,
  feature,
  bucket_id,
  value,
  context
}
```

字段含义：

- `key`：被分析主体，如 `server_ip:port`、`service`、`tenant`
- `feature`：该主体上的某个被检测特征，如 `bytes_total`、`new_peer_ratio`、`client_group_mix`
- `bucket_id`：固定统计窗口的绝对编号，作为热路径时间索引
- `value`：当前窗口特征值
- `context`：辅助解释该值所需的上下文，如样本量、分母、Top-K 明细、group 分布等

语义说明：

- `key` 决定“谁在被建模”
- `feature` 决定“检测哪一种行为”
- `bucket_id` 决定“落在哪个统计时间桶”
- `context` 决定“如何正确解释当前值”
- `Observation` 是上游聚合层产出的稀疏统计记录，不是基线层对原始事件再做一次窗口聚合
- 上游只向基线层上报可用记录；不完整、可疑或不可判定的 bucket 不进入 `Observation`，而是直接表现为 gap
- 对于没有记录到达的时间桶，基线层视为 gap，通过相邻 `bucket_id` 的跳变惰性处理，不逐桶展开空记录

与 `Observation` 配套的静态规格定义为：

```text
FeatureSpec = {
  delta,
  tz,
  stat_meta
}
```

其中：

- `delta`：固定统计窗口宽度，如 `1 min`、`5 min`
- `tz`：时间语义所属时区
- `stat_meta`：仅保留确实会影响算法公式、阈值或状态更新的最小静态元数据；若某语义已编码进 `feature` 身份，则不在此重复表达

时间口径统一约束：

- `bucket_id` 必须基于 `UTC` 连续编号；它是热路径内部唯一的绝对时间索引
- `tz` 不参与 `bucket_id` 编号，只用于把 `bucket_id + delta` 映射为本地日历字段与本地时钟相位
- 趋势项使用绝对时间索引；日 / 周周期、月位置和事件窗口使用本地日历语义
- `DST`（夏令时）切换日按真实本地时钟处理，允许出现 `23 h` 或 `25 h` 的自然日；`v1` 不做补桶、压桶或额外相位补偿

### 2.3 基线来源

`基线来源`（Baseline Source）是上游为某个 `(key, feature)` 可选显式配置的候选基线提供者，用于在本级基线尚未可服务或暂时不可用时，临时借用其他可服务来源的基线输出。它是统一机制，适用于所有特征类型，不限于 `T2`。

统一定义为：

```text
BaselineSourceConfig(key, feature) = [
  source_0 = self,
  source_1,
  source_2,
  ...
]
```

其中：

- `self`：当前 `(key, feature)` 自己的基线
- `source_i`：上游为该 `(key, feature)` 显式配置的其他候选来源，按优先级排序

约束：

- `BaselineSourceConfig` 由上游配置 / 注册层可选提供，不作为每条运行时 `Observation` 的必带字段
- `BaselineSourceConfig` 的生效粒度固定为单个 `Series = (key, feature)`；不得把某个 `key` 的来源配置扩散到同一 task 下的其他 `key`
- 若工程实现中一个 task 承载多个 runtime `key`，则必须通过 `(key, feature) -> BaselineSourceConfig?` 的查找表或等价 resolver 获取来源配置，而不是在 task 级保存一份全局 `BaselineSourceConfig`
- 算法层只消费该配置并选择可用来源，不负责推断来源关系
- `source_i` 必须与 `self` 共享同一 `feature` 身份、同一时间粒度、同一 `bucket_id` 对齐方式，以及同一统计口径；若口径不一致，则禁止配置为同一特征的基线来源
- 该机制不预设树形父子结构，也不要求来源之间存在固定层级语义
- 若上游未提供 `BaselineSourceConfig`，则基线算法必须能够独立运行，只是禁用该可选能力

统一选择规则：

```text
if Serviceable(self):
    baseline_source = self
else if exists first Serviceable(source_i), i >= 1:
    baseline_source = source_i
else:
    baseline_source = none
```

其中 `Serviceable(source)` 表示该来源当前已经具备为该特征类型在线评分产出所需基线输出的能力。它强调“当前可服务”，而不是强绑定某一种内部模型状态。

补充说明：

- 从算法语义上，`Serviceable(source)` 只要求该来源当前能提供在线评分所需的基线期望、尺度或等价解释量
- 在分阶段工程实现中，若正式切换闭环尚未完成，可暂时允许“已训练且当前 bucket 可预测的 `candidate model`”作为可服务占位来源
- 待正式切换闭环完成后，来源选择的稳定优先级应收口为“正式模型优先于 `candidate model` 占位来源”

### 2.4 统一建模单元

统一建模对象定义为：

```
Series = (Key, Feature)
```

其中：

- `Key`：分析粒度键；
- `Feature`：该键上每分钟产出的某个特征值。

`Observation` 是输入事件，`Series(Key, Feature)` 是检测器视角下的时间序列对象。

### 2.5 Key 分层（解决高基数）

- L1：`Key = entity`（单实体视角）
- L2：`Key = entity × group`（如 client_subnet / ASN / region）
- L3：`Key = entity × peer_entity`（关系对）

说明：L3 不做全量，采用 Top-K 活跃关系 + 候选队列（按流量/连接数/最近活跃度筛选）。

---

## 3. 设计边界与统一原则

本设计统一的是框架，不是所有特征背后的统计模型。

统一部分：

- 同一套 `Observation -> Series -> Detector -> Fusion -> Output` 流水线
- 同一套 `Key / Feature / FeatureType` 抽象
- 同一套检测器接口与输出协议
- 同一套数学风险融合与输出结构

不统一部分：

- 不同特征的分布假设
- 残差或偏离量的定义
- 阈值的统计语义
- 冷启动、低样本量和持续性约束的细节

设计约束：

- 不能因为框架统一，就强行把所有特征压成同一种 `z-score`
- 不能简单按 `metric / relation` 二分；真正决定算法的是特征的数值性质
- 本方案只覆盖数学基线层，不覆盖业务判别层
- “是否是坏事”“是否值得告警”“告警优先级如何路由”等业务解释，明确不属于本设计范围
- 已由上游定义为不同 `feature` 的语义，不在基线层重复拆成运行时字段
- 同一算法族内的 feature 差异，优先通过 `feature` 级参数配置（profile）表达，而不是引入新的算法分支

---

## 4. 特征类型与归类规则

首版固定 3 类特征类型，检测器按类型选择，不再按 `metric / relation` 二分。

### 4.1 T1：数值时序类

典型特征：

- `bytes_total`
- `packet_total`
- `bps`
- `pps`
- `conn_count_total`
- `distinct_peer_count`
- `avg_rtt`
- `p95_rtt`
- `avg_http_response_time`
- `p95_http_response_time`
- `avg_tls_handshake_time`
- `p95_tls_handshake_time`
- `avg_dns_response_time`
- `p95_dns_response_time`
- `avg_flow_duration`
- `p95_flow_duration`
- `beacon_score`
- `scan_pattern_score`
- `sedanspot_edge_score`
- `anograph_window_score`

特点：

- 值域通常无界或半无界
- 常见趋势、周期、阶跃
- 适合做“预测值 vs 实际值”的残差检测

检测器职责：

- 学习趋势、季节性、节假日影响
- 输出预测残差、异常分和漂移失配证据

进一步细分：

- `T1a`：非负计数 / 流量类，如 `bytes_total`、`conn_count_total`、`distinct_peer_count`
- `T1b`：时延 / 连续值类，如 `avg_rtt`、`p95_rtt`

说明：

- `T1a` 与 `T1b` 都属于数值时序类，但因为值域、方差行为和异常语义不同，不应共用完全相同的建模细节
- 对于 `avg_rtt`、`p95_rtt`、`bytes_total`、`conn_count_total` 这类已由上游定义为独立 `feature` 的对象，基线层不再重复拆解其语义字段
- 同属 `T1a` 或 `T1b` 的不同 feature，默认共享同一检测主干；若差异仅体现在阈值、变换、最小样本门槛等，则通过各自的 `feature profile` 注入参数
- `T1a` 处理非负总量 / 次数 / 强度量；像 `bps`、`pps` 这类“固定时间窗口内总量再除以固定 `delta`”得到的强度指标，本质上仍属于 `T1a`
- `T1b` 处理“单样本连续属性在 bucket 内做聚合后的统计值”；典型聚合方式包括均值、中位数、分位数、截尾均值
- 若外部规则、图算法或专家模块输出的是单值分数时间序列，只要它需要做时间基线，就按 `T1b` 处理；来源不单独决定类型
- 判别规则可简化为：
  1. 若该特征本质上是总量、次数、基数或固定窗口强度量，则归 `T1a`
  2. 若该特征本质上是对单样本连续属性做窗口聚合后的统计值，则归 `T1b`
  3. 若该特征本质上是比例、占比、成功率、失败率等受限比值，则不归 `T1`，而归 `T2`

### 4.2 T2：比例 / 率类

典型特征：

- `error_rate`
- `syn_rate`
- `rst_rate`
- `new_peer_ratio`
- `new_group_share`
- `expert_hit_ratio`
- `abnormal_edge_ratio`

特点：

- 值域天然受限于 `[0,1]`
- 低样本量场景下波动容易虚高
- 同样的比例偏移，在不同样本量下意义不同

检测器职责：

- 联合解释“比例值 + 样本量”
- 抑制低分母下的伪异常
- 输出偏离程度和可信度
- 首版采用“平滑比例 + `logit` 季节基线 + 标准化偏差残差”的方案 B
- 在线热路径禁止精确二项尾概率、Beta-Binomial 尾概率和重型特殊函数计算
- 若外部模块输出的是命中占比、异常连接占比、风险占比等受限比值，则按 `T2` 处理，而不是因来源不同单独成类

### 4.3 T3：分布漂移类

典型特征：

- `client_group_mix`
- `peer_role_mix`
- `peer_asn_mix`
- `dst_port_mix`
- `expert_group_mix`
- `external_peer_mix`

特点：

- 建模对象是固定 `group_space` 上的一份质量分布或稀疏质量块，而不是普通单值点
- 异常语义是结构变化，不是单点高低
- `v1` 不直接在完整分布距离上做重型在线建模，而是先提取摘要特征，再分别路由到 `T1 / T2`

检测器职责：

- 消费当前窗口的分布块输入，维护稳定支持空间
- 提取关系结构变化的摘要特征
- 通过摘要特征与融合层表达结构性漂移强度、受影响 head / tail 和持续性
- 若外部模块直接输出单值差异量，如 `JSD / PSI / mix drift`，则该单值应按 `T1a` 处理，而不是按 `T3` 处理

---

### 4.4 首版归类规则与典型示例

首版统一归类规则如下：

- 任何“可预测的连续数值”归 `T1`
- 任何“带分母的比例 / 率”归 `T2`
- 任何“以 group 分布 / 稀疏质量块为输入、需要在基线层内部提取结构摘要”的对象归 `T3`
- 外部规则、图算法或模型输出若进入基线，按其数学形态归入 `T1 / T2 / T3`；来源不单独决定类型
- 若某外部证据不需要时间基线，只作为硬规则或强证据使用，则不进入 `T1 / T2 / T3`，而直接进入融合层
- 因此，外部预先算好的 `JSD / PSI / drift score` 若是单值时间序列，应按 `T1a` 处理；`T3` 保留给原始分布对象或等价结构输入

该规则优先保证方法学稳定，不在首版阶段为特例开口子。

典型示例如下。

`T1a`：非负计数 / 流量类

- `bytes_total`
- `conn_count_total`
- `distinct_peer_count`

`T1b`：时延 / 连续值类

- `avg_rtt`
- `p95_rtt`

`T2`：比例 / 率类

- `error_rate`
- `syn_rate` / `rst_rate`
- `new_peer_ratio`
- `new_group_share`（新网段 / 新 ASN 流量占比）

`T3`：分布漂移类

- `client_group_mix`
- `peer_role_mix`
- `peer_asn_mix`
- `dst_port_mix`

### 4.5 外部专家证据的归类方式

补充说明：

- 外部专家证据若落成单值分数序列，例如 `scan_pattern_score`、`beacon_score`、`sedanspot_edge_score`，应按 `T1b` 归类
- 外部专家证据若落成受限比值，例如 `expert_hit_ratio`、`abnormal_edge_ratio`，应按 `T2` 归类
- 外部专家证据若直接给出原始分组分布或等价稀疏质量块，可按 `T3` 归类；若只给出单值差异量，例如 `expert_group_mix_jsd`、`external_peer_distribution_psi`，则按 `T1a` 归类
- 也就是说，来源不决定类型；数学形态才决定类型

---

## 5. 统一接口与任务规格

### 5.1 边界原则

- 检测器负责回答：“这个特征现在是否异常，异常强度多大？”
- 融合层负责回答：“多个特征合起来风险多大？”
- 输出层负责回答：“如何产出统一的数学异常结果，供上层系统消费？”
- 输出层只提供数学异常强度、模式标签和解释证据，不提供业务处置建议

禁止让单个检测器直接承担业务判别职责，否则会把数学基线与业务解释混在一起。

### 5.2 统一输出协议

无论底层检测器采用什么算法，都统一输出：

```text
DetectorResult = {
  key,
  feature,
  feature_type,
  ts,
  raw_score,
  normalized_score,
  confidence,
  persistence,
  severity,
  reason_code,
  evidence
}
```

字段说明：

- `raw_score`：检测器原始分数，保留该算法的原生语义
- `normalized_score`：统一校准后的标准分，用于融合
- `confidence`：当前判断可信度，反映样本量、历史长度、模型稳定性
- `persistence`：连续异常窗口数
- `severity`：数学异常强度等级，如 `info / low / medium / high`，不等于业务告警等级
- `reason_code`：数学模式标签，如 `spike`、`drift`、`scan`、`rare_peer`，不直接代表业务结论
- `evidence`：解释性证据，如基线值、偏离量、Top-K 变化明细

### 5.3 基线任务、事件日历与历史数据读取接口

基线层在线职责是消费当前观测、维护轻量状态并输出异常结果；它不负责保存可重放历史。与工程侧的接口在 `v1` 中收口为 3 类任务级能力：任务规格、事件日历、历史数据读取。其中任务规格按特征类型分为 `T1/T2` 共用的 `BaselineTaskSpec`，以及 `T3` 专用的 `T3TaskSpec`。

`T1 / T2` 统一任务规格：

```text
BaselineTaskSpec = {
  key,
  feature,
  feature_type,
  feature_profile,
  delta,
  tz,
  baseline_source_configs?,
  event_calendar_spec?,
  history_reader?
}
```

说明：

- `feature_profile`：该特征所属 profile 或等价静态参数集
- `baseline_source_configs`：可选的基线来源配置集合，沿用第 `2.3` 节定义；其每个元素的语义归属是 `BaselineSourceConfig(key, feature)`，不是 task 级全局默认值
- `event_calendar_spec`：可选的事件日历静态规格，仅用于构造 `h_event(t)`
- `history_reader`：可选的历史数据读取接口，仅用于正式重建、回放验证等慢路径

补充约束：

- 对 `T1 / T2`，`BaselineTaskSpec` 描述的是一个 `feature` 的静态算法规格；运行时实际建模对象仍然是 `Series = (key, feature)`。
- 若一个 `ValueTask / RatioTask` 同时接收多个 `key` 的观测，`baseline_source_configs` 必须按运行时 `key` 匹配，未匹配的 `key` 视为 absent。
- 禁止把 `baseline_source_configs?` 解释为“该 task 下所有 `key` 共享同一来源配置”。

工程结构可表达为：

```text
baseline_source_configs? = [
  {
    key,
    baseline_sources: [ { source_key }, ... ]
  },
  ...
]
```

字段语义必须无歧义：

- `baseline_source_configs` 是 `T1 / T2` 任务规格中唯一允许出现的来源配置字段，字段名使用复数，因为它表达的是“按 `key` 索引的一组单序列来源配置”。
- `baseline_source_configs[i].key` 是该条配置生效的运行时 `key`；结合当前 task 的 `feature` 后，才构成完整建模单元 `(key, feature)`。
- `baseline_source_configs[i].baseline_sources` 是该 `(key, feature)` 的候选外部来源列表，不包含 `self`，`self` 由算法选择规则隐式置于最高优先级。
- `baseline_source_configs` 中不得出现重复 `key`；单条 `baseline_sources` 中不得出现重复 `source_key`，也不得把 `source_key` 配成同一个 `key`。
- 顶层单数 `baseline_source_config` 不允许作为 `T1 / T2` task 级字段使用，避免被误解为“task 下所有 `key` 共用同一来源配置”。
- 单数 `BaselineSourceConfig?` 只表示已经按 `(key, feature)` 解析后的单序列来源配置，可由 relation routed 特征或内部 resolver 物化使用。

`EventCalendarSpec` 摘要定义为：

```text
EventCalendarSpec = {
  calendar_id,
  calendar_version,
  entries
}

EventCalendarEntry = {
  event_code,
  scope_type,       // global | feature | key | key_feature
  alignment_mode,   // local_wall_clock | absolute_utc
  start_ts,
  end_ts,
  enabled,
  feature?,
  key?,
  tz?
}
```

`EventCalendarSpec` 约束：

- 事件日历不是运行时 `Observation` 字段，而是任务级静态注入能力
- `v1` 不在基线层内展开规则语言、cron 或 recurrence；上游必须先展开为显式时间区间
- 训练与预测必须使用相同的 `calendar_id + calendar_version`
- 若预测阶段未提供日历，或版本与模型记录不一致，则事件块必须禁用，模型退化为 `h_event(t) = 0`
- `scope_type` 与 `alignment_mode` 只负责限定事件作用域和时间对齐，不引入新的热路径状态

`HistoryReader` 摘要定义为：

```text
HistoryReader.fetch(
  key,
  feature,
  bucket_start,
  bucket_end
) -> HistoryFetchResult

HistoryFetchResult = {
  status,          // ok | insufficient_data | unavailable
  observations
}
```

`HistoryReader` 约束：

- `history_reader` 是任务级能力，不是每条运行时 `Observation` 的字段
- 在线热路径禁止调用 `history_reader`
- 返回的 `observations` 必须保持与在线输入一致的稀疏语义：不补空 bucket、不把 gap 填成 `0`
- 不同特征类型分别返回各自在线输入协议：`Observation_T1a`、`Observation_T1b`、`Observation_T2`、`T3Block`
- 对 `T1 / T2`，`fetch(key, feature, ...)` 中的 `feature` 指普通特征身份；对 `T3`，这里的 `feature` 视为任务引用，工程实现上应能唯一定位到对应的 `T3TaskSpec`（如直接使用 `task_id` 或等价注册引用）
- 若 `status != ok`，则本次正式重建直接放弃；基线层不得自行补齐历史

若任务未注入 `event_calendar_spec` 或 `history_reader`，基线仍可正常在线运行，只是分别禁用事件块或正式重建慢路径。

### 5.4 更新层次与语义

基线建立后不会停止学习，但不同层次的更新职责不同。为避免把所有“更新”都误解成一次新的全模型训练，本文统一区分以下 3 层：

1. 在线更新：每个有效 bucket 到来时都执行的轻量状态更新
2. 增量适配：在在线路径上持续修正少量轻量参数或临时基线
3. 正式重建：基于一段历史窗口，对正式基线模型做异步结构参数重建

其中：

- `在线更新` 作用于点异常分、持续性计数、漂移证据、覆盖率、ready / degraded 状态等轻量在线状态
- `增量适配` 作用于 `shadow baseline`、残差尺度、少量验证统计等可在 `O(1)` 状态下连续修正的部分
- `正式重建` 作用于趋势、季节、月位置等正式模型结构参数，依赖 `history_reader` 或等价历史数据能力

设计约束：

- 不允许把所有正式模型参数都放到热路径上逐点重估
- 不允许把“基线持续更新”误解成“每个 bucket 都做一次全模型重建”
- 当文中出现 `train / gate_train / training objective` 等术语时，默认指正式模型训练样本准入与正式重建目标，而不是在线热路径逐点训练
- 当文中出现“切换基线”时，若无特殊说明，优先指在线临时基线或正式模型版本切换，而不是停止在线更新

---

## 6. T1：数值时序类统一设计（T1a / T1b）

### 6.1 T1 公共原则

`T1a` 与 `T1b` 统一复用以下主干：

```text
Observation
-> Transform
-> Baseline Model
-> Reliability Adapter
-> Residual Normalization
-> Point Score
-> Drift Evidence Accumulator (BOCPD-style)
-> DetectorResult
```

统一部分：

- 都采用上游聚合产出的稀疏记录流，不在基线层内部逐 bucket 展开空记录
- 都使用相同的时间索引、gap 处理、趋势 / 季节 / 月位置 / 事件项框架
- 都通过统一的可靠性适配接口控制训练、评分、漂移更新与置信度
- 都采用“点异常分 + 漂移分”的有界软融合
- 都输出统一 `DetectorResult` 协议

差异落点：

- `T1a` 主要处理非负总量 / 次数 / 基数，默认关注方差稳定化与相对偏离
- `T1b` 主要处理时延 / 连续值，额外处理样本量变化带来的统计稳定性差异

统一约束：

- 已编码进 `feature` 身份的语义，不在 `T1` 内重复拆成运行时字段
- `T1` 内部默认先判断“是否同一算法不同参数”，能通过 `feature profile` 表达的差异，不上升为新的算法分支
- `feature profile` 可覆盖 `transform`、阈值、最小样本门槛、样本量修正强度等参数，但不改变主干接口

对任意 `T1` feature，在 bucket `t` 上统一定义可靠性适配接口：

```text
gate_train(t) ∈ {0,1}
gate_score(t) ∈ {0,1}
gate_shift(t) ∈ {0,1}
rho_t >= 1
```

其中：

- `gate_train(t)`：是否允许该点进入正式模型训练 / 正式重建
- `gate_score(t)`：是否允许该点输出正式点异常分
- `gate_shift(t)`：是否允许该点更新漂移检测状态
- `rho_t`：当前点的有效不确定性放大系数

`T1` 统一通过以下方式使用该接口：

```text
sigma_eff,t = sigma * rho_t
w_train(t) = 1 / rho_t^2
confidence = confidence_base / rho_t
```

其中：

- `sigma_eff,t`：在线残差标准化所用的有效尺度
- `w_train(t)`：训练样本权重，仅在 `gate_train(t) = 1` 时生效
- `confidence_base`：不考虑当前点统计支撑度时，由历史长度、覆盖率、模型状态给出的基准置信度

两类默认实例：

- `T1a`：对所有到达基线的记录，`gate_train = gate_score = gate_shift = 1`，`rho_t = 1`
- `T1b`：`gate_*` 由 `sample_count` 联合决定，`rho_t` 由样本量修正函数给出

### 6.2 T1a 适用范围

`T1a` 只处理以下特征：

- 非负聚合量
- 以“总量 / 次数 / 基数”为主
- 典型如 `bytes_total`、`conn_count_total`、`packet_total`、`distinct_peer_count`

`T1a` 明确不处理：

- RTT / 时延类连续值
- 百分位时延，如 `p95_rtt`
- 比例 / 率
- 分布漂移量
- 专家评分

### 6.3 T1a 输入规格

对每个 `(Key, Feature)`，输入不是基线层内部逐分钟展开的稠密时间序列，而是上游聚合层产出的稀疏统计记录流：

```text
Observation_T1a = {
  key,
  feature,
  bucket_id,
  value
}

FeatureSpec_T1a = {
  delta,
  tz
}
```

字段约束：

- `value = y_t >= 0`
- `bucket_id` 对应上游固定统计窗口的绝对编号
- `delta` 是固定统计窗口宽度，如 `1 min`、`5 min`、`10 min`
- `tz` 必须固定，不允许同一序列跨时区建模
- `delta`、`tz` 属于静态特征规格，不在热路径上随每条记录重复传输
- `bytes_total`、`conn_count_total`、`distinct_peer_count` 等口径差异直接编码进 `feature` 身份，不再额外拆成 `agg_type` 一类算法字段
- 记录一旦进入基线，默认即为可用记录；不可用 bucket 由上游直接不报，基线侧按 gap 处理

时间索引统一使用绝对 bucket 编号：

```text
P_day = 24h / delta
P_week = 7 * P_day
```

为避免 `DST` 歧义，时间字段按以下口径解释：

- `bucket_id`：统一对应 `UTC` 下的绝对窗口编号
- `t_abs = bucket_id`：趋势项使用的绝对时间索引
- `phase_day_local(t)`：由 `bucket_id`、`delta`、`tz` 映射得到的本地日内相位，取值归一化到 `[0, 1)`
- `phase_week_local(t)`：由 `bucket_id`、`delta`、`tz` 映射得到的本地周内相位，取值归一化到 `[0, 1)`
- `dom(t)`、`days_to_month_end(t)`、`is_last_weekday_of_month(t, w)` 等月位置函数，全部基于本地日历字段计算

如需恢复窗口起始时间，可由 `bucket_id`、`delta`、`tz` 反解得到 `ts`，供离线展示和结果输出使用。

缺失语义必须严格区分：

- 真 0：该 `bucket_id` 有记录，且 `value = 0`
- gap：该 `bucket_id` 没有任何记录到达，不等于 0

处理规则：

- 真 0 正常参与训练和评分
- gap 不插值、不补 0，由 `bucket_id` 跳变惰性处理
- 基线层不为所有潜在序列逐分钟补空桶，也不在内部重做窗口聚合

训练数据要求：

- 有效 bucket 数达到 `7 * P_day`，才能启用日周期
- 有效 bucket 数达到 `4 * P_week`，才能启用周周期
- 覆盖至少 `3` 个自然月，才能稳定学习月位置特征
- `month_cov_min` 只用于月位置特征组的启用判断，不再直接否决 `Core` 模型的可用性
- 有效覆盖率偏低会降低 `confidence` 并附带覆盖率降级标记，但只要 `Core` 所需样本条件满足，序列仍可进入 `core_no_month_ready`

其中覆盖率定义为：

```text
coverage
= valid_bucket_count
  / (bucket_id_max - bucket_id_min + 1)
```

### 6.4 T1a 数值变换

`T1a` 的输入变换定义为可替换组件：

```text
x_t = Transform(y_t; φ)
```

`v1` 默认采用：

```text
x_t = log(1 + y_t)
```

默认选择 `log1p` 的原因：

- 把乘性波动近似转为加性偏差
- 压缩重尾大值
- 缓解“均值越大、方差越大”的异方差问题
- 保留 `0` 值可处理性

设计约束：

- `Transform` 必须保持单调递增
- `Transform(0)` 必须有定义
- `Transform` 应优先服务于方差稳定化，而不是追求分布完全正态

后续若实测发现 `log1p` 对某些子特征不够稳，可替换为其他单调变换，但不改变 `T1a` 其余训练和在线评分框架。

变换后的残差定义为：

```text
r_t = x_t - μ_t
```

该残差近似对应原始域中的“相对偏离”，比绝对偏离更适合 `bytes_total`、`conn_count_total` 等流量型特征。

### 6.5 T1a 模型结构

在变换域中建模：

```text
x_t = μ_t + ε_t
```

统一超模型定义为：

```text
μ_t = μ_core(t) + h_monthpos(t) + h_event(t)

μ_core(t)
= g_trend(t_abs)
 + s_day(t)
 + s_week(t)
```

组件语义：

- `Core`：趋势、日周期、周周期主干
- `core_no_month`：只启用 `Core` 的正式可用形态；它是 `v1` 的正式回退模型，不是临时技巧
- `monthpos`：在 `Core` 之上补充稳定重复的月位置增量
- `event`：在 `Core + monthpos` 之上补充已标注的外生事件增量

趋势项 `v1` 固定为简单线性趋势：

```text
g_trend(t_abs) = β0 + k * t_abs
```

约束：

- `v1` 的正式模型不内建 `changepoint`
- 结构变化优先通过“漂移确认 -> 估计 `τ_hat` -> 裁剪历史重建”处理
- 对低覆盖或稀疏序列，`v1` 允许先产出 `core_no_month`，而不是因为 `monthpos` 支撑不足长期停留在仅观察状态

季节项固定启用日周期和周周期：

```text
s_day(t)
= Σ_{m=1}^{K_day}
  [a_m sin(2πm * phase_day_local(t))
 + b_m cos(2πm * phase_day_local(t))]

s_week(t)
= Σ_{m=1}^{K_week}
  [c_m sin(2πm * phase_week_local(t))
 + d_m cos(2πm * phase_week_local(t))]
```

说明：

- `phase_day_local(t)`、`phase_week_local(t)` 都由 `bucket_id`、`delta`、`tz` 映射得到，而不是直接把 `UTC bucket_id` 代入周期项
- 在 `DST` 切换日，本地时钟可出现 `23 h / 25 h` 的自然日；`v1` 接受这种真实时钟不等距，不额外做人工补偿

月位置层定义为：

```text
h_monthpos(t)
= a_dom[dom(t)]
 + b_dme[min(days_to_month_end(t), DME_max)]
 + Σ_w c_w * I(is_last_weekday_of_month(t, w))
```

首版实现仅保留 3 组特征：

- `day_of_month`：`1..31` 的 one-hot
- `days_to_month_end`：`0..DME_max` 的 one-hot
- `last_weekday_of_month`：7 个二值 one-hot

说明：

- `day_of_month` 表达“每月几号”效应
- `days_to_month_end` 表达“接近月末”效应
- `last_weekday_of_month` 表达“最后一个周五 / 周六”等自然月规则
- 该层负责自动学习稳定重复的月规则，不预先写死具体业务规则
- `v1` 将 `last_business_day_of_month` 移出活动设计；若未来接入可靠工作日日历，再作为增强项单独评估

`monthpos` 的建模边界：

- 稳定、重复、可从历史中归纳的月规则，优先由 `h_monthpos(t)` 吸收
- 非稳定、低频、一次性或外部注入的模式，交由 `h_event(t)` 处理
- `monthpos` 在训练上通过 staged fit 直接在 `r_core` 上学习增量；`v1` 不再额外要求显式 `X_monthpos^⊥` 投影或正交化算子
- 训练时仍应对 `day_of_month`、`days_to_month_end`、`last_weekday_of_month` 做去中心化，以降低与截距的耦合

`monthpos` 启用规则收口为单一成熟度门槛：

- `M_month_enable`：月位置层启用所需的最小自然月数
- `month_cov_min`：月位置层启用所需的最小有效覆盖率
- 当 `monthpos` 未达到启用条件时，接口保留，但该块不进入训练与预测，等价于 `h_monthpos(t) = 0`
- 已达到总体启用条件但样本明显稀疏的列，可在实现上跳过或仅依赖正则收缩，不再单独引入新的对外门槛参数

`h_event(t)` 定义为基于任务级 `EventCalendarSpec` 的外生事件层：

```text
h_event(t) = Σ_e β_e * I(t ∈ W_e)
```

边界与约束：

- `h_event(t)` 只解释“已知、可标注、可复用”的外生特殊规律
- `h_event(t)` 不吸收无标签随机事件；未知扰动必须保留在残差中，由异常检测层处理
- `v1` 中，事件层唯一正式输入来源是任务级 `EventCalendarSpec`
- 同一 `event_code` 在 `v1` 中合并成 1 列二值 indicator，命中规则采用“区间有交即命中”
- 训练与预测必须使用同一 `calendar_id + calendar_version`；若版本不一致，则事件块禁用
- `v1` 不引入“节假日专属日周期”，只建模事件整体偏移

模型状态：

- `cold_start`：`Core` 最小样本条件都不满足；只能观察，或临时借用基线来源
- `core_no_month_ready`：`Core` 可用，但 `monthpos` 未启用；这是 `v1` 的正式可用主状态
- `full_ready`：`Core` 可用，且 `monthpos` 已启用

补充说明：

- `event` 不单独决定主模型状态，只在 `core_no_month_ready` 或 `full_ready` 上独立启停
- 覆盖率不足以支持 `monthpos` 时，不应把序列重新打回 `cold_start`
- `v1` 不引入 `monthpos × weekday`、`monthpos × hour_bucket` 交互项，也不对 `day_of_month` 额外引入 spline 或 fused lasso

### 6.6 T1a 训练流程与目标

令 `Ω_train` 为所有有记录到达的有效 bucket 集合。

gap 不进入训练损失；不可用 bucket 由上游直接过滤，因此在基线层等价于 gap。

在 `T1a` 中，可靠性适配退化为：

```text
gate_train(t) = 1
gate_score(t) = 1
gate_shift(t) = 1
rho_t = 1
w_train(t) = 1
```

`T1a` 的正式模型训练固定采用“统一超模型 + 层级分阶段拟合”：

```text
Step 1 (Core):
min Σ_{t∈Ω_train} Huber(x_t - μ_core(t))
  + λ_season * ||θ_s||_2^2

Step 2 (monthpos):
r_core,t = x_t - μ_core(t)
min Σ_{t∈Ω_train} Huber(r_core,t - h_monthpos(t))
  + λ_dom * ||θ_dom||_2^2
  + λ_dme * ||θ_dme||_2^2
  + λ_lwd * ||θ_lwd||_2^2

Step 3 (event):
r_enh,t = x_t - μ_core(t) - h_monthpos(t)
min Σ_{t∈Ω_train} Huber(r_enh,t - h_event(t))
  + λ_event * ||θ_e||_2^2
```

说明：

- `Core` 先解释主干结构，`monthpos` 只解释 `Core` 残差中的稳定月位置增量，`event` 只解释已标注的外生残差
- `v1` 活动设计不再包含 `joint_recalibration`
- 分阶段拟合与一次性整体联训在数值上不保证完全相同；本设计优先保证解释层级、稳定性和可实现性
- 最终预测统一使用 `μ_t = μ_core(t) + h_monthpos(t) + h_event(t)`

训练输出至少包括：

```text
Model_T1a = {
  beta0,
  k,
  transform_name,
  solver_name,
  fit_strategy,       // staged
  fit_summary,        // per-block FitBlockResult digest
  active_components,  // enabled monthpos groups + enabled event types
  event_calendar_id,
  event_calendar_version,
  active_event_codes,
  season_day_coeff,
  season_week_coeff,
  monthpos_dom_coeff,
  monthpos_dme_coeff,
  monthpos_last_weekday_coeff,
  event_coeff,
  sigma_ref,
  train_start,
  train_end,
  delta,
  tz
}
```

残差尺度估计采用鲁棒 MAD：

```text
r_t = x_t - μ_t
sigma = max(sigma_min, 1.4826 * median(|r_t - median(r)|))
```

#### 6.6.1 统一块求解器契约

`T1a / T1b / T2` 的离线训练与正式重建统一复用同一类块求解器：

```text
WeightedHuberRidgeBlockSolver
```

该求解器只运行在离线训练 / 正式重建慢路径，不进入在线评分热路径。主契约收口为：

```text
BlockFitSpec = {
  block_name,       // core | monthpos | event
  y_target,
  X,
  sample_weight,
  ridge_diag,
  init_beta?,
  col_roles
}

BlockSolverConfig = {
  solver_name       // v1 固定为 weighted_huber_ridge_irls
}
```

统一约束：

- `y_target` 与 `X` 的行必须按同一 `Ω_train` 顺序对齐
- `sample_weight` 是各类型唯一需要覆盖的训练权重入口
- `ridge_diag` 与 `X` 列一一对应，未正则列取 `0`
- `v1` 求解器必须确定性执行，不允许随机初始化或采样类算法
- 数值保护常量和收敛阈值属于实现常量，不再作为主设计参数对外展开

#### 6.6.2 `IRLS + weighted ridge` 求解流程

`v1` 的统一块求解器固定采用：

```text
weighted_huber_ridge_irls
```

默认流程收口为：

1. 先解一次 weighted ridge，得到初始化系数  
2. 基于初始残差估计 `Huber` 截断尺度  
3. 进入 `IRLS` 外循环，交替更新鲁棒权重与 weighted ridge 子问题  
4. 满足收敛条件则接受；若最后一轮有限但未完全收敛，可接受为 `degraded`

实现建议：

- 线性代数路径首选 `Cholesky`，失败时回退到 `QR`
- `v1` 不引入 `SGD`、`EM`、采样类算法或通用黑盒优化器
- `core` 在列结构一致时可使用上一版系数 warm start；`monthpos`、`event` 默认从 `0` 开始

#### 6.6.3 收敛、稳定性与回退契约

统一约束：

- 所有接受的 `coef`、目标函数和条件数估计都必须为有限值
- 同一输入、同一配置下，求解器输出必须可复现

块级回退：

- `core`：先回退到不带 `Huber` 的 weighted ridge；若仍失败，则依次退化到 `intercept + day/week`、`intercept-only`
- `monthpos`：若求解失败或数值不稳，则整块置零，`status = degraded`
- `event`：若求解失败或数值不稳，则整块置零，`status = degraded`

模型级接受规则：

- `core` 必须产出可用结果，正式模型才可进入候选态
- `monthpos`、`event` 可独立 `skipped / degraded`，不阻塞整体正式模型生成
- 当 `core = ok` 且 `monthpos = skipped | degraded` 时，模型状态标记为 `core_no_month_ready`
- 当 `core = ok` 且 `monthpos` 生效时，模型状态标记为 `full_ready`

#### 6.6.4 `T1a / T1b / T2` 到统一求解器的映射

`core` 块：

```text
y_target = x_t
X_core = [intercept, trend, season_day, season_week]
ridge_diag = [0, 0, λ_season, ..., λ_season]
```

`monthpos` 块：

```text
y_target = r_core,t
X_month = X_monthpos
ridge_diag = [λ_dom / λ_dme / λ_lwd by column role]
```

`event` 块：

```text
y_target = r_enh,t
X_event = event indicator columns derived from EventCalendarSpec
ridge_diag = [λ_event, ..., λ_event]
```

不同特征类型只在 `sample_weight` 和 `y_target` 所在变换域上区分：

- `T1a`

```text
ω_t = 1
```

- `T1b`

```text
ω_t = 1 / rho_t^2
```

- `T2`

```text
ω_t = w_train(t)
```

补充说明：

- `T1a / T1b` 的 `y_target` 位于单调变换域（如 `log1p`）
- `T2` 的 `y_target` 位于 `logit` 变换域
- `monthpos`、`event` 的块求解器契约完全一致，只是设计矩阵、正则向量和基础权重不同
- `T1b / T2` 不再单独定义另一套求解器，只在块映射和权重函数上覆盖 `T1a`

### 6.7 T1a 在线评分

在 `T1a` 中，在线评分准入与可靠性适配退化为：

```text
gate_score(t) = 1
rho_t = 1
```

当 `gate_score(t) = 1` 时执行：

```text
x_t = Transform(y_t; φ_model)
μ_t = predict(Model_T1a, t)
r_t = x_t - μ_t
sigma_eff,t = sigma
z_t = r_t / sigma_eff,t
```

其中：

- `φ_model` 由训练输出中的 `transform_name` 及其参数确定
- `z_t` 是鲁棒标准化残差
- `t` 由当前记录的 `bucket_id` 映射得到

`T1a` 默认采用双侧检测：

- `z_t > 0`：高于基线，可能是 `spike`
- `z_t < 0`：低于基线，可能是 `drop`

点异常强度定义为：

```text
score_point = clip((|z_t| - z_warn) / (z_crit - z_warn), 0, 1)
```

持续性计数规则：

```text
if |z_t| >= z_warn:
    persistence += 1
else:
    persistence = 0
```

gap 的处理规则：

- 不计算 `r_t`
- 不输出点异常分
- 不更新在线残差统计
- `persistence` 默认冻结，不累加也不清零
- 缺失 bucket 数通过相邻记录的 `bucket_id` 差值惰性计算，不逐桶展开

### 6.8 T1a 漂移证据累积器（BOCPD-style）接法

`漂移证据累积器（BOCPD-style）` 在 `T1a` 中的角色是“旧基线失配的在线证据累积器”，不承担单点尖刺检测。这里保留 `BOCPD-style`，仅用于标注方法来源是“持续累积漂移证据”的思路；`v1` 默认实现不是完整 `BOCPD` 检测，不维护 `hazard function`、`run-length posterior` 等完整后验状态，而是固定状态、固定算子的轻量实现。

输入不是原始 `y_t`，而是标准化后并经过平滑的残差信号：

```text
z_t = r_t / sigma
e_t = clip(z_t, -shift_clip, shift_clip)
u_t = α * e_t + (1 - α) * u_{t-1}
```

其中：

- `clip()`：抑制单点尖刺对漂移证据的污染
- `shift_clip`：漂移证据输入截断上限
- `α`：EWMA 平滑系数，建议初值 `0.2`
- `u_t`：用于检测持续偏移的平滑残差证据

每个序列只维护固定状态：

```text
ShiftState = {
  u_t,
  s_up_t,
  s_down_t,
  c_t,
  dir_t
}
```

其中：

- `s_up_t`：向上漂移证据累积量
- `s_down_t`：向下漂移证据累积量
- `c_t`：当前方向的连续确认计数
- `dir_t ∈ { up, down, none }`：当前主导漂移方向

在 `T1a` 中，漂移更新准入退化为：

```text
gate_shift(t) = 1
```

每个满足 `gate_shift(t) = 1` 的记录到来时，固定状态按以下方式更新：

```text
s_up_t
= max(0, λ_mem * s_up_{t-1} + u_t - κ_shift)

s_down_t
= max(0, λ_mem * s_down_{t-1} - u_t - κ_shift)
```

方向与连续确认计数定义为：

```text
if s_up_t > s_down_t:
    dir_t = up
else if s_down_t > s_up_t:
    dir_t = down
else:
    dir_t = none

if dir_t == up and u_t >= u_min:
    c_t += 1
else if dir_t == down and u_t <= -u_min:
    c_t += 1
else:
    c_t = max(0, c_t - 1)
```

其中：

- `λ_mem`：漂移证据记忆衰减系数
- `κ_shift`：漂移死区，过滤轻微偏差
- `u_min`：进入连续确认计数的最小平滑偏移

漂移置信度定义为：

```text
p_shift_t
= clip(max(s_up_t, s_down_t) / H_shift, 0, 1)
```

其中 `H_shift` 是漂移证据饱和阈值。

对稀疏记录流，若上一个已处理 bucket 为 `b_prev`，当前 bucket 为 `b_cur`，则连续 gap 数定义为：

```text
g = b_cur - b_prev - 1
```

gap 下的在线更新规则：

- 当连续缺失窗口数 `g <= G_skip` 时：漂移证据累积器冻结，不更新
- 当 `g > G_reset` 时：`ShiftState` 直接重置
- 当 `G_skip < g <= G_reset` 时：`v1` 仍按冻结处理，不引入复杂 gap-aware 状态转移

漂移分数定义为：

```text
score_shift
= clip((p_shift_t - p_shift_low) / (p_shift_high - p_shift_low), 0, 1)
```

最终统一分数定义为：

```text
normalized_score = 1 - (1 - score_point) * (1 - w_shift * score_shift)
```

其中 `w_shift` 是漂移分数权重，建议初值 `0.8`。该公式属于有界软融合：

- 当点异常和漂移异常都弱时，分数保持低位
- 当任一项很强时，结果快速接近强异常
- 当两项都处于中等水平时，能够体现叠加效应
- 分数天然限制在 `[0,1]`，避免简单相加带来的炸分

若 `score_shift` 较高，则方向优先由 `dir_t` 给出。

对应原因标签：

- `baseline_shift_up`
- `baseline_shift_down`

### 6.9 T1a 阈值语义

点异常阈值：

- `z_warn`：单窗口异常起点，建议初值 `3`
- `z_crit`：单窗口强异常阈值，建议初值 `5`
- `shift_clip`：漂移证据输入的截断上限，建议初值 `6`

漂移阈值：

- `λ_mem`：漂移证据记忆衰减系数，建议初值 `0.9`
- `κ_shift`：漂移死区，建议初值 `0.25`
- `u_min`：连续确认最小平滑偏移，建议初值 `0.5`
- `H_shift`：漂移证据饱和阈值，建议初值 `3.0`
- `p_shift_low`：进入疑似漂移区间，建议初值 `0.3`
- `p_shift_high`：高置信度漂移阈值，建议初值 `0.6`
- `M_shift`：低阈值持续窗口数，建议初值 `3`

缺失阈值：

- `G_skip`：允许冻结状态的最大连续缺失窗口数，建议初值 `3`
- `G_reset`：超过该值直接重置漂移证据状态，建议初值 `12`

稳定性阈值：

- `sigma_min`：残差尺度下限，用于防止极平稳序列产生虚高 `z-score`

### 6.10 T1a 输出语义与正式重建触发

`T1a` 的输出协议沿用第 `5.2` 节统一 `DetectorResult`。相对统一协议，`T1a` 的关键语义为：

- `raw_score`：建议定义为 `|z_t|`
- `normalized_score`：取 `1 - (1 - score_point) * (1 - w_shift * score_shift)`
- `confidence`：综合历史长度、训练覆盖率、模型状态
- `reason_code`：按优先级输出 `baseline_shift_up`、`baseline_shift_down`、`spike`、`drop`
- `evidence`：至少包含 `y_t`、`x_t`、`baseline_mu_t`、`resid_r_t`、`z_t`、`p_shift_t`、`dir_t`、`score_point`、`score_shift`

异步正式重建候选建议在满足以下任一条件时触发：

- `p_shift_t >= p_shift_high`
- `p_shift_t >= p_shift_low` 且连续 `M_shift` 个窗口成立
- 残差均值在最近短窗内持续同向偏移，且点异常频繁出现

以上条件只负责产生“正式重建候选”；统一慢路径、`shadow baseline`、`history_reader` 约束与切换规则见第 `10.2` 节。

### 6.11 T1b：时延 / 连续值类差异规格（v1）

本节只定义相对 `T1a` 的差异项。未显式覆盖之处，默认沿用 `T1` 公共主干与 `T1a` 中相同的稀疏输入、gap 处理、趋势 / 季节 / 月位置框架、漂移证据累积器（BOCPD-style）融合方式和统一输出协议。

#### 6.11.1 适用范围

`T1b` 处理以下特征：

- 时延 / 延时类连续统计值
- 典型如 `avg_rtt`、`p95_rtt`、`avg_latency`

`T1b` 的设计约束：

- `avg_rtt`、`p95_rtt` 等语义由上游定义为独立 `feature`
- `T1b` 不再额外拆出 `stat_type`、`quantile_q`、`sample_type` 等重复语义字段
- 不同 feature 的差异，优先通过各自 `feature profile` 参数化表达

#### 6.11.2 输入差异

`T1b` 相对 `T1a` 唯一新增的运行时字段是 `sample_count`：

```text
Observation_T1b = {
  key,
  feature,
  bucket_id,
  value,
  sample_count
}

FeatureSpec_T1b = {
  delta,
  tz
}
```

字段约束：

- `value = y_t >= 0`
- `sample_count = n_t >= 1` 表示当前 `bucket_id` 内参与 `value` 计算的原始样本数
- `sample_count` 必须与 `value` 来自同一批原始样本
- 无样本窗口默认不上报记录，直接形成 gap
- 记录一旦进入基线，默认即为可用记录；不可用 bucket 由上游直接不报，基线侧按 gap 处理

`sample_count` 的职责边界：

- 不参与 `feature` 身份定义
- 不承载业务语义
- 只用于控制训练、评分、漂移更新与置信度

#### 6.11.3 `feature profile` 差异参数

`T1b` 的差异优先通过 `feature profile` 注入：

```text
FeatureProfile_T1b(feature) = {
  profile_name,
  n_train_min,
  transform_name_override?
}
```

说明：

- `avg_rtt` 与 `p95_rtt` 默认共享 `T1b` 主干
- 差异主要体现在样本量支撑度；点异常阈值和漂移阈值默认沿用 `T1a` 的共享取值
- `transform_name_override` 只在个别连续值特征需要偏离默认变换时才使用，不作为首版 profile 主参数
- `T1b` 的样本量门槛在 `v1` 中收口为 1 个主参数 `n_train_min`
- 其余门槛和置信度修正强度统一由派生规则给出：

```text
n_score_min(feature) = ceil(0.5 * n_train_min(feature))
n_shift_min(feature) = 2 * n_train_min(feature)
kappa_sample(feature) = n_train_min(feature)
```

#### 6.11.4 数值变换差异

`T1b` 的输入变换仍定义为可替换组件：

```text
x_t = Transform(y_t; φ_feature)
```

默认规则：

- `T1b` 默认也采用 `log1p`
- 若某个连续值特征存在更合适的单调方差稳定化变换，可通过 `transform_name_override` 覆盖
- `Transform` 必须保持单调递增，并优先服务于方差稳定化，而不是强求正态化

#### 6.11.5 训练差异

`T1b` 的可靠性适配定义为：

```text
gate_score(t) = I(n_t >= n_score_min(feature))
gate_train(t) = I(n_t >= n_train_min(feature))
gate_shift(t) = I(n_t >= n_shift_min(feature))
rho_t = sqrt(1 + kappa_sample(feature) / n_t)
```

其中：

- `rho_t >= 1`
- `n_t` 越小，`rho_t` 越大
- `kappa_sample(feature)` 控制该 feature 对低样本的敏感程度

`T1b` 的训练集合定义为：

```text
Ω_train = { t | gate_train(t) = 1 }
```

训练流程相对 `T1a` 的差异为：

```text
min
Σ_{t∈Ω_train} (1 / rho_t^2) * Huber(resid_prev,t - component_t)
+ step_regularization
```

说明：

- `T1b` 仍采用与 `T1a` 相同的统一超模型和层级分阶段拟合流程
- `T1b` 的块求解器接口、`Huber` 定义、`IRLS + weighted ridge` 流程，以及收敛 / 回退契约，统一沿用 `6.6.1 ~ 6.6.4`
- 样本数低于 `n_train_min(feature)` 的点不进入任何离线拟合步骤
- 刚过门槛的点可参与训练，但权重较低
- `n_t` 充分大时，`rho_t -> 1`，训练权重 `1 / rho_t^2 -> 1`
- 对 `Core`、`monthpos`、`event` 的每一个激活组件拟合步骤，都统一乘以 `1 / rho_t^2`
- 除样本量门槛与权重外，`μ_t` 的结构和 `T1a` 保持一致
- 训练输出记为 `Model_T1b`，其趋势 / 季节 / 月位置 / 事件主体结构沿用 `Model_T1a`

#### 6.11.6 在线评分差异

当 `gate_score(t) = 1` 时：

```text
x_t = Transform(y_t; φ_feature)
μ_t = predict(Model_T1b, t)
r_t = x_t - μ_t
sigma_eff,t = sigma * rho_t
z_t = r_t / sigma_eff,t
score_point = clip((|z_t| - z_warn) / (z_crit - z_warn), 0, 1)
```

其中：

- `sigma` 是离线训练得到的残差基准尺度
- `sigma_eff,t` 是按样本量修正后的有效尺度
- `sigma_eff,t = sigma * rho_t`
- `n_t` 越小，`rho_t` 越大，单点异常越不容易被放大

当 `gate_score(t) = 0` 时：

- 不产生正式 `score_point`
- `persistence` 冻结，不累加也不清零
- 可保留内部抑制原因 `insufficient_sample`

#### 6.11.7 漂移证据累积器（BOCPD-style）差异

当 `gate_shift(t) = 1` 时，沿用 `T1a` 的平滑残差输入方式，但使用样本量修正后的 `z_t`：

```text
e_t = clip(z_t, -shift_clip, shift_clip)
u_t = α * e_t + (1 - α) * u_{t-1}
```

当 `gate_shift(t) = 0` 时：

- 漂移证据状态冻结
- 不更新 `ShiftState`

含义：

- `T1b` 允许样本量刚够评分的点输出点异常结果
- 但只有样本量进一步达到 `n_shift_min(feature)` 后，才允许其驱动“基线正在漂移”的判断

#### 6.11.8 置信度与输出差异

`T1b` 的样本量置信度修正统一通过 `rho_t` 表达：

```text
confidence = confidence_base / rho_t
```

其中 `confidence_base` 沿用 `T1a` 中由历史长度、覆盖率、模型状态得到的基准置信度。

`T1b` 的 `evidence` 相对 `T1a` 至少新增：

- `sample_count`
- `sigma_eff_t`

以保证下游能够解释“为何同样的残差，在不同样本数下得到不同异常强度”。

---

## 7. T2：比例 / 率类统一设计（方案 B）

### 7.1 统一主干与差异边界

`T2` 与 `T1` 共享统一检测主干：

```text
Observation
  -> Transform
  -> Baseline Model
  -> Reliability Adapter
  -> Residual Normalization
  -> Point Score
  -> Drift Evidence Accumulator (BOCPD-style)
  -> DetectorResult
```

与 `T1` 的差异只落在以下部分：

- 观测输入不是单个 `value`，而是 `numerator + denominator`
- 值域受限于 `[0,1]`，需先做边界平滑，再进入季节基线
- 在线残差不能只看比例值本身，必须把 `denominator` 内生到方差解释中

### 7.2 输入规格

`T2` 运行时输入定义为：

```text
Observation_T2 = {
  key,
  feature,
  bucket_id,
  numerator,
  denominator
}
```

约束：

- `ratio` 可由上游附带传输用于观测，但不是基线权威输入
- `denominator <= 0` 的 bucket 不进入基线，等价为 gap
- bucket 一旦进入基线，即默认可用；不可用 bucket 由上游直接过滤
- `feature` 身份承载业务口径，例如 `error_rate`、`new_peer_ratio`、`success_rate`
- 若上游为该 `(key, feature)` 显式配置了 `BaselineSourceConfig`，则 `T2` 冷启动阶段可按第 `2.3` 节统一机制选择可用基线来源；该配置不是运行时输入字段

### 7.3 `feature profile` 与默认分组

`T2` 的差异优先通过 `feature profile` 注入：

```text
FeatureProfile_T2(feature) = {
  profile_name,
  s_prior,
  d_min_train,
  phi_over
}
```

`T2 v1` 的参数收口原则：

```text
d_score_min(feature) = ceil(0.5 * d_min_train(feature))
d_shift_min(feature) = 2 * d_min_train(feature)
kappa_den(feature) = d_min_train(feature)
```

说明：

- `T2` 首版不按“业务好坏方向”拆分算法，只按统计稳定性分组
- 接近 `0` 还是接近 `1` 的边界差异，通过 `m0(feature)`、`alpha0(feature)`、`beta0(feature)` 自动吸收
- `eps_logit`、`v_floor` 属于全局共享实现常量，不再作为 profile 参数展开
- `z_warn`、`z_crit`、`shift_clip` 在 `T2 v1` 中作为共享阈值使用，不再按 profile 细分
- `d_min_train` 是 `T2` 唯一对外暴露的分母支撑度主参数；评分门槛、漂移门槛和置信度修正强度均由它派生

自动生成规则：

```text
Ω_train = { t | gate_train(t) = 1 }

m0(feature)
= clip(
    Σ_{t∈Ω_train} numerator_t
    / Σ_{t∈Ω_train} denominator_t,
    m_floor,
    1 - m_floor
  )

alpha0(feature) = s_prior(feature) * m0(feature)
beta0(feature)  = s_prior(feature) * (1 - m0(feature))
```

其中建议：

- `m_floor = 1e-4`
- `s_prior(feature)` 由所属 profile 决定，不由每个 feature 单独手工指定
- `m0(feature)` 是当前训练窗口上的静态先验中心，不是全历史均值，也不是时刻 `t` 的动态基线比例

`T2` 首版建议只保留两类默认 profile：

| profile | 适用特征 | `s_prior` | `d_min_train` | `phi_over` |
|---|---|---:|---:|---:|
| `rate_core` | `error_rate`、`failure_rate`、`success_rate`、`syn_rate`、`rst_rate` | 2 | 50 | 1.5 |
| `ratio_bursty` | `new_peer_ratio`、`new_group_share`、其他新对象 / 稀有对象 / 结构份额类比例 | 4 | 100 | 2.0 |

参数解释：

- `s_prior`：比例平滑先验总强度，等价于加入多少“伪样本总量”。值越大，低分母下越倾向于向历史平均比例 `m0` 收缩，边界稳定性更强，但对短时真实突变的响应更保守。
- `phi_over`：过度离散修正系数，也可理解为方差膨胀因子，位于 `T2` 的在线方差层，用于吸收超出理想二项方差的额外波动。`T2 v1` 中它是按 `profile` 固定配置的在线常量，不做逐 feature 离线估计。值越大，标准化残差越保守。
- `v_floor`：最小方差下限，位于计数空间，用于防止极端边界比例和低分母场景下方差塌缩。在 `T2 v1` 中它是全局共享的固定实现常量，不按 feature 或 profile 细分，也不作为业务调参项。

### 7.4 变换与训练目标

首版采用带伪计数平滑的比例变换：

```text
p_t^smooth
= (numerator_t + alpha0(feature))
  / (denominator_t + alpha0(feature) + beta0(feature))

x_t
= logit(clip(p_t^smooth, eps_logit, 1 - eps_logit))
```

说明：

- `alpha0(feature)`、`beta0(feature)` 为 feature profile 给定的平滑先验
- `eps_logit` 用于避免 `logit(0)` 和 `logit(1)`；在 `v1` 中作为全局实现常量
- `T2` 的基线模型学习对象是 `x_t` 的季节期望，而不是裸 `ratio`
- 日 / 周 / 月结构、缺失处理、惰性 gap 处理，与 `T1` 保持一致
- `T2` 复用与 `T1` 相同的统一超模型组件栈，只是在 `logit` 空间建模比例的正常形态

训练目标：

- 学习 `x_t` 在该 `key + feature` 上的稳定季节期望 `mu_hat_t`
- 使高分母样本对基线训练贡献更大，低分母样本贡献更谨慎
- 使正常波动下的在线残差在标准化后接近零均值、近似稳定方差
- 不在训练阶段直接拟合业务“好坏方向”，只拟合数学上的稳定比例行为

训练集合定义为：

```text
Ω_train = { t | gate_train(t) = 1 }
```

其中 `gate_train(t)` 由 `d_min_train(feature)` 控制，只有分母达到训练门槛的 bucket 才参与训练。

`T2` 的主基线训练在 `logit` 空间中进行，采用分母加权的稳健回归目标。这里的 `Huber` 损失是一种稳健损失：残差较小时近似平方损失，残差较大时降低尖刺点对模型的拉动。

训练权重定义为：

```text
w_train(t)
= denominator_t / (denominator_t + d_min_train(feature))
```

性质：

- `0 < w_train(t) < 1`
- 刚达到训练门槛时，`w_train(t)` 约为 `0.5`
- 分母越大，`w_train(t)` 越接近 `1`

训练流程采用分母加权的层级分阶段拟合，而不是默认把 `Core / monthpos / event` 一次性整体联训：

```text
Step 1 (Core):
min Σ_{t∈Ω_train} w_train(t) * Huber(x_t - μ_core(t))
  + λ_season * ||θ_s||_2^2

Step 2 (monthpos):
r_core,t = x_t - μ_core(t)
min Σ_{t∈Ω_train} w_train(t) * Huber(r_core,t - h_monthpos(t))
  + λ_dom * ||θ_dom||_2^2
  + λ_dme * ||θ_dme||_2^2
  + λ_lwd * ||θ_lwd||_2^2

Step 3 (event):
r_enh,t = x_t - μ_core(t) - h_monthpos(t)
min Σ_{t∈Ω_train} w_train(t) * Huber(r_enh,t - h_event(t))
  + λ_event * ||θ_e||_2^2
```

说明：

- `μ_t` 的趋势 / 日周期 / 周周期 / 月位置 / 事件结构沿用 `T1` 公共主干
- `T2` 不在计数空间直接拟合基线，而是在 `logit` 空间拟合比例的正常时序形态
- `T2` 的块求解器接口、`Huber` 定义、`IRLS + weighted ridge` 流程，以及收敛 / 回退契约，统一沿用 `6.6.1 ~ 6.6.4`
- `monthpos`、`event` 的组件启用条件沿用 `T1` 的语义，但其统计对象限定为 `Ω_train` 中满足分母门槛的有效 bucket
- `T2` 的 `Core` 同样不内建 `changepoint`，结构变化默认通过正式重建的历史窗口选择处理
- `T2 v1` 的默认正式可用形态是 `core_no_month`；尤其对 `ratio_bursty`、新 Key 和低覆盖比例序列，不要求先具备 `monthpos` 才能产出正式基线
- 训练完成后先得到 `μ_t`，再恢复 `p_hat_t = sigmoid(μ_t)`
- `phi_over` 不并入主训练损失，也不做逐 feature 训练；在线方差层直接使用所属 `profile` 的固定值

### 7.5 在线评分与漂移证据累积器（BOCPD-style）接法

`T2` 中的 `漂移证据累积器（BOCPD-style）` 复用 `T1a` 的同一固定状态实现。这里的 `BOCPD-style` 同样只表示方法来源，不表示要在比例模型中额外引入完整 `BOCPD` 后验递推。

在线阶段先由基线模型给出：

```text
mu_hat_t
```

再恢复比例期望：

```text
p_hat_t = sigmoid(mu_hat_t)
```

首版推荐使用带符号标准化偏差残差：

```text
Var_ideal,t
= denominator_t * p_hat_t * (1 - p_hat_t)

Var_model,t
= Var_ideal,t * phi_over(profile(feature))

Var_eff_t
= max(Var_model,t, v_floor)

r_t
= (numerator_t - denominator_t * p_hat_t)
   / sqrt(Var_eff_t)
```

其中：

- `phi_over(profile(feature)) >= 1` 为过度离散修正系数，用于放大理想二项方差，使模型不过度乐观；它按 `profile` 固定给出，在线评分热路径直接读取
- `v_floor` 为全局共享的最小方差下限，避免极端边界处数值不稳；它解决的是数值稳定问题，不替代 `phi_over` 的统计修正职责
- `Var_eff_t` 为最终用于标准化残差的有效方差

`T2 v1` 的在线方差层职责拆分为：

- `phi_over`：负责吸收稳定存在的过度离散，按 `profile` 固定配置
- `v_floor`：负责数值稳定，作为全局固定实现常量
- `kappa_den / rho_t`：只负责置信度修正，不进入 `r_t`

因此，`T2 v1` 不再定义逐 feature 的 `phi_over` 离线估计、回退或覆盖流程。在线评分只执行上式中的固定方差计算，不额外维护方差层状态。

门控与可靠性适配沿用 `T1b` 的统一模式，但把样本量替换为分母：

```text
gate_score(t) = I(denominator_t >= d_score_min(feature))
gate_train(t) = I(denominator_t >= d_min_train(feature))
gate_shift(t) = I(denominator_t >= d_shift_min(feature))
```

分母支撑度置信度修正定义为：

```text
rho_t = sqrt(1 + kappa_den(feature) / denominator_t)
confidence = confidence_base / rho_t
```

其中 `confidence_base` 沿用 `T1` 中由历史长度、覆盖率、模型状态得到的基准置信度。`rho_t` 只用于 `confidence` 与 `evidence`，不再二次缩放 `r_t`。

点异常强度定义为：

```text
score_point
= clip(
    (|r_t| - z_warn)
    / (z_crit - z_warn),
    0, 1
  )
```

持续性计数规则沿用 `T1`，但把 `z_t` 替换为 `r_t`：

```text
if |r_t| >= z_warn:
    persistence += 1
else:
    persistence = 0
```

`T2` 复用 `T1a` 的固定状态漂移证据累积器，只是把输入换成比例残差：

```text
e_t = clip(r_t, -shift_clip, shift_clip)
u_t = α * e_t + (1 - α) * u_{t-1}
```

当 `gate_shift(t) = 1` 时，`ShiftState` 的更新公式、`dir_t` 的定义、`p_shift_t` 的计算、gap 下的冻结 / 重置规则，全部沿用 `T1a` 第 `6.8` 节定义。

漂移分数仍沿用统一融合公式：

```text
score_shift = clip((p_shift_t - p_shift_low) / (p_shift_high - p_shift_low), 0, 1)
normalized_score = 1 - (1 - score_point) * (1 - w_shift * score_shift)
```

评分与输出语义：

- `raw_score` 建议定义为 `|r_t|`
- 方向信息由 `sign(r_t)` 保留，但输出标签仍使用统一数学标签
- 漂移证据输入采用截断后的带符号残差 `clip(r_t, -shift_clip, shift_clip)`
- 漂移证据累积器（BOCPD-style）只负责识别“比例行为基线可能失配”，不直接承担业务判别

### 7.6 阈值语义

`T2` 的阈值统一定义在标准化偏差残差 `r_t` 上，而不是定义在原始 `ratio` 上。`denominator` 对异常强度的影响已经通过方差层（最终体现为 `Var_eff_t`）内生表达，不再让阈值显式依赖分母。

点异常阈值：

- `z_warn`：单窗口异常起点
- `z_crit`：单窗口强异常阈值
- `shift_clip`：漂移证据输入截断上限

漂移阈值：

- `p_shift_low`：进入疑似漂移区间，`T2` 默认沿用 `T1` 的统一定义与建议初值
- `p_shift_high`：高置信度漂移阈值，`T2` 默认沿用 `T1` 的统一定义与建议初值
- `M_shift`：低阈值持续窗口数，`T2` 默认沿用 `T1` 的统一定义与建议初值

缺失阈值：

- `G_skip`：允许冻结状态的最大连续缺失窗口数，`T2` 默认沿用 `T1` 的统一定义与建议初值
- `G_reset`：超过该值直接重置漂移证据状态，`T2` 默认沿用 `T1` 的统一定义与建议初值

支撑度阈值：

- `d_score_min(feature)`：最小评分分母，由 `d_min_train(feature)` 派生
- `d_min_train(feature)`：最小训练分母
- `d_shift_min(feature)`：最小漂移更新分母，由 `d_min_train(feature)` 派生
- `kappa_den(feature)`：分母支撑度置信度修正强度，由 `d_min_train(feature)` 派生，仅影响 `confidence`

### 7.7 输出语义与正式重建触发

`T2` 的输出协议沿用第 `5.2` 节统一 `DetectorResult`。相对 `T1` 的差异主要在 `raw_score` 和 `evidence`：

- `raw_score`：建议定义为 `|r_t|`
- `normalized_score`：取 `1 - (1 - score_point) * (1 - w_shift * score_shift)`
- `confidence`：综合历史长度、训练覆盖率、模型状态与当前分母支撑度
- `reason_code`：按优先级输出 `baseline_shift_up`、`baseline_shift_down`、`spike`、`drop`
- `evidence`：至少包含 `numerator`、`denominator`、`p_t^smooth`、`x_t`、`p_hat_t`、`Var_eff_t`、`r_t`、`rho_t`、`p_shift_t`、`dir_t`、`score_point`、`score_shift`

其中：

- `baseline_shift_up / down` 表示当前比例行为相对基线发生持续上移 / 下移
- `spike / drop` 表示当前窗口相对基线发生瞬时高跳 / 低跳
- 以上标签仅表示数学模式，不代表业务好坏方向

异步正式重建候选建议在满足以下任一条件时触发：

- `p_shift_t >= p_shift_high`
- `p_shift_t >= p_shift_low` 且连续 `M_shift` 个窗口成立
- `r_t` 在最近短窗内持续同向偏移，且点异常频繁出现

以上条件只负责产生“正式重建候选”；统一慢路径、`shadow baseline`、`history_reader` 约束与切换规则见第 `10.2` 节。

### 7.8 冷启动与基线来源（可选）

`T2` 直接复用第 `2.3` 节定义的统一 `基线来源` 机制，不再单独定义 `T2` 专属来源链。`T2` 只是对“什么样的来源配置有效”补充自身约束。

`T2` 的来源配置约束：

- 上游若为某个 `(key, feature)` 显式配置 `BaselineSourceConfig`，其中每个 `source_i` 必须与 `self` 共享同一 `feature` 身份
- `source_i` 必须共享同一 `numerator / denominator` 统计口径，以及相同的时间粒度与 `bucket_id` 对齐方式
- `T2` 不允许跨比例定义借用来源，例如 `error_rate` 不能借用 `success_rate`，`new_peer_ratio` 不能借用 `new_group_share`
- 若未配置 `BaselineSourceConfig`，则 `T2` 冷启动时仅观察并累计本级训练数据

冷启动行为：

- 当 `baseline_source = self` 时，使用本级基线正常评分
- 当 `baseline_source = source_i` 且 `i >= 1` 时，使用该基线来源评分；`T2` 的评分公式不变，只是 `p_hat_t` 改为该来源提供的基线期望，而 `phi_over` 仍按当前 `feature profile` 的固定值使用
- 当 `baseline_source = none` 时，仅观察，不正式评分
- 即使当前借用了其他基线来源，本级序列仍继续累计 `Ω_train` 并训练自己的模型，不能因为来源借用而停止本级训练
- 一旦 `self` 进入 `core_no_month_ready` 或 `full_ready`，`v1` 直接切换到本级基线，不做多来源混合或渐进 blending

输出约束：

- 使用其他基线来源时，不新增专用 `reason_code`，仍沿用 `baseline_shift_up`、`baseline_shift_down`、`spike`、`drop`
- `evidence` 应补充 `baseline_source_kind = self | configured_source | none`、`baseline_source_key` 与 `model_state`
- 当 `baseline_source_kind = configured_source` 时，`confidence_base` 应低于本级自有基线处于可服务状态时的水平，以反映其仅是冷启动期的借用参考；若本级正式模型已 ready，则该借用置信度还应低于正式模型直出时的水平

### 7.9 性能约束

为保证分钟级高基数场景可落地，`T2` 首版必须满足以下约束：

- 单条记录在线评分复杂度必须为 `O(1)`
- 单序列状态复杂度必须为 `O(1)`
- 热路径只允许固定次数的四则运算及少量 `log`、`exp`、`sqrt`
- 热路径禁止精确 `Binomial` 尾概率、精确 `Beta-Binomial` 尾概率、不完全 Beta 函数、`lgamma` 密集计算
- 热路径禁止逐 bucket 迭代求解、数值优化、EM 或采样类算法

工程建议：

- 方案 B 的在线实现固定为“平滑比例 + `logit` 基线 + 标准化偏差残差”
- 若后续需要更精确的概率解释，只允许对 Top-N 可疑点走慢路径复核，不进入全量在线热路径
- 离线训练只要求完成 `m0` 统计、`alpha0 / beta0` 派生和统一块求解器训练；`phi_over` 直接取 `profile` 固定值，不引入额外的逐 feature 方差标定流程

---

## 8. T3：分布漂移类设计（v1）

### 8.1 设计原则

- `T3` 的建模对象不是某个 `group` 的单值时间序列，而是固定 `group_space` 上的一份质量分布。
- `group_space` 定义“分到哪些组”；`mass_metric` 定义“什么量在这些组上分配”，例如 `conn_count`、`bps`、`pps`。
- 同一 `(key, bucket_id, group_space)` 往往同时存在多个 `mass_metric`；热路径不应为每个指标重复上传一份 group 列表。
- 因此，`T3` 的运行时输入采用列式稀疏块（columnar sparse block），而不是对象数组；多个指标共享同一份 `group_idx`。
- 上游负责 group 的划分与映射关系维护；基线层只消费稳定 `group_idx`，不参与 group 推断。

### 8.2 任务级静态规格

`T3` 的任务级静态规格定义为：

```text
T3TaskSpec = {
  task_id,
  feature_base,
  group_space_id,
  group_space_version?,
  metric_set_id,
  metrics = [m_0, m_1, ... , m_{M-1}],
  encode_type,
  support_policy,
  summary_policy
}
```

字段说明：

- `task_id`：任务静态标识；运行时块只引用该 ID，不重复上传任务元数据。
- `feature_base`：该分布对象的基础特征身份，例如 `client_group_mix`、`peer_role_mix`。
- `group_space_id`：group 空间静态标识，由上游定义并维护其映射关系与版本。
- `group_space_version?`：可选的 group 空间语义版本；当上游 group 划分规则或 `group_id` 映射语义发生变化时应更新。
- `metric_set_id`：指标集合静态标识，用于避免在每个 bucket 上重复上传 `conn_count / bps / pps` 等指标名。
- `metrics`：固定顺序的 `mass_metric` 列表，决定 `values[M][nnz]` 中第 `m` 列的语义。
- `encode_type`：稀疏编码方式，首版支持 `exact_sparse | topk_other`。
- `support_policy`：支持空间选择策略，用于从超大 `group_space` 中收口出当前模型真正建模的固定支持集。
- `summary_policy`：摘要特征选择策略，用于确定 `headK` 与 `stable head set` 的规模。

约束：

- 一个 `task_id` 唯一绑定一组 `(feature_base, group_space_id, metric_set_id)`。
- 同一 `task_id` 下的全部 `mass_metric` 必须共享相同的时间粒度、`bucket_id` 对齐方式与 group 划分口径。
- `metrics` 只允许可加和的质量指标，例如 `count`、`bytes`、`packets`、`requests`；非可加和统计量不得直接作为 `mass_metric` 输入。
- 若某类统计量本身不可加和，则必须由上游先改写为可加和组成量后再进入 `T3`，例如把平均值改写为 `sum + sample_count`。

### 8.3 支持空间与建模依据

仅有 `group_space_id` 并不足以支撑 `T3` 建模。对超大值域空间，`T3` 真正可比较的对象不是“完整原始 group 空间”，而是当前模型版本在其上冻结下来的固定服务基准（basis）。

任务级支持策略定义为：

```text
SupportPolicy = {
  K_support,
  min_hist_share,
  min_active_ratio
}
```

任务级摘要策略定义为：

```text
SummaryPolicy = {
  K_head,
  K_stable
}
```

字段说明：

- `K_support`：显式支持集的最大规模；用于限制每个逻辑 `T3` 特征保留多少个稳定 group。
- `min_hist_share`：进入显式支持集所需的最小历史累计占比门槛；过小的长尾 group 不单独保留。
- `min_active_ratio`：进入显式支持集或 `stable head set` 所需的最小活跃覆盖率。
- `K_head`：用于 `headK_share` 的头部规模；`v1` 建议默认 `5`。
- `K_stable`：用于固定历史头部集合 `StableHeadSet_T3` 的目标规模；`v1` 允许 `1 ~ 10`，建议默认 `5`。

对每个逻辑 `T3` 特征 `Feature_T3(m) = (feature_base, metric_m)`，正式模型在训练 / 正式重建时，先基于 `support_policy` 派生一个显式支持集：

```text
SupportExplicit_T3(m) = { g_1, g_2, ... , g_K }
```

选择原则：

- `g_1 ... g_K` 来自训练窗口上的稳定高占比 group，依据历史累计占比与活跃覆盖率共同筛选。
- 当前 bucket 的 `Top-K` 不是模型支持集；`Top-K` 只负责运行时输入压缩，不能直接决定时间上可比较的建模空间。
- `SupportExplicit_T3(m)` 只包含真实 group，不包含 `__other__`、`__new__` 等运行时聚合槽位。
- `SupportExplicit_T3(m)` 只在正式重建或 basis 刷新时更新，不得在在线热路径上随每个 bucket 动态变动。

然后，从 `SupportExplicit_T3(m)` 中派生固定历史头部集合：

```text
StableHeadSet_T3(m) = [h_1*, h_2*, ... , h_{K_stable_eff}*]
```

其中：

- `h_i*` 仅来自 `SupportExplicit_T3(m)`
- `K_stable_eff <= min(K_stable, K_support)`
- 若满足覆盖率门槛的 group 不足，则 `K_stable_eff` 可小于目标值

其选择规则为：

```text
hist_share(g)
= Σ_t mass_t(g, m) / Σ_t total_t^(m)

active_ratio(g)
= 有效 bucket 中 mass_t(g, m) > 0 的占比
```

然后：

1. 仅保留 `active_ratio(g) >= min_active_ratio` 的候选  
2. 按 `hist_share(g)` 从高到低排序  
3. 取前 `K_stable` 个，得到 `StableHeadSet_T3(m)`  

在 `StableHeadSet_T3(m)` 上，进一步定义历史头部内部原型分布：

```text
q_i^(m)
= hist_share(h_i*) / Σ_{j=1..K_stable_eff} hist_share(h_j*)
```

据此，对每个 `metric_m` 定义当前模型版本的正式服务基准：

```text
ServiceBasis_T3(m) = {
  basis_version,
  group_space_id,
  group_space_version?,
  support_explicit = SupportExplicit_T3(m),
  stable_head      = StableHeadSet_T3(m),
  head_proto_q     = [q_1^(m), ... , q_{K_stable_eff}^(m)],
  K_head
}
```

设计含义：

- 对 `T3` 来说，真正可比较的不是“原始 group 空间”，而是同一 `ServiceBasis_T3(m)` 下提取出的摘要特征。
- `g_1 ... g_K` 不是由上游固定指定，而是由基线在重建窗口上派生并在模型版本内冻结。
- `StableHeadSet_T3(m)` 也不是当前动态 topK，而是当前 basis 的固定历史头部集合。
- 若某个 `group_space` 在合理训练窗口内无法形成稳定支持集，则应退化为更粗粒度 group，或只保留全局形状特征。

### 8.4 运行时输入：`T3Block`

`T3` 的运行时输入不是逐特征对象流，而是单块稀疏分布：

```text
T3Block = {
  task_id,
  key,
  bucket_id,
  nnz,
  group_idx[nnz],
  totals[M],
  active_count[M]?,
  values[M][nnz]
}
```

字段说明：

- `task_id`：关联到第 `8.2` 节定义的 `T3TaskSpec`。
- `key`：被分析主体；热路径实现中宜预编码为稳定整数 ID。
- `bucket_id`：固定统计窗口的绝对编号。
- `nnz`：本块中实际上传的非零 group 数。
- `group_idx[j]`：`group_space_id` 下的稳定 group 整数 ID，按升序排列。
- `totals[m]`：第 `m` 个 `mass_metric` 在当前 bucket、全量 group 空间上的总质量。
- `active_count[m]?`：第 `m` 个 `mass_metric` 在当前 bucket 的活跃 group 数。它是上游可选提供的增强字段，用于启用 `distinct_group_count`，捕捉“总质量还不大，但活跃 group 数已经明显扩张”的早期结构信号。这里的计数是按 `metric_m` 分别定义的，而不是对整个 `T3Block` 只给一个统一计数。
- `values[m][j]`：第 `m` 个 `mass_metric` 在 `group_idx[j]` 上的质量。

语义约束：

- 同一块中的 `group_idx` 是所有 `mass_metric` 非零支撑集合的并集；若某个 group 未出现在 `group_idx` 中，则其在所有 `mass_metric` 上都视为 `0`。
- `values` 物理上采用按指标分列的连续存储；逻辑上可视为一个 `M × nnz` 的非负稀疏质量矩阵。
- `totals[m] >= Σ_j values[m][j]` 必须成立；若 `topk_other` 显式上传了预留的 `__other__` 槽位，则建议满足 `totals[m] = Σ_j values[m][j]`。
- `active_count[m]` 若存在，则必须与当前 `metric_m` 的活跃定义一致；若不存在，基线层不得自行从 `topk_other` 结果推断全量活跃 group 数。
- `active_count[m]` 是可选增强输入；若未提供，则仅禁用 `distinct_group_count`，其余摘要特征仍可正常工作。
- 在网络流量场景中，若 `conn_count / bps / pps` 只要“该 group 本分钟产生过流量”就记为活跃，则不同 `m` 上的 `active_count[m]` 往往相同；但在通用方案里不得假设这一点，因为不同业务可能对不同 `metric` 采用不同活跃定义。
- 为保证热路径性能，`task_id`、`key`、`group_idx` 在运行时都应使用稳定整数编码，而不是字符串。

两类编码方式的解释：

- `exact_sparse`：上传该 bucket 上全部非零 group，适用于上游已经完成稳定 group 划分，且单 bucket 的 `nnz` 可控的场景。
- `topk_other`：运行时压缩编码，不等价于模型支持集。它至少应保留当前 bucket 中所有属于任一 `SupportExplicit_T3(m)` 的活跃 group；同时，为保证 `top1_share / headK_share` 可精确构造，还必须保留当前 bucket 在该 `metric_m` 下的真实前 `K_head` 个活跃 group。其余非支持集 group 可折叠到预留的 `__other__` group。

### 8.5 `topk` 与 `sketch` 的职责边界

`T3 v1` 对 `topk` 与 `sketch` 的定位如下：

- `topk` 是运行时压缩输入或候选保留手段，不是正式建模依据。
- `sketch` 可以用于上游预处理层完成 heavy hitter 候选发现、尾部质量估计或近似去重，但不作为 `T3` 的正式建模输入。
- 基线层正式消费的仍然是 `T3Block`：也就是稳定 group ID 上的稀疏质量块，而不是 `sketch` 内部状态。
- 若上游依赖 `sketch` 构造 `topk_other`，则必须保证：当前 basis 的 `support_explicit` 中所有活跃 group，以及当前真实前 `K_head` 个活跃 group，不得因为近似截断而丢失；否则该 bucket 将失去与历史模型的可比性。
- 因此，`sketch` 的职责是“帮助上游决定该上传什么”，而不是“替代基线层的支持空间定义”。

### 8.6 多指标展开语义

一个 `T3Block` 可以同时承载多个 `mass_metric`。基线层内部不把它视为一个“多变量联训模型”，而是把它展开为多个共享同一份 group 快照的逻辑 `T3` 特征：

```text
Feature_T3(m) = (feature_base, metric_m)
Series_T3(m)  = (key, Feature_T3(m))
```

含义：

- 若 `metrics = [conn_count, bps, pps]`，则一个 `T3Block` 会派生出 3 个逻辑 `T3` 特征。
- 这 3 个特征共享同一份 `group_idx`，但各自使用不同的 `totals[m]` 与 `values[m][:]`。
- 它们在基线层中独立建模、独立评分，后续若需要组合提级，再由统一融合层完成。
- 基线层不得要求上游把同一份 group 分布拆成 3 份重复输入。

示例：

```text
metrics   = [conn_count, bps, pps]
group_idx = [7, 18, 92, 104]
totals    = [12000, 860000000, 450000]
values    = [
  [4200, 3100,  900,   600],
  [320000000, 170000000, 110000000, 80000000],
  [130000, 102000, 40000, 29000]
]
```

### 8.7 与统一抽象的关系

`T3Block` 是 `T3` 在运行时的高性能专用编码，不改变本文第 `2.2` 节的统一抽象。语义上，它等价于一批共享 `(key, bucket_id, group_space)` 的 `T3 Observation`：

```text
Observation_T3(m) = {
  key,
  feature = (feature_base, metric_m),
  bucket_id,
  context = {
    group_space_id,
    encode_type,
    group_idx,
    total = totals[m],
    mass  = values[m][:]
  }
}
```

设计含义：

- 统一框架层仍然按 `Series = (key, feature)` 思考问题。
- `T3Block` 只是在输入编码上消除重复的 `group_idx`、指标名和任务元数据，以支撑高基数分钟级热路径。
- `T3 v1` 的摘要特征提取、在线评分与阈值语义，都基于这里定义的 `Observation_T3(m)` 继续展开。

### 8.8 `T3 v1` 的落地形态：关系分布摘要特征层

考虑到网络流量场景的吞吐量与高基数约束，`T3 v1` 采用“关系分布摘要特征层”落地：先从 `T3Block` 中提取少量标量摘要特征，再分别路由到 `T1 / T2` 检测主干。

含义：

- `T3Block` 仍然是 `T3` 的专用输入与语义来源。
- `T3 v1` 的首版在线检测重点是“关系分布摘要特征提取”。
- 这些摘要特征保留连接关系变化的核心信息，但把检测对象压缩成少量标量，以降低状态量、训练复杂度和调参面。

统一定义：

```text
SummaryFeature_T3 = f(T3Block, metric_m, ServiceBasis_T3(m))
```

其中：

- `metric_m`：`T3Block` 中的某一个质量指标，如 `conn_count`、`bps`、`pps`
- `ServiceBasis_T3(m)`：第 `8.3` 节定义的该指标当前服务基准
- `f`：从当前分布中提取一个标量摘要量的函数

对任意 `metric_m`，先定义当前 share 向量：

```text
total_t^(m) = totals[m]
p_t^(m)(g)  = mass_t(g, m) / total_t^(m)
```

约束：

- 当 `total_t^(m) <= 0` 时，该 `metric_m` 的全部摘要特征都视为 gap，不评分、不更新。
- 当某个 `group` 在当前 bucket 未出现时，其 share 按 `0` 处理，而不是 gap。

#### 8.8.1 运行时集合、头部排序与性能契约

对每个 `metric_m`，记：

```text
SupportExplicit_T3(m) = ServiceBasis_T3(m).support_explicit
StableHeadSet_T3(m)   = ServiceBasis_T3(m).stable_head
Active_t^(m)          = { g | mass_t(g, m) > 0 }
```

据此定义：

```text
support_mass_t^(m)
= Σ_{g ∈ SupportExplicit_T3(m)} mass_t(g, m)

out_mass_t^(m)
= total_t^(m) - support_mass_t^(m)
```

通俗解释：

- `support_mass_t^(m)`：当前这一分钟里，历史显式支持集还占了多少质量
- `out_mass_t^(m)`：当前这一分钟里，有多少质量已经落到支持集之外

对当前 share 向量 `p_t^(m)`，按 share 从大到小排序，记前 `K_head` 个当前头部 group 为：

```text
g_(1,t)^(m), g_(2,t)^(m), ... , g_(K_head,t)^(m)
```

若当前活跃 group 不足 `K_head` 个，则缺失位置按 share `0` 处理。

性能约束：

- `top1_share / headK_share` 的正确性依赖输入满足第 `8.4` 节的 `topk_other` 约束：当前真实前 `K_head` 个活跃 group 必须被显式保留。
- 在线热路径对单个 `metric_m` 的摘要提取必须保持 `O(nnz)`；不得为 `T3` 引入与原始 group 值域空间线性相关的额外状态。

#### 8.8.2 通用摘要特征定义与通俗解释

首版建议保留以下通用摘要特征：

| 特征名 | 数学定义 | 路由 | 默认配置 | 通俗解释 |
|---|---|---|---|---|
| `entropy_shannon` | `- Σ_{g ∈ Active_t^(m)} p_t^(m)(g) log(p_t^(m)(g))` | `T1a` | `value = entropy_shannon`，`transform = identity` | “当前分布更分散还是更集中。”值越大，说明份额越分散；值越小，说明越集中到少数 group。 |
| `top1_share` | `p_t^(m)(g_(1,t)^(m))` | `T2` | `numerator = mass_t(g_(1,t)^(m), m)`，`denominator = total_t^(m)`，`profile = rate_core` | “当前第一名 group 占了多少。”它用来判断是否突然被某一个 group 主导。 |
| `headK_share` | `Σ_{k=1}^{K_head} p_t^(m)(g_(k,t)^(m))` | `T2` | `numerator = Σ_{k=1}^{K_head} mass_t(g_(k,t)^(m), m)`，`denominator = total_t^(m)`，`profile = rate_core` | “当前前 `K_head` 名 group 合起来占多少。”它比只看第一名更稳，用来判断整体头部集中度。 |
| `out_of_support_share` | `out_mass_t^(m) / total_t^(m)` | `T2` | `numerator = out_mass_t^(m)`，`denominator = total_t^(m)`，`profile = ratio_bursty` | “当前这一分钟里，有多少质量已经落在历史显式支持集之外。”它是 `v1` 默认的 support 外扩散特征。 |

补充说明：

- `entropy_shannon` 对应“分布整体变得更散或更集中”。
- `top1_share`、`headK_share` 对应“头部结构是否被显著强化或削弱”。
- `out_of_support_share` 不区分“真正新 group”和“旧尾部 group”，它只回答“有多少质量已经逃离当前显式支持集”。`new_group_share / other_group_share` 作为未来增强保留，不进入 `v1` 默认正式设计。

#### 8.8.3 可选增强摘要特征：`distinct_group_count`

为补足 share 类摘要特征对“低质量、广泛扩散”不够敏感的盲区，`T3 v1` 保留 `distinct_group_count` 作为可选增强特征；当上游能够低成本、稳定地给出 `active_count[m]` 时，可启用该特征：

| 特征名 | 数学定义 | 路由 | 默认配置 | 通俗解释 |
|---|---|---|---|---|
| `distinct_group_count` | `active_count[m]` | `T1a` | `value = active_count[m]`，`transform = log1p` | “这一分钟里，真正活跃的 group 有多少个。”它适合看来源是否突然变多、变杂。 |

启用约束：

- `active_count[m]` 由上游选择性提供；提供时启用 `distinct_group_count`，不提供时直接禁用该特征。
- 当 `active_count[m]` 缺失时，`T3` 仍可完整运行，但会削弱对“低质量、多点扩散 / 扫描式扩张”的敏感度。
- `topk_other` 编码下，若没有 `active_count[m]`，基线层不得用 `nnz` 近似替代真实活跃 group 数。
- `active_count[m]` 必须与当前 `metric_m` 的活跃判定一致；例如对 `bps`、`pps`、`conn_count` 的活跃定义，应由上游在同一统计口径下给出。
- 在网络流量里，若活跃定义统一为“只要该 group 在本窗口产生了流量就计数”，则 `active_count[conn_count]`、`active_count[bps]`、`active_count[pps]` 往往会相同；但这只是流量场景下常见的特例，不应写成通用算法假设。

设计意图：

- `entropy_shannon`、`out_of_support_share` 都是质量加权摘要；若突然出现很多小 group，但它们暂时只占很小质量，这两类特征的变化可能并不强。
- `distinct_group_count` 则直接刻画“当前到底活跃了多少个 group”，它补的是活跃广度，而不是质量占比。
- 因此，它是有价值的增强特征，但不是 `T3 v1` 主闭环成立的必要前提。
- 这份计数应由上游在具备稳定定义、且能低成本提供时选择性给出，而不是由基线层在热路径上为此引入额外代价。

### 8.9 固定身份 `stable head set` 特征

为增强“历史固定头部集合整体失守”与“历史固定头部集合内部比例关系变化”的检测能力，`T3 v1` 在通用摘要特征之外，额外定义一组固定身份 `stable head set` 特征。

#### 8.9.1 设计目标

这组特征解决的不是“当前第一名是谁”，而是：

- 历史上一批长期重要的头部 group 是否整体失守
- 历史固定头部集合内部的份额分配是否发生明显变化

也就是说：

- `top1_share / headK_share` 负责回答“当前分布形状是否更集中”
- `stable head set` 负责回答“历史固定对象是否还在、内部关系是否变了”

#### 8.9.2 `stable head set` 的选择规则

对每个逻辑 `T3` 特征 `Feature_T3(m) = (feature_base, metric_m)`，在正式重建或 basis 刷新时，从 `SupportExplicit_T3(m)` 中选出固定历史头部集合：

```text
StableHeadSet_T3(m) = [h_1*, h_2*, ... , h_{K_stable_eff}*]
```

选择规则沿用第 `8.3` 节定义：

- 候选集仅来自 `SupportExplicit_T3(m)`
- 只保留 `active_ratio(g) >= min_active_ratio` 的 group
- 按 `hist_share(g)` 从高到低排序
- 取前 `K_stable` 个
- `v1` 允许 `K_stable ∈ [1, 10]`，建议默认 `5`

通俗解释：

- `stable head set` 是“这段历史里最稳定、最重要的一批老牌头部 group”
- 它们不是当前这一分钟临时排出来的前 `K_stable` 名，而是当前 basis 冻结下来的固定历史对象集合

#### 8.9.3 运行时 share 向量、coverage 与内部子分布

对当前 bucket，记 `K_stable_eff = |StableHeadSet_T3(m)|`。对每个 `i = 1 .. K_stable_eff`，定义：

```text
s_i,t^(m) = p_t^(m)(h_i*)
```

若某个 `stable head` 在当前 bucket 未出现，则对应 `s_i,t^(m) = 0`。

再定义当前固定头部集合整体覆盖率：

```text
c_t^(m) = Σ_{i=1..K_stable_eff} s_i,t^(m)
```

若 `c_t^(m) > 0`，则定义当前固定头部集合内部的归一化子分布：

```text
u_i,t^(m) = s_i,t^(m) / c_t^(m)
```

说明：

- `s_i,t^(m)` 是“历史固定第 `i` 个头部，现在还占多少”
- `c_t^(m)` 是“历史固定头部集合整体现在还占多少”
- `u_i,t^(m)` 是“在这批固定头部内部，现在各自如何分配”

#### 8.9.4 特征定义与通俗解释

首版建议增加以下固定身份特征族：

| 特征名 | 数学定义 | 路由 | 默认配置 | 通俗解释 |
|---|---|---|---|---|
| `stable_g[i]_share` | `s_i,t^(m)` | `T2` | `numerator = mass_t(h_i*, m)`，`denominator = total_t^(m)`，`profile = rate_core` | “历史固定第 `i` 个头部 group，现在还占多少份额。”它用于跟踪固定老牌对象是否失守。 |
| `stable_headK_coverage` | `c_t^(m)` | `T2` | `numerator = Σ_{i=1..K_stable_eff} mass_t(h_i*, m)`，`denominator = total_t^(m)`，`profile = rate_core` | “历史固定头部集合合起来还占多少。”若它明显下降，说明历史头部整体正在被新结构分流。 |
| `stable_headK_mix_drift` | `0.5 * Σ_{i=1..K_stable_eff} |u_i,t^(m) - q_i^(m)|` | `T1a` | `value = stable_headK_mix_drift`，`transform = identity` | “历史固定头部集合内部的份额分配，与历史原型相比偏了多少。”它用于度量头部内部关系是否整体发生重排。 |

补充说明：

- `stable_g[i]_share` 是参数化特征族，不强制固定为 `2` 个；其规模由 `K_stable` 决定。
- `stable_headK_mix_drift` 是单标量，不展开成 pairwise 比例矩阵，因此不会引入 `O(K^2)` 特征数膨胀。
- `stable_headK_mix_drift` 的值域落在 `[0, 1]`，值越大表示当前固定头部内部比例关系与历史原型差异越大。

#### 8.9.5 缺失与降级规则

- 若 `K_stable_eff = 0`，则整组固定身份特征都不创建。
- 若 `K_stable_eff >= 1`，则 `stable_g[i]_share` 与 `stable_headK_coverage` 都可创建；即使当前 share 为 `0` 也应保留，因为这本身是有意义信号。
- `stable_headK_mix_drift` 仅在同时满足以下条件时有效：
  1. `K_stable_eff >= 2`
  2. `c_t^(m) > 0`
- 当 `K_stable_eff = 1` 时，`stable_headK_mix_drift` 退化为常数 `0`，因此在 `v1` 中直接禁用，而不是保留为无信息特征。
- 若 `total_t^(m) <= 0`，则该 bucket 对应的全部固定身份特征都视为 gap

设计意图：

- 不为了凑齐特征而伪造不存在的历史头部
- 但一旦历史头部已经被定义，它在当前分钟掉到 `0` 本身就应被保留为可检测信号

#### 8.9.6 命名示例

对 `feature_base = client_group_mix`、`metric = bps`、`K_stable = 5` 的场景，可派生出如下摘要特征：

- `client_group_mix_bps_stable_g1_share`
- `client_group_mix_bps_stable_g2_share`
- `client_group_mix_bps_stable_g3_share`
- `client_group_mix_bps_stable_g4_share`
- `client_group_mix_bps_stable_g5_share`
- `client_group_mix_bps_stable_head5_coverage`
- `client_group_mix_bps_stable_head5_mix_drift`

同理，对 `pps`、`conn_count` 也可分别派生，但各自拥有独立的 `ServiceBasis_T3(m)` 与 `StableHeadSet_T3(m)`。

### 8.10 `T3` 摘要特征的路由与实现收口

`T3 v1` 的摘要特征不单独实现一套新的时间基线，而是按以下规则复用现有检测主干。第 `8.8`、`8.9` 节的特征表已经给出每个摘要特征的数学定义、路由方向与默认配置，本节只收口统一的 `Observation` 构造规则，不再逐特征重列表达。

- 受限于 `[0,1]` 的 share / balance / coverage 特征，统一路由到 `T2`
- 非负标量摘要且不需要显式分母方差建模的特征，统一路由到 `T1a`
- `T3 v1` 不再为摘要特征引入额外的 `T1b` 专属连续值兼容层

设计意图：

- `T2` 已经能很好处理“占比 / 份额 / 比例”这类摘要特征
- `T1a` 已经能很好处理“非负标量时序”这类摘要特征
- 这样 `T3` 只负责提取特征，不再重复发明新的检测器

#### 8.10.1 Share 类摘要到 `T2`

对任意 share 类特征，都按：

```text
Observation_T2 = {
  key,
  feature,
  bucket_id,
  numerator,
  denominator
}
```

构造，其中：

- `numerator`：当前摘要特征所对应的质量
- `denominator`：该摘要特征的总支撑质量

各 share / coverage 特征的 `numerator`、`denominator` 与默认 `profile`，沿用第 `8.8.2` 节和第 `8.9.4` 节的特征表定义。

补充约束：

- 当某个 `T2` 摘要特征的 `denominator <= 0` 时，该特征在当前 bucket 视为 gap。
- `stable_g[i]_share` 是参数化特征族；实现上按 `i = 1 .. K_stable_eff` 展开。

#### 8.10.2 标量摘要到 `T1a`

对非负标量摘要特征，统一按：

```text
Observation_T1a = {
  key,
  feature,
  bucket_id,
  value
}
```

构造。各标量摘要的 `value` 与默认变换，沿用第 `8.8.2` 节、第 `8.8.3` 节和第 `8.9.4` 节的特征表定义。

补充约束：

- 各特征的启用条件、gap 规则与降级规则，沿用第 `8.8` 节和第 `8.9.5` 节，不再在这里重复展开。

#### 8.10.3 首版 profile 收口原则

`T3 v1` 不新增新的 `T2 profile`，首版只复用已有两类：

- `rate_core`
- `ratio_bursty`

也不为 `T3` 摘要特征新增新的 `T1a` 求解器或阈值体系，只通过：

- `feature` 身份
- 变换选择
- 现有共享阈值

完成首版落地。

### 8.11 `T3 v1` 首版推荐特征集

综合性能、可实现性和检测收益，`T3 v1` 对每个 `(key, group_space, metric)` 的首版推荐特征集按分层收口如下：

必选核心：

- `entropy_shannon`
- `top1_share`
- `headK_share`
- `out_of_support_share`
- `stable_headK_coverage`
- `stable_headK_mix_drift`

可选增强：

- `distinct_group_count`
  当上游能够低成本、稳定地给出 `active_count[m]` 时启用；若缺失则任务仍可正常运行，但会削弱对“低质量、多点扩散 / 扫描式扩张”的敏感度

- `stable_g[i]_share`
  按 `i = 1 .. K_stable_eff` 展开；当 `K_stable_eff >= 1` 时启用

未来增强：

- `new_group_share / other_group_share`
  需要额外的 tail membership 状态或等价高性能机制；`v1` 默认不细化，作为未来增强保留

说明：

- 必选核心已经覆盖 `T3 v1` 首版主干关心的 4 类结构变化：`support_escape`、`head_concentration`、`legacy_head_dilution`、`stable_head_mix_shift`。
- `distinct_group_count` 与 `entropy_shannon` 不重复：前者看活跃广度，后者看质量分布的集中 / 分散程度。
- `stable_g[i]_share` 更偏向固定老牌对象级的解释与归因，不是当前 4 类模式成立的必要前提，因此保留为可选增强更合适。
- `T3 v1` 仍有两个有意识保留的边界：
  1. 若任务不提供 `active_count[m]`，则对“低质量但已明显多点扩散”的早期异常会变得不敏感。
  2. `out_of_support_share` 只回答“有多少质量已经逃离显式支持集”，不区分“从未见过的新 group”与“历史存在但未进入支持集的尾部 group”。

资源与性能估算：

- 对单个 `metric_m`，摘要提取热路径复杂度为 `O(nnz)`；其中 `K_head`、`K_stable` 都是小常数，不改变复杂度阶数。
- 对单个 `metric_m`，路由出的 scalar 数量为：

```text
N_series(metric_m)
= 4
 + I(K_stable_eff >= 1)
 + K_stable_eff
 + I(K_stable_eff >= 2)
 + I(distinct_group_count 启用)
```

其中：

  - `4` 对应 `entropy_shannon`、`top1_share`、`headK_share`、`out_of_support_share`
  - `I(K_stable_eff >= 1)` 对应 `stable_headK_coverage`
  - `K_stable_eff` 对应 `stable_g[i]_share`
  - `I(K_stable_eff >= 2)` 对应 `stable_headK_mix_drift`
  - `I(distinct_group_count 启用)` 对应 `distinct_group_count`

- 对整个 `T3Task`，`N_series(task) = Σ_m N_series(metric_m)`；因此 `K_stable` 是 `T3` 资源消耗最敏感的主参数之一，`v1` 建议默认取 `5`，如需更高吞吐可降到 `3`。

---

## 9. 分型检测器与统一融合

### 9.1 单特征判定

统一原则：

- 所有特征都要输出统一的 `DetectorResult`
- 不要求所有特征都输出相同语义的 `z_score`
- 关系类特征可通过 `normalized_score + reason_code + evidence` 进入统一融合

首版约束：

- `T1a` 采用第 6 节定义的专用规格
- `T1b` 采用第 `6.11` 节定义的差异规格，并遵循与 `T1a` 相同的公共主干
- `T2` 采用第 7 节定义的专用规格，并共享统一输出协议
- `T3 v1` 优先按第 `8.8` 节定义的“关系分布摘要特征层”落地；派生出的标量特征分别按其数学形态路由到 `T1 / T2` 检测器

### 9.2 融合风险分

统一融合层在 `v1` 中拆成两部分：

1. `T1 / T2`：同一 `key` 下的轻量单特征证据融合  
2. `T3`：单特征证据标准化 -> 同源摘要特征模式融合 -> Key 级风险合成  

设计目标：

- 捕捉“多个 `T1 / T2` 特征同时轻微异常，但单个都未单独提级”的协同信号
- 不让单个摘要特征的小幅波动轻易被放大成高强度异常
- 当多个摘要特征对同一种结构变化给出一致证据时，显式提级
- 保持组合提级的语义是“数学证据一致性增强”，而不是业务判别

#### 9.2.1 单特征证据标准化

对任意进入融合层的单特征结果 `DetectorResult_f(t)`，定义：

```text
s_f = normalized_score_f ∈ [0,1]
c_f = confidence_f       ∈ [0,1]
π_f = min(1, persistence_f / N_fuse)
a_f = s_f * c_f * π_f
```

其中：

- `s_f`：检测器标准分
- `c_f`：当前判断可信度
- `persistence_f`：该特征的连续异常窗口数
- `N_fuse`：融合层持续性归一化窗口，建议与全局持续窗口 `N` 保持一致
- `a_f`：融合层使用的有效异常证据强度

方向证据定义为：

```text
a_f^up   = a_f * I(dir_f = up)
a_f^down = a_f * I(dir_f = down)
```

其中 `dir_f` 来自单特征检测器的数学方向：

- `up`：当前值相对基线偏高
- `down`：当前值相对基线偏低

说明：

- 这里的 `up / down` 只表达数学偏移方向，不表达业务好坏
- 未达到最小置信度或持续性门槛的特征，不会完全丢弃，但其证据强度会被 `c_f`、`π_f` 自动压低

#### 9.2.2 同源摘要特征分组

组合提级不是在所有特征之间任意拼接，而是先对同一来源的 `T3` 摘要特征做局部模式融合。

定义同源分组单元：

```text
Bundle_T3(key, feature_base, metric_m, t)
```

其中：

- `key`：被分析主体
- `feature_base`：如 `client_group_mix`
- `metric_m`：如 `bps`、`pps`、`conn_count`

含义：

- 只有来自同一个 `(key, feature_base, metric_m)` 的摘要特征，才进入同一个局部模式融合单元
- 不同 `metric` 先各自形成局部模式分，再在下一层做跨指标合成

#### 9.2.3 模式证据计算通式

对任意模式 `P`，定义：

```text
core_P    = AggCore(required evidences)
support_P = AggSup(optional evidences)
oppose_P  = AggOpp(contradict evidences)

score_P = clip(core_P + λ_sup * support_P - λ_opp * oppose_P, 0, 1)
```

建议默认：

- `AggCore`：对 required 证据取几何平均
- `AggSup`：对 optional 证据取前 `2` 项的算术平均
- `AggOpp`：对冲突证据取最大值
- `λ_sup = 0.5`
- `λ_opp = 0.5`

辅助算子定义：

```text
GeomMean(x_1, ..., x_n)
= (Π_i x_i)^(1/n)

Top2Mean(x_1, ..., x_n)
= mean(按值从大到小排序后的前 2 项非零元素)
```

边界规则：

- `GeomMean` 若任一 required 项为 `0`，则结果为 `0`
- `Top2Mean` 若只有 `1` 个正项，则退化为该单项值
- `Top2Mean` 若没有正项，则结果为 `0`
- 未创建、当前 bucket 为 gap、或当前方向不匹配的证据项，视为 absent；它们不进入 `AggSup / AggOpp`，而 `AggCore` 的 required 项若 absent，则按 `0` 处理

设计意图：

- `core_P` 保证模式必须具备核心证据，不能只靠辅助项堆出来
- `support_P` 让多个侧面一致时获得显式提级
- `oppose_P` 用于抑制与该模式明显不一致的证据
- `oppose_P` 是 `v1` 的正式机制，但只接入“数学上确实构成反证”的少量特征，避免把普通弱波动误当成冲突证据
- `λ_opp` 在 `v1` 中保持单一全局常量，不扩展为 pattern 级调参面，用来控制“支持证据提级”和“反证证据抑制”之间的平衡

#### 9.2.4 `T3 v1` 局部模式库

首版建议定义以下 `4` 类数学模式。

**模式 A：`support_escape`**

语义：

- 当前质量开始显著逃离历史显式支持集，并伴随整体分布扩散或历史头部受挤压

证据定义：

```text
core = a_out_of_support_share^up
support = Top2Mean(
  a_entropy_shannon^up,
  a_distinct_group_count^up,
  a_stable_headK_coverage^down
)
oppose = Top2Mean(
  a_top1_share^up,
  a_headK_share^up,
  a_entropy_shannon^down
)
score_support_escape
= clip(core + 0.5 * support - 0.5 * oppose, 0, 1)
```

解释：

- 该模式对应“支持集外质量显著抬升”
- 若当前更像头部集中接管，而不是支持集外扩散，则 `oppose` 会压低该模式分

**模式 B：`head_concentration`**

语义：

- 流量开始显著集中到单一或少数 group

证据定义：

```text
core = a_top1_share^up
support = Top2Mean(
  a_headK_share^up,
  a_entropy_shannon^down
)
oppose = Top2Mean(
  a_out_of_support_share^up,
  a_entropy_shannon^up
)
score_head_concentration
= clip(core + 0.5 * support - 0.5 * oppose, 0, 1)
```

解释：

- 该模式对应“越来越由少数来源主导”
- 它不区分这种集中是业务正常还是异常，只表达数学上的集中增强
- 若当前更像支持集外扩散，而不是集中增强，则 `oppose` 会压低该模式分

**模式 C：`legacy_head_dilution`**

语义：

- 历史稳定头部整体失去份额，不再主导当前结构

证据定义：

```text
core = a_stable_headK_coverage^down
support = Top2Mean(
  a_out_of_support_share^up,
  a_entropy_shannon^up
)
oppose = a_stable_headK_coverage^up
score_legacy_head_dilution
= clip(core + 0.5 * support - 0.5 * oppose, 0, 1)
```

解释：

- 它回答的不是“固定头部内部怎么变”
- 而是“历史固定头部集合整体是否正在被支持集外结构分流”

**模式 D：`stable_head_mix_shift`**

语义：

 - 历史固定头部集合整体仍在场，但其内部比例关系发生了显著重排

证据定义：

```text
core = a_stable_headK_mix_drift^up
support = 0
oppose = Top2Mean(
  a_stable_headK_coverage^down,
  a_out_of_support_share^up
)
score_stable_head_mix_shift
= clip(core + 0.5 * support - 0.5 * oppose, 0, 1)
```

解释：

- `stable_headK_mix_drift` 直接回答“固定历史头部集合内部比例偏了多少”
- `oppose` 则负责识别“这已经不是内部关系变化，而是固定头部整体正在失守”的情形

#### 9.2.5 跨指标模式合成

同一种结构模式可能在多个 `metric` 上同时出现，例如：

- `conn_count`
- `bps`
- `pps`

因此，对同一 `(key, feature_base, pattern)`，再做一次跨指标合成：

```text
M_valid(P, t)
= { m | score_P^(m) 在当前 bucket 可计算 }

ScorePattern(key, feature_base, P, t)
= 1 - Π_{m ∈ M_valid(P,t)} (1 - score_P^(m))
```

其中：

- `score_P^(m)`：局部模式 `P` 在 `metric_m` 上的模式分
- `M_valid(P, t)`：当前 bucket 上该模式可计算的有效指标集合；只有局部模式分真实可得的 `metric` 才参与跨指标合成

设计意图：

- 若同一结构变化同时反映在 `conn_count` 和 `bps` 上，应比只在单一指标命中时更值得提级
- 若只有某个单一指标轻微命中，则不会被过度放大
- 若某个 `metric` 当前缺测、支撑不足或局部模式不可计算，则它不参与 `M_valid(P, t)`，而不是额外引入新的缺测惩罚

边界规则：

- 若 `|M_valid(P, t)| = 0`，则当前 bucket 不产生该模式的跨指标证据
- 若 `|M_valid(P, t)| = 1`，则 `ScorePattern` 退化为该单指标上的 `score_P^(m)`
- 若 `|M_valid(P, t)| >= 2`，则按饱和型方式完成多指标提级

#### 9.2.6 Key 级统一风险合成

先定义同一 `key` 下 `T1 / T2` 的轻量融合风险：

```text
F_T1T2(Key,t)
= { f | f 属于当前 key 的 T1 / T2 特征，且当前 bucket 的 a_f 可计算 }

Risk_T1T2(Key,t)
= 1 - Π_{f ∈ F_T1T2(Key,t)} (1 - w_f * a_f)
```

其中：

- `a_f` 沿用第 `9.2.1` 节定义的单特征有效异常证据强度
- `w_f`：单特征权重；`v1` 对 `T1 / T2` 默认统一取 `1`
- 该层只表达同一 `key` 下多特征数学证据的一致性增强，不承担业务告警判别

边界规则：

- 若 `|F_T1T2(Key,t)| = 0`，则 `Risk_T1T2(Key,t) = 0`
- 若 `|F_T1T2(Key,t)| = 1`，则退化为该单特征的 `w_f * a_f`
- 该层不区分“正向指标”与“负向指标”的业务含义，只融合检测器已经输出的数学异常证据

再定义 `T3` 单特征基础风险：

```text
Risk_single_T3(Key,t)
= 1 - Π_f (1 - w_f * a_f)
```

再定义模式风险：

```text
Risk_pattern(Key,t)
= 1 - Π_P (1 - λ_P * ScorePattern(Key, feature_base, P, t))
```

`T3` 子风险定义为：

```text
Risk_T3(Key,t)
= 1 - (1 - Risk_single_T3(Key,t)) * (1 - Risk_pattern(Key,t))
```

最终统一风险为：

```text
Risk(Key,t)
= 1 - (1 - Risk_T1T2(Key,t)) * (1 - Risk_T3(Key,t))
```

其中：

- `w_f`：单特征权重；`T1 / T2 / T3` 在 `v1` 首版都固定为 `1`
- `λ_P`：模式权重；`T3 v1` 首版建议收口为“两档权重”

`T3 v1` 的默认权重口径：

- `w_f = 1`：对 `T3` 摘要特征默认不再区分单特征家族权重，先让差异主要由单特征检测器自身的 `normalized_score / confidence / persistence` 表达
- `λ_P`：保留为模式层主参数，但 `v1` 只保留两档：
  - 基础形状模式：`support_escape`、`head_concentration` 统一取 `0.7`
  - 历史头部敏感模式：`legacy_head_dilution`、`stable_head_mix_shift` 统一取 `0.85`

设计含义：

- 若当前 key 上只有 `T1 / T2` 异常证据，则 `Risk(Key,t)` 退化为 `Risk_T1T2(Key,t)`
- 若当前 key 上只有 `T3` 异常证据，则 `Risk(Key,t)` 退化为 `Risk_T3(Key,t)`
- 若多个模式命中，风险会按饱和型方式上升，而不是线性无上限叠加
- 这样既能提级，又避免“特征越多分越高”的机械堆叠
- 两档 `λ_P` 已足够表达“普通结构变化”和“历史头部结构变化”的强弱差异，不再为 `v1` 引入 `4` 档独立调参面

#### 9.2.7 输出证据收口

融合层除输出最终 `Risk(Key,t)` 外，`v1` 建议把解释输出收口为：

```text
FusionResult = {
  key,
  ts,
  risk,
  dominant_single[<=3],
  dominant_pattern[<=2]
}
```

字段说明：

- `risk`：融合后的最终数学风险分，对应 `Risk(Key,t)`
- `dominant_single`：当前风险贡献最高的最多 `3` 个单特征证据
- `dominant_pattern`：当前风险贡献最高的最多 `2` 个模式证据

单特征证据建议结构：

```text
dominant_single[i] = {
  feature,
  dir,
  reason_code,
  a_f,
  normalized_score,
  confidence,
  persistence
}
```

模式证据建议结构：

```text
dominant_pattern[j] = {
  pattern,
  feature_base,
  score_pattern,
  metrics_hit,
  supporting_features
}
```

其中：

- `pattern`：模式名，如 `support_escape`、`stable_head_mix_shift`
- `feature_base`：该模式对应的关系分布基础身份，如 `client_group_mix`
- `score_pattern`：该模式在当前 bucket 的模式分
- `metrics_hit`：当前真正参与该模式跨指标合成的指标集合，即 `M_valid(P, t)` 的命中部分
- `supporting_features`：支撑该模式的核心摘要特征名列表，首版只保留少量高价值项，不回传整份底层 `evidence`

协议边界：

- `FusionResult` 是对同一 `key`、同一 `ts` 上一组 `DetectorResult` 的融合汇总结果，不是并行的第二套单特征输出协议
- `dominant_single` 只是源 `DetectorResult` 的轻量投影，字段必须是其子集；实现上应能通过 `(key, feature, ts)` 回溯到原始 `DetectorResult`
- `dominant_pattern` 是融合层派生出来的结构模式证据，不回写为 `DetectorResult`，也不复用单特征层的 `reason_code` 命名体系
- `FusionResult` 在 `v1` 中只输出连续值 `risk`，不再额外定义融合层 `severity`；若未来需要分档，应新增独立字段 `risk_level`，而不是复用 `DetectorResult.severity`

设计意图：

- 上层需要知道“是谁触发了风险、是什么模式、落在哪些指标上”，而不是拿到一份重复拼装的巨型解释对象
- 因此，`v1` 不再单独输出“模式标签集合”；模式标签已由 `dominant_pattern[*].pattern` 自然表达
- 底层检测器的完整 `evidence` 仍保留在各自 `DetectorResult` 中，但融合层对外只回传最小必要解释

#### 9.2.8 边界与非目标

- 组合提级只表达“多个数学证据是否一致支持某种结构变化”
- 它不负责判断该变化是否是攻击、上线、割接或其他业务事件
- 若上游存在强规则、图算法或外部专家证据，可在本层之后继续参与更高层融合，但不改变本层的数学语义

---

## 10. 在线执行、漂移证据、shadow baseline 与正式重建

### 10.1 在线路径

```
流量/日志输入
   -> 1 分钟聚合（按 Key 分层）
   -> 特征计算（按 FeatureType 产出）
   -> 在线评分（按类型路由到对应检测器）
   -> 风险融合（统一引擎）
   -> 结果输出 / 回放评估
```

在线路径约束：

- 只消费当前观测与已加载模型
- 只维护轻量在线状态，如残差平滑、漂移证据、冷启动状态、临时基线状态
- 不保存可重放的历史观测明细
- 不调用 `history_reader`

### 10.2 异步正式重建路径

当在线路径怀疑旧基线失配时，先由临时在线基线托底，再异步发起正式重建。第 `6.10` 节和第 `7.7` 节只定义各特征类型如何产生“正式重建候选”；统一慢路径由本节定义。

#### 10.2.1 触发、术语与重建请求

推荐流程：

其中：

- `shadow baseline`（影子基线）：正式模型尚未更新前的临时在线基线，用于在旧基线失配后先维持在线评分稳定
- `incumbent model`：当前正在对外提供评分的模型；若 `shadow baseline` 已接管，则 `incumbent = shadow baseline`，否则 `incumbent` 为当前正式模型
- `candidate model`：基于 `HistoryReader` 返回的历史观测训练得到、尚未通过切换验证的新模型

```text
shift_confirmed
  -> 估计 τ_hat
  -> shadow baseline 临时接管在线评分
  -> 创建 RebuildRequest
  -> 调用 HistoryReader.fetch(...)
  -> 训练 candidate model
  -> candidate vs incumbent 验证
  -> 通过则切换到 full model
```

其中：

- `τ_hat`：本次持续漂移的估计起点（estimated regime start）
- `v1` 默认正式重建不依赖 `changepoint`，而是优先把训练窗口裁剪到 `τ_hat` 之后，只用新阶段历史重建正式模型
- 若 `τ_hat` 之后的历史不足以支撑全部组件，则按各组件启用条件只重建当前有支撑的组件，不强行补齐

统一正式重建请求建议定义为：

```text
RebuildRequest = {
  key,
  feature,
  rebuild_reason,
  bucket_start_hint,
  bucket_end
}
```

其中：

- `rebuild_reason`：如 `shift_confirmed`、`scheduled_refresh`
- `bucket_start_hint`：建议正式重建起点；对漂移触发重建，默认取 `τ_hat`
- `bucket_end`：本次正式重建希望覆盖的末尾 bucket
- 对 `T1 / T2`，`feature` 表示普通特征身份；对 `T3`，`feature` 表示任务引用，工程实现上应能唯一定位到对应的 `T3TaskSpec`

默认建议的 `τ_hat` 估计口径为：

```text
τ_hat = t_confirm - c_t + 1
```

其中：

- `t_confirm`：本次确认旧基线失配的当前 bucket
- `c_t`：漂移证据状态中的当前方向连续确认计数

默认正式重建口径：

```text
Ω_rebuild = { t | bucket_id ∈ [τ_hat, bucket_end] }
```

说明：

- `Ω_rebuild` 只覆盖当前怀疑已进入新阶段的历史，不主动把变化前的旧阶段样本混入正式训练
- `Core`、`monthpos`、`event` 仍按各自启用条件决定是否参与本次重建
- 若 `Ω_rebuild` 太短，无法支撑正式模型，则保留 `shadow baseline` 或现有正式基线，等待后续历史积累

#### 10.2.2 `shadow baseline` 桥接层

`shadow baseline` 的激活与退出规则：

- 激活条件：当前不存在已激活的 `shadow baseline`，且本级或可用基线来源存在可参考服务模型，同时在线漂移证据已满足 `shift_confirmed`
- `v1` 工程约束：为避免“刚确认漂移就触发重建，但新阶段 replay 长度还不足以完成当前简化 `holdout` 验证”，`shadow baseline` 与对应重建请求的实际激活还要满足最小可验证窗口约束；当前实现收口为连续确认点数 `c_t >= 3`
- 激活粒度：按单特征序列 `Series = (key, feature)` 激活；对 `T3`，作用对象是 routed 后的摘要特征序列，而不是整个原始 `T3Block`
- 主要退出条件：正式基线重建完成并成功切换。也就是说，`shadow baseline` 的主要职责就是在“旧基线已失配”与“新正式基线可用”之间提供一个临时桥接
- 保护性退出条件：连续 gap 超过 `G_reset`，或其依附的参考模型 / 来源模型已失效
- 非退出条件：`history_reader` 暂时不可用、重建历史不足、重建任务失败，或短时间内表面上“看起来恢复正常”，都不应直接撤销 `shadow baseline`

补充说明：

- 上述 `c_t >= 3` 属于当前 `v1` 为闭合“`shadow` 接管 -> 正式重建 -> `holdout` 验证 -> 正式切换”链路而增加的工程保护，不改变 `shift_confirmed` 本身的算法定义
- 后续若正式切换验证支持更短的新阶段窗口，或引入更合适的短窗验证 / replay 策略，可再放宽这条实现约束

`shadow baseline` 的最小正式规格：

```text
ShadowState = {
  active,
  ref_model_id,
  delta,
  last_bucket
}
```

字段说明：

- `active`：当前序列是否由 `shadow baseline` 接管
- `ref_model_id`：激活时冻结的参考服务模型；可以是本级正式模型，也可以是当前选中的可用基线来源模型
- `delta`：参考模型之上的在线偏移量；这是 `shadow baseline` 的唯一核心自适应状态
- `last_bucket`：最近一次处理的有效 `bucket_id`

工程收口与分期说明：

- `ref_model_id` 是算法语义名，表示“激活时冻结的参考服务模型”；它不要求工程上先建立全局 `model registry`，再来实现 `shadow baseline`
- `Story 18.12` 允许先采用显式冻结引用模型的工程表示：

```text
ShadowState_v1 = {
  active,
  ref_kind,          // self_formal | self_candidate | source_formal | source_candidate
  ref_source_key?,
  ref_model_version,
  frozen_ref_model,
  delta,
  last_bucket
}
```

- 其中 `frozen_ref_model` 表示激活时刻冻结的引用模型句柄；`ref_source_key` 仅在来源模型下填写
- `Story 18.12A` 再把这套工程表示正式收口为代码契约与测试要求，不再保留“算法语义里是 `ref_model_id`、工程实现里靠临时约定承载”的中间状态

设计约束：

- `shadow baseline` 不重建 `Core / monthpos / event / support_policy`
- `shadow baseline` 不保存历史窗口，也不维护独立的季节项或方差层
- 它只是在冻结参考模型预测的基础上，增加一个在线可更新的偏移量 `delta`

预测定义：

- 对 `T1a / T1b`，令 `x_t = Transform(y_t; φ_model)`，`μ_ref,t` 为冻结参考模型在 `t` 时刻的变换空间预测，则：

```text
μ_shadow,t^- = μ_ref,t + delta_{t-1}
```

- 对 `T2`，令 `x_t = logit(p_t^smooth)`，`μ_ref,t` 为冻结参考模型在 `logit` 空间的预测，则：

```text
μ_shadow,t^- = μ_ref,t + delta_{t-1}
p_shadow_hat,t = sigmoid(μ_shadow,t^-)
```

- 对 `T3`，`shadow baseline` 只作用于 routed 后的摘要特征序列；其预测公式沿用该摘要特征所路由到的 `T1 / T2` 形式

激活初始化：

- 若激活时当前 bucket 有效，则：

```text
delta_0 = x_t_confirm - μ_ref,t_confirm
```

- 若激活时当前 bucket 无有效观测，则 `delta_0 = 0`

在线更新：

- 当当前 bucket 满足原特征类型自身的 `gate_score` 且不是 gap 时，`shadow baseline` 执行：

```text
delta_t
= (1 - α) * delta_{t-1}
 + α * (x_t - μ_ref,t)
```

- 这里直接复用全局平滑系数 `α`，不再为 `shadow baseline` 单独引入新的更新参数
- 当当前 bucket 为 gap 或不满足该特征类型自身的评分 gate 时，`delta` 冻结不更新
- 当连续 gap 超过 `G_reset` 时，`ShadowState` 直接重置

评分接法：

- `shadow baseline` 激活后，点异常评分中的预测值由正式模型预测替换为 `μ_shadow,t^-`
- `T1a / T1b` 不单独维护新的尺度状态，但在 `shadow baseline` 模式下，评分使用放宽后的有效尺度：

```text
sigma_shadow = k_shadow_sigma * sigma_ref
```

分阶段实现约束：

- 设计目标上，`T1a / T1b` 的 `shadow baseline` 应按 `sigma_shadow = k_shadow_sigma * sigma_ref` 进行标准化评分
- 但考虑当前 `T1` 主评分链尚未实现 `sigma_ref`，`Story 18.12` 先允许采用最小桥接口径：

```text
raw_score_shadow_t1_v1 = |x_t - μ_shadow,t^-| / k_shadow_sigma
```

- 这个口径只表达“桥接期分数放宽”，不等价于最终标准化残差，也不应作为长期正式语义
- `Story 18.12A` 必须补齐 `sigma_ref` 的训练、持久化、predictor 输出与 `T1` 标准化评分，并把 `shadow` 评分切换为：

```text
z_shadow,t = (x_t - μ_shadow,t^-) / sigma_shadow
```

- `T2` 继续复用原有 `phi_over / v_floor` 方差层，但在 `shadow baseline` 模式下，评分使用放宽后的有效方差：

```text
Var_shadow_eff_t
= k_shadow_sigma^2 * Var_eff_t
```

- 上述放宽只改变桥接期的评分尺度，不新增独立的 `sigma_shadow` 在线状态
- `shadow baseline` 不改变原有 `reason_code` 体系；它只改变“当前拿什么基线去解释观测”
- `shadow baseline` 激活时，最终 `confidence` 建议做上限裁剪：

```text
confidence = min(confidence_base, c_shadow_max)
```

其中 `c_shadow_max` 用于表达“当前结果可用，但仍处于临时桥接模式”；`v1` 建议 `0.8`
- 对上层显式暴露 `shadow` 状态，但不修改统一 `DetectorResult` 主结构；建议在 `evidence` 中补充：
  - `shadow_active = true | false`
  - `baseline_provider = formal | shadow | source`

实现含义：

- `shadow baseline` 是一个“冻结参考模型 + 单偏移量自适应”的轻量桥接层，而不是第二套正式模型
- 它的状态量是 `O(1)`，可用于高基数分钟级在线路径
- 在后续 `candidate vs incumbent` 验证中，若 `incumbent = shadow baseline`，就按这里定义的同一套规则进行单遍 replay 与 prequential 计损

#### 10.2.3 候选验证与正式切换

切换验证建议采用“尾部保留验证（holdout tail validation）”：

```text
N_val_switch = 16

Ω_val = Ω_rebuild 中最后 N_val_switch 个有效 bucket
Ω_fit = Ω_rebuild \ Ω_val
```

约束：

- `candidate model` 只使用 `Ω_fit` 训练，避免把验证尾段样本泄漏到候选模型中
- 若 `Ω_rebuild` 的有效 bucket 数少于 `2 * N_val_switch`，则本次不做切换验证，继续保留 `incumbent model`
- `Ω_val` 中的“有效 bucket”定义沿用各特征类型自身的 gate 规则；gap 和无效 bucket 不计入尾段长度

对 `T3`，切换验证必须在共同摘要基准上进行。定义：

```text
EvalBasis_T3(m)
= incumbent 所对应的 ServiceBasis_T3(m)
```

其中：

- 若 `incumbent` 为当前正式 `T3` 模型，则直接取其 `ServiceBasis_T3(m)`
- 若 `incumbent = shadow baseline`，则取其冻结参考模型 `ref_model_id` 所对应的最近正式 `ServiceBasis_T3(m)`
- `EvalBasis_T3(m)` 在单次重建与验证过程中冻结不变

同时，对 `candidate` 区分两个视图：

```text
CandidateServiceBasis_T3(m)
= 基于 Ω_fit 新派生的 service basis

CandidateEvalBasis_T3(m)
= EvalBasis_T3(m)
```

含义：

- `candidate service model`：使用 `CandidateServiceBasis_T3(m)` 训练；这是切换成功后真正对外服务的新模型
- `candidate eval model`：使用 `EvalBasis_T3(m)` 在同一批 `Ω_fit` 观测上重建摘要特征并训练；它只用于与 `incumbent` 公平比较
- 对 `T1 / T2`，不区分两套视图；可视为 `candidate_eval = candidate model`

`EvalBasis_T3(m)` 的变化规则：

- 普通 basis 刷新：当 `group_space_id` 不变且 `group_space_version` 兼容时，`EvalBasis_T3(m)` 只在“正式切换成功”后，随新的 `ServiceBasis_T3(m)` 一并更新
- 不兼容 basis 变化：当 `group_space_id` 改变，或 `group_space_version` 明确表示 group 划分规则 / `group_id` 语义已变时，旧 `EvalBasis_T3(m)` 与新任务不再可比；此时应将新的 `T3` 任务视为新 lineage，停止直接做 `candidate vs incumbent` 的 `T3` 任务级比较

验证损失统一定义为：

```text
L_val(model)
= Σ_{t ∈ Ω_val_valid} w_val(t) * Huber(x_t - μ_model,t)
```

其中：

- 对 `T1a / T1b`，`w_val(t) = 1`
- 对 `T2`，`w_val(t)` 复用分母支撑度权重 `denominator_t / (denominator_t + d_min_train(feature))`
- 对 `T3` 任务，先在共同 `EvalBasis_T3` 上重建各启用摘要特征，再对其 routed `T1 / T2` 验证损失取简单平均作为 `L_val(task)`

比较对象定义：

- `candidate model`：冻结参数，在 `Ω_val` 上只做预测不更新
- `incumbent model`：当前服务模型
- 对 `T3`：
  - `candidate service model`：基于 `CandidateServiceBasis_T3` 训练，只用于切换后服务
  - `candidate eval model`：基于 `EvalBasis_T3` 训练，只用于验证比较
  - `incumbent model`：始终按 `EvalBasis_T3` 对应的现服务 basis 计损
- 若 `incumbent = shadow baseline`，则采用同一套轻量影子规则从 `Ω_fit` 起点做一次单遍 replay；在 `Ω_val` 上按“先预测、后更新”的 prequential 方式计损，避免使用当前 bucket 标签泄漏
- 若 `incumbent` 为当前正式模型，则直接冻结该模型并在 `Ω_val` 上计损

切换通过条件建议定义为：

```text
candidate_pass
= I(candidate_core_status = ok)
   * I(L_val(candidate_eval) <= (1 + eps_switch) * L_val(incumbent))
```

其中：

- `candidate_core_status = ok` 表示候选模型至少具备可服务的 `Core`
- `eps_switch`：候选模型相对现服务模型允许的验证损失放宽系数，`v1` 建议 `0.05`
- 若 `T3` 任务发生 `group_space_id / group_space_version` 不兼容变化，则 `candidate_eval` 不再构造；该任务按新 lineage 处理，不做直接 incumbent 比较

切换规则：

- 若 `candidate_pass = 1`，则基于完整 `Ω_rebuild` 再训练一次最终 `full model`；可使用已通过验证的 `candidate service model` 作为 warm start
- `full model` 训练完成后原子切换对外服务模型
- 若 `candidate_pass = 0`，则保留 `incumbent model`，等待更多新阶段历史或下次计划重建
- 若当前不存在 `incumbent model`，则只要 `candidate_core_status = ok`，即可跳过比较直接切换
- 若 `T3` 任务被判定为新 lineage，则旧 basis 不再作为 `EvalBasis` 参与新任务验证；新任务按冷启动 / 首次建模流程建立自己的 `ServiceBasis_T3`

#### 10.2.4 历史读取、回退与计划重建

对 `HistoryReader.fetch(...)` 的使用约束：

- 只在异步正式重建、回放验证等慢路径调用
- 只按当前 `(key, feature)` 拉取历史观测
- 正式重建所需历史数据由外部历史源提供，基线层不缓存可重放数据

回退行为：

- 若 `history_reader` 未注入，则跳过正式重建，保留当前在线基线并发出 `rebuild_blocked` 内部状态；`rebuild_blocked` 表示“当前任务缺少正式重建所需的历史读取能力”
- 若 `HistoryFetchResult.status = insufficient_data`，则本次正式重建不执行，继续使用在线临时基线或现有正式基线
- 若 `HistoryFetchResult.status = unavailable`，则本次正式重建失败，后续按计划任务或下次触发条件重试

离线每日（或触发式）：

```
历史窗口（由 HistoryReader 提供）
  -> 分类型模型正式重建 / 更新基线
  -> 参数下发
```

实时触发正式重建条件：

1. 旧基线失配证据达到确认阈值；  
2. 残差均值长期偏移；  
3. 固定计划正式重建（如每日）。

---

## 11. 工程约束

### 11.1 高基数控制

- L3（关系对）仅保留 Top-K 活跃关系；
- 长尾关系以 L2 分组特征承接；
- 超高基数计数使用 HLL / Sketch。

### 11.2 冷启动策略

- 冷启动统一遵循第 `2.3` 节的 `基线来源` 机制；各类型若有额外约束，以专章定义为准，其中 `T2` 的补充规则见第 `7.8` 节。
- 若存在 ready 的可用来源，可临时借用其基线输出；若不存在，则仅观察并继续积累本级训练数据。
- 本级基线一旦进入 `core_no_month_ready` 或 `full_ready`，即切回独立基线，`v1` 不做多来源混合。
- 冷启动窗口默认抑制高强度异常输出，保留观察结果。

### 11.3 缺失值策略

不做插值，仅在有观测点时更新。

### 11.4 历史数据与正式重建约束

- 任务级历史数据接口契约见第 `5.3` 节，统一正式重建慢路径见第 `10.2` 节。
- 基线层默认只保存模型状态、轻量在线状态和必要元数据，不保存可重放的历史观测。
- `history_reader` 的缺失或失败，不得影响在线评分热路径；其影响只限于正式重建、回放验证等慢路径能力。
- `history_reader` 返回的数据协议必须与在线 `Observation` 保持一致，避免为训练路径单独维护另一套输入语义。

### 11.5 算法代码注释约束

- 涉及基线算法的代码，必须提供清晰注释；这里的“基线算法代码”包括但不限于评分逻辑、漂移证据累积器、`shadow baseline`、正式重建、候选验证与切换、`T3` 摘要提取与 basis / support / stable head 相关逻辑。
- 注释优先解释“为什么这样做”和“对应哪一层算法语义”，而不是机械复述代码字面含义。
- 对状态迁移、重建触发条件、`shadow baseline` 接管 / 退出、`T3` basis 切换等非直观逻辑，必须在代码块前给出短而明确的说明。
- 当实现直接对应本文中的某个概念、状态或公式时，注释应尽量沿用本文术语，便于设计与实现对照。

---

## 12. 参数目录（首版建议）

本章将参数分为 3 层：

1. 主参数：真正决定算法语义，值得在设计与标定阶段讨论  
2. 派生参数：由主参数按固定规则计算，不单独调参  
3. 实现常量：只影响求解器或数值稳定性，不作为业务侧调参项  

### 12.1 主参数

| 参数 | 含义 | 作用域 | 建议初值 |
|---|---|---|---|
| 粒度 | 统计窗口 | 全局 | `1 min` |
| 持续窗口 `N` | 关系异常连续命中窗口 | 全局 / 融合层 | `2` |
| `L3 Top-K` | 每实体保留关系对数量 | 全局 | `100 ~ 1000`（按算力） |
| 正式重建周期 | 定时正式重建 | 全局 | 每日 `1` 次（可叠加触发式） |
| `transform_name` | `T1a` 默认输入变换 | T1a | `log1p` |
| `K_day` | 日周期谐波数 | T1 / Core | `4` |
| `K_week` | 周周期谐波数 | T1 / Core | `3` |
| `DME_max` | `days_to_month_end` 截断上限 | T1 / monthpos | `7` |
| `M_month_enable` | 月位置层启用最小自然月数 | T1 / monthpos | `4` |
| `month_cov_min` | 月位置层启用最小有效覆盖率 | T1 / monthpos | `80%` |
| `K_support` | `T3` 显式支持集最大规模 | T3 / support_policy | `16` |
| `min_hist_share` | 进入 `SupportExplicit_T3` 的最小历史累计占比 | T3 / support_policy | `0.5%` |
| `min_active_ratio` | 进入 `SupportExplicit_T3 / StableHeadSet_T3` 的最小活跃覆盖率 | T3 / support_policy | `20%` |
| `K_head` | `headK_share` 的头部规模 | T3 / summary_policy | `5` |
| `K_stable` | `StableHeadSet_T3` 的目标规模 | T3 / summary_policy | `5` |
| `z_warn` | 单窗口异常起点 | T1 / T2 共享 | `3.0` |
| `z_crit` | 单窗口强异常阈值 | T1 / T2 共享 | `5.0` |
| `shift_clip` | 漂移证据输入截断上限 | T1 / T2 共享 | `6.0` |
| `α` | EWMA 平滑系数 | T1 / T2 共享 | `0.2` |
| `λ_mem` | 漂移证据记忆衰减系数 | T1 / T2 共享 | `0.9` |
| `κ_shift` | 漂移死区 | T1 / T2 共享 | `0.25` |
| `u_min` | 连续确认最小平滑偏移 | T1 / T2 共享 | `0.5` |
| `H_shift` | 漂移证据饱和阈值 | T1 / T2 共享 | `3.0` |
| `p_shift_low` | 低漂移置信度阈值 | T1 / T2 共享 | `0.3` |
| `p_shift_high` | 高漂移置信度阈值 | T1 / T2 共享 | `0.6` |
| `M_shift` | 低阈值持续窗口数 | T1 / T2 共享 | `3` |
| `G_skip` | 漂移证据冻结缺失阈值 | T1 / T2 共享 | `3` |
| `G_reset` | 漂移证据重置缺失阈值 | T1 / T2 共享 | `12` |
| `w_shift` | 漂移分数权重 | T1 / T2 共享 | `0.8` |
| `n_train_min(profile)` | `T1b` 最小训练样本数 | T1b / profile | `cont_core: 50`；`cont_tail: 100` |
| `s_prior(profile)` | `T2` 比例平滑先验总强度 | T2 / profile | `rate_core: 2`；`ratio_bursty: 4` |
| `d_min_train(profile)` | `T2` 最小训练分母 | T2 / profile | `rate_core: 50`；`ratio_bursty: 100` |
| `phi_over(profile)` | `T2` 过度离散修正系数 | T2 / profile | `rate_core: 1.5`；`ratio_bursty: 2.0` |
| `λ_P(pattern)` | Key 级模式风险权重 | T3 / 融合层 | 基础形状模式：`0.7`；历史头部敏感模式：`0.85` |

### 12.2 派生参数

| 参数 | 派生规则 | 说明 |
|---|---|---|
| `n_score_min(feature)` | `ceil(0.5 * n_train_min(feature))` | `T1b` 最小评分样本数 |
| `n_shift_min(feature)` | `2 * n_train_min(feature)` | `T1b` 最小漂移更新样本数 |
| `kappa_sample(feature)` | `n_train_min(feature)` | `T1b` 样本量置信度修正强度 |
| `d_score_min(feature)` | `ceil(0.5 * d_min_train(feature))` | `T2` 最小评分分母 |
| `d_shift_min(feature)` | `2 * d_min_train(feature)` | `T2` 最小漂移更新分母 |
| `kappa_den(feature)` | `d_min_train(feature)` | `T2` 分母置信度修正强度 |
| `m0(feature)` | `clip(Σ numerator / Σ denominator, m_floor, 1 - m_floor)` | `T2` 当前训练窗口静态先验中心 |
| `alpha0(feature)` | `s_prior(feature) * m0(feature)` | `T2` 分子伪计数 |
| `beta0(feature)` | `s_prior(feature) * (1 - m0(feature))` | `T2` 分母补偿伪计数 |
| `N_fuse` | `N` | 融合层持续性归一化窗口 |

### 12.3 实现常量

| 参数 | 含义 | 建议初值 |
|---|---|---|
| `solver_name` | 统一块求解器名称 | `weighted_huber_ridge_irls` |
| `c_huber` | Huber 截断尺度系数 | `1.5` |
| `s_min_fit` | 拟合阶段最小鲁棒尺度下限 | `1e-3` |
| `max_iter_fit` | IRLS 最大迭代轮数 | `15` |
| `tol_obj_rel` | 目标函数相对收敛阈值 | `1e-4` |
| `tol_beta_inf` | 系数无穷范数收敛阈值 | `1e-5` |
| `cond_max` | 线性系统条件数上限 | `1e8` |
| `N_val_switch` | 重建切换验证尾段长度 | `16` |
| `eps_switch` | 候选模型相对现服务模型的验证损失放宽系数 | `0.05` |
| `c_shadow_max` | `shadow baseline` 模式下的置信度上限 | `0.8` |
| `k_shadow_sigma` | `shadow baseline` 模式下的评分尺度放宽系数 | `1.5` |
| `λ_season` | 日 / 周季节项正则强度 | `1.0` |
| `λ_dom` | `day_of_month` 正则强度 | `4.0` |
| `λ_dme` | `days_to_month_end` 正则强度 | `2.0` |
| `λ_lwd` | `last_weekday_of_month` 正则强度 | `1.0` |
| `λ_event` | 事件项正则强度 | `2.0` |
| `sigma_min` | 残差尺度下限 | `1e-3` |
| `eps_logit` | `logit` 裁剪下限 | `1e-4` |
| `m_floor` | `m0` 裁剪下限 | `1e-4` |
| `v_floor` | `T2` 最小方差下限 | `0.25` |
| `λ_sup` | 模式内支持证据加权系数 | `0.5` |
| `λ_opp` | 模式内冲突证据抑制系数 | `0.5` |
| `w_f` | 单特征基础风险权重 | `1.0` |

说明：

- `T1b` 首版建议保留两类 profile：`cont_core` 用于 `avg_rtt`、`avg_latency`、`avg_duration` 等主体连续统计；`cont_tail` 用于 `p95_rtt`、`p99_latency` 等尾部分位连续统计
- `T1b` 的默认变换为 `log1p`；仅在具体特征确有必要时，才通过 `transform_name_override` 覆盖
- 正式模型的默认训练组织方式是“统一超模型 + 层级分阶段拟合”；`joint_recalibration` 已移出 `v1` 活动设计
- 正式重建的默认历史窗口选择是“从 `τ_hat` 开始的后移窗口重建”，而不是在正式模型中内置 `changepoint`
- `core_no_month_ready` 是 `v1` 的正式可用主模型状态；`month_cov_min` 只控制 `monthpos` 启用，不再单独否决 `Core`
- `v1` 默认漂移模块采用固定状态的漂移证据累积器（BOCPD-style），仅保留“持续累积漂移证据”的方法语义，不维护完整 `BOCPD` 后验状态
- `T2` 首版只建议保留 `rate_core`、`ratio_bursty` 两类默认 profile，且默认正式可用形态为 `core_no_month`
- `T3 v1` 已收口为“关系分布摘要特征层 + 模式融合”；主参数保留 `support_policy`、`summary_policy` 与 `λ_P(pattern)`，模式内 `λ_sup / λ_opp` 默认下沉为实现常量
- `T3` 模式库首版保留 `4` 个模式，但 `λ_P(pattern)` 只保留两档，避免“模式保留 4 个，权重也调 4 套”的双重复杂度

---

## 13. 未来增强与参考来源

### 13.2 未来增强备忘：`changepoint`

- 当前正式设计已将 `changepoint` 从 `T1 / T2` 的活动模型结构中剔除
- 当前默认策略是：在线用漂移证据累积器发现旧基线失配，离线正式重建时用 `τ_hat` 裁剪历史，只在新阶段窗口上重训正式模型
- 未来若需要在“长窗口混合历史”场景下更充分利用变化前后的样本，可重新评估把 `changepoint` 作为正式重建慢路径中的增强拟合能力
- 即使未来恢复 `changepoint`，也只建议放在正式重建慢路径，不进入在线评分热路径

### 13.3 参考来源（用于方法对齐）

- Elastic Security ML Jobs / 预置规则：  
  https://www.elastic.co/docs/reference/machine-learning/ootb-ml-jobs-siem  
  https://www.elastic.co/docs/reference/security/prebuilt-rules/rules/ml/ml_rare_destination_country
- Vectra 检测样例：  
  https://www.vectra.ai/detections/suspicious-port-sweep  
  https://www.vectra.ai/detections/suspicious-port-scan
- ExtraHop 检测样例：  
  https://www.extrahop.com/resources/detections/unconventional-outbound-connection  
  https://www.extrahop.com/resources/detections/unconventional-protocol-communication
- SedanSpot：  
  https://www.cs.cmu.edu/afs/cs.cmu.edu/user/deswaran/www/papers/icdm18-sedanspot.pdf  
  https://github.com/dhivyaeswaran/sedanspot
- AnoGraph：  
  https://arxiv.org/pdf/2106.04486.pdf  
  https://github.com/Stream-AD/AnoGraph
- RITA：  
  https://github.com/activecm/rita
