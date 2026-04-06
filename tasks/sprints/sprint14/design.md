# Sprint 14 设计文档：BuiltinRegistry、Stream Hub 与流式 Group DAG

## 背景

Sprint 13 已完成流式控制面与并发基础补齐，但仍有两个核心问题：

1. 内置流式通道和内置算子仍存在硬编码装配逻辑，扩展成本高，且前后端规则不易一致。
2. Stream 通道管理虽可用，但在类型参数、角色约束、链式处理语义上仍不够产品化。

本设计覆盖 Story 14.11、Story 14.12 与 Story 14.13，形成「统一注册加载 + Stream 通道产品化 + 流式 Group DAG 编排」的一体化方案。

---

## 设计目标

1. 以 `BuiltinRegistry` 统一管理内置通道类型与内置算子注册，替代硬编码分支。
2. 让 Stream 通道新增/修改从字符串 `option` 演进为结构化 `options`，并由后端统一校验。
3. 前端“新增 Stream 通道”支持按类型动态渲染参数（含 `spsc/spmc/mpsc/mpmc`、`fanin/fanout` 语义）。
4. 支持链式处理：某条 SQL 的 sink 可作为后续 SQL 的 source。
5. 支持 source 语法自动展开：当 source 指向 `stream_hub(split)` 根通道且未指定子通道时，自动等价 `[*]`。
6. 支持单任务多条流式 SQL 共享同一 source 并发消费，满足“分析 + 存储”双分支场景。

---

## 非目标（Sprint 14）

1. 不实现跨任务动态订阅同一 source（late join）。
2. 不实现广播回放与持久化重放。
3. 不引入兼容双轨（V1/V2）。

---

## 统一方案总览

### 1) BuiltinRegistry（Story 14.11）

引入统一注册中心，承载两类描述符：

1. `StreamChannelTypeDescriptor`
2. `BuiltinOperatorDescriptor`

由描述符驱动：

1. `StreamPlugin` 的通道构建（替代 `if(type==...)`）
2. `CatalogPlugin` 的内置算子注册（替代手写多行 `Register(...)`）
3. `Web` 的通道类型与参数表单渲染（通过类型元数据查询接口）

### 2) Stream Hub 产品化（Story 14.12）

对外统一通道类型为 `stream_hub`，通过 `mode` 统一表达 fanin/fanout 语义：

1. `mode=split`：一入多出（FanOut 语义）
2. `mode=merge`：多入一出（FanIn 语义）

Sprint 14 同时实现 `split` 与 `merge`，并统一落地调度语义、租约语义与管理面可观测字段。

### 3) 流式 Group DAG（Story 14.13）

在单任务多 SQL 场景下，新增 `StreamTaskGroup + DagNodeRuntime + BroadcastHub`：

1. 支持串行链式、同源并发及串并组合；
2. 同源分支可实现 source 一次读取、多分支广播；
3. 通过 DAG 依赖与启动条件实现可控编排；
4. 组级 stop/fail/cancel 一致收敛。

---

## 对现有任务调度的影响评估（重点）

本节回答「14.11/14.12 是否影响现有调度主链路」。

### 影响面分级

1. 低影响（保持不变）：
- `TaskPlugin` 的任务模型、`task_id/runtime_task_id` 生成与状态持久化逻辑不变。
- `StreamRuntime` 线程池、`ShardRunner` 运行模型不变（不新增调度线程、不改 step 语义）。
- 非 stream 任务（batch/dataframe/database）执行路径不变。

2. 中影响（局部改造）：
- `Scheduler` 在 stream source 解析阶段新增 selector 展开（`stream.<hub>`/`[*]`/`[i]`）。
- source 租约键从“原始 source 名”改为“展开后的真实 source 键集合”。
- `ClassifySqlTaskKind` 与 `ExecuteStreamTask` 复用同一 source 解析函数，避免分类与执行语义漂移。

3. 高风险点（重点防护）：
- 自动展开 `[*]` 可能导致 source 数增加（例如 16 分区），影响 fanin 与租约冲突概率。
- `modify/remove` 与 `execute` 并发下若 in-use 只看 root key，存在遗漏风险；必须按展开后的 key 全量判定。

### 受影响代码点（函数级）

1. `src/services/scheduler/scheduler_plugin.cpp`：
- `ClassifySqlTaskKind`：改为调用统一 source 解析入口，不再直接 `FindChannel(source_name)`。
- `ExecuteStreamTask`：改为消费统一解析结果（`resolved_channels/resolved_keys/expand_rule`）。
- `ResolveStreamSink`：增加 `INTO stream.<hub>[*|i]` 拒绝逻辑。
- `TryAcquireStreamTaskLeases`：输入使用“展开后 source_keys”。

2. `src/services/scheduler/scheduler_plugin.h`（新增）：
- `struct ResolvedSourceBinding { std::shared_ptr<IStreamChannel> ch; std::string lease_key; std::string display_ref; };`
- `int ResolveStreamSourceBindings(const SqlStatement& stmt, ResolveStage stage, ...);`

3. `src/services/scheduler/stream_task.h/.cpp`（必须增强）：
- 为 `TaskSnapshot`（single）与 `GroupNodeSnapshot`（group node）统一增加 `resolved_sources/source_expand_rule` 只读观测字段；
- `status/list` 仅从运行态快照读取，不再引入 `SchedulerPlugin` 并行元数据表，避免双写不一致。

### 调度改造边界（不破坏主链路）

1. 新增统一函数（建议）：
- `ResolveStreamSourceBindings(stmt.sources, stage, out_bindings, out_keys, out_expand_rule, err)`
- `stage=classify|execute`，两阶段必须共用同一实现。

2. `ResolveStreamSink(stmt.dest, ...)` 保持现有入口，仅增加 selector 约束：
- `INTO stream.<hub>[*|i]` 直接报错；
- `INTO stream.<hub>` 合法；
- 其他 sink（`dataframe/db`）逻辑不变。

### 并发与性能影响评估

1. 锁模型：
- 维持现有锁：`stream_channel_refs_mu_`（租约/修改互斥）与 `stream_tasks_mu_`（任务表）。
- 约束锁顺序：先解析 source（无锁）-> 再拿 `stream_channel_refs_mu_` 申请租约 -> 最后更新 `stream_tasks_`；禁止反向获取，避免死锁。
- 新增 TOCTOU 防护：`execute` 与 `modify/remove` 必须共享“版本校验 + 引用登记”原子路径，禁止锁外解析结果直接复用。

2. TOCTOU 防护流程（必须）：
- 第 1 阶段（无锁）：解析 SQL，得到候选 `resolved_source_keys/resolved_sink_keys` 与 `channel_version_snapshot`。
- 第 2 阶段（持有 `stream_channel_refs_mu_`）：逐项复核通道仍存在且版本未变，再执行冲突检测并一次性登记引用计数；任一失败则整批回滚。
- 第 3 阶段（释放租约锁后）：持有 `stream_tasks_mu_` 注册运行态任务。
- `modify/remove` 同样在 `stream_channel_refs_mu_` 下完成 `in_use` 判断与版本推进，杜绝检查-使用分离窗口。
- 版本来源：运行态 `StreamChannelRef.version`（与持久化 `stream_channel_store.version` 同步），`modify/remove` 在提交成功后单调递增。

3. 时间复杂度：
- source 展开复杂度 `O(S + P)`（`S` 为 SQL source 数，`P` 为展开后的分区数）。
- 租约冲突检测复杂度 `O(K)`（`K` 为展开 key 数）。

4. 容量护栏（建议）：
- 新增最大展开数限制（例如 `max_resolved_sources=64`，可配置），超过直接拒绝并返回明确错误，防止异常 SQL 放大调度开销。

### 行为变化说明

1. 对现有 SQL（无 selector）：
- 普通 stream 通道：行为不变。
- `stream_hub(split)`：`FROM stream.<hub>` 自动等价 `FROM stream.<hub>[*]`。
- `stream_hub(merge)`：`FROM stream.<hub>` 保持 root 读取，不做 selector 自动展开。

2. 租约语义：
- 同一分区仍独占；
- `[*]` 任务会占用全部分区 key，冲突更严格但语义正确。
- `stream_hub(merge)` 任务按 hub root key 占用租约（不展开子 key）。

3. 可观测性：
- `stream execute/status/list` 增加 `resolved_sources` 与 `source_expand_rule`，便于定位自动展开与冲突来源。

---

## SQL Parser 影响评估与保护方案（重点）

本节回答「SQL 解析核心能力不容有失」的风险控制。

### 风险结论

直接扩大通用标识符词法（把 `[` `]` `*` 全局纳入 `ReadIdentifier`）风险较高，可能影响：

1. 列名/函数参数解析边界；
2. `WHERE/USING/WITH/INTO` 关键字定位；
3. 历史 SQL 的容错行为。

因此采用**最小侵入方案**：只在 source/dest 通道引用位置解析 selector，不改通用词法规则。

### 语法增量（限定在通道引用）

仅新增以下局部语法，不改全局表达式语法：

```ebnf
channel_ref := identifier selector?
selector    := "[" "*" "]" | "[" digits "]"
digits      := "0" | ("1".."9" {"0".."9"})
```

适用位置仅限：

1. `FROM channel_ref`
2. `INTO channel_ref`

### 代码改造策略（最小侵入）

1. 保持 `ReadIdentifier()` 完全不变（不引入 `[]`）。
2. 新增 `ReadChannelRef(std::string* err)`，仅在 `Parse()` 的 `FROM/INTO` 分支调用：
- 先读取基础标识符（例如 `stream.npm_hub`）；
- 再尝试读取可选 selector（`[*]` 或 `[<digits>]`）；
- 若 selector 语法异常，立即返回 parser error。
3. `SELECT` 列解析、`WHERE` 截断逻辑、`USING/THEN/WITH` 解析逻辑保持现状。

### 对核心解析能力的保护约束

1. 不修改 `FindExtensionStart()` 的关键字提取逻辑。
2. 不修改 `ValidateWhereClause()` 的安全检查逻辑。
3. 不新增全局 lexer 状态，不引入 parser 全局开关，不做运行时双轨。
4. parser 只负责语法正确性，不负责语义：
- `stream.hub[99]` 越界在调度阶段报错；
- `INTO stream.hub[0]` 语法可过，语义阶段报错。
- `FROM stream.hub[*]` 若命中 `mode=merge`，语法可过，语义阶段报 `STREAM_HUB_SELECTOR_NOT_ALLOWED_MERGE`。

### 解析阶段错误规则

以下场景统一在 parser 阶段失败：

1. 缺失 `]`：`stream.hub[`
2. 空索引：`stream.hub[]`
3. 非数字索引：`stream.hub[abc]`
4. 负索引：`stream.hub[-1]`
5. 非法尾随字符：`stream.hub[0]x`

### 回归保护（必须新增）

基于现有测试体系在 `src/tests/test_framework/main.cpp` 增补 parser 用例（不新建第二套解析器）：

1. 历史回归：
- 当前 parser 用例全量复跑，确保历史 SQL 的 `sources/dest/sql_part/operators/with_params` 与预期一致。

2. selector 正常用例：
- `FROM stream.hub`
- `FROM stream.hub[*]`
- `FROM stream.hub[0]`
- `FROM stream.hub[0],stream.hub[1] USING ...`

3. selector 非法用例：
- `FROM stream.hub[`
- `FROM stream.hub[]`
- `FROM stream.hub[-1]`
- `FROM stream.hub[abc]`
- `INTO stream.hub[0]x`

4. 组合回归：
- 带 `WHERE`、`USING THEN`、多 source、`WITH` 组合；
- 验证 `sql_part` 不被 selector 解析破坏。

---

## Story 14.11 详细设计：BuiltinRegistry

### 14.11.1 核心接口

新增头文件（建议）：

1. `src/framework/core/builtin_registry.h`
2. `src/framework/core/builtin_registry.cpp`

关键接口（示意）：

```cpp
struct StreamOptionField {
    std::string key;
    std::string type;      // int|string|bool|enum|array
    bool required = false;
    std::string default_value;
    std::vector<std::string> enum_values;
    int64_t min_value = 0;
    int64_t max_value = 0;
    bool has_range = false;
    bool power_of_two = false;
    std::string desc;
};

struct StreamChannelTypeDescriptor {
    std::string type;       // ring, tcp_session_mock, stream_hub ...
    std::string display_name;
    std::vector<std::string> allowed_roles; // source|sink|both
    std::vector<StreamOptionField> option_schema;

    std::function<int(const rapidjson::Value& options,
                      std::string* normalized_json,
                      std::string* err)> validate_and_normalize;

    std::function<int(const std::string& category,
                      const std::string& name,
                      const std::string& normalized_json,
                      std::shared_ptr<IStreamChannel>* out,
                      std::string* err)> build;
};

struct BuiltinOperatorDescriptor {
    std::string category;
    std::string name;
    std::vector<std::string> aliases; // 含 builtin.*
    OperatorFactory factory;
};
```

### 14.11.2 StreamPlugin 改造

1. `BuildOneChannelLocked` 改为：
- 读取 `type` 对应描述符；
- 用描述符 `validate_and_normalize` 校验并规范化参数；
- 调用描述符 `build` 构建通道。

2. 删除硬编码分支（`ring`、`tcp_session_mock`）：
- `ring`、`tcp_session_mock` 通过注册函数挂到 `BuiltinRegistry`。

3. 存储层改造（与 Story 14.12 共用）：
- `stream_channel_store` 增加结构化参数字段与角色字段：
  - `option_json TEXT NOT NULL DEFAULT '{}'`
  - `role TEXT NOT NULL DEFAULT 'both'`
  - `version INTEGER NOT NULL DEFAULT 1`（用于 `execute`/`modify/remove` 并发下的 TOCTOU 版本校验）
- 旧字段 `option` 在迁移窗口保留读取兼容（项目构建期可直接迁移为新字段并统一写新字段）。

### 14.11.3 CatalogPlugin 改造

`CatalogPlugin::Load` 改为遍历 `BuiltinRegistry::ListBuiltinOperators()` 批量注册：

1. 主名注册：`category.name` 或 legacy 名。
2. 别名注册：`builtin.*` 别名由描述符统一给出，避免手工重复。

---

## Story 14.12 详细设计：Stream 通道产品化

### 14.12.1 API 设计

新增接口：

1. `POST /channels/stream/definitions/query`
- 返回可创建类型、角色能力、参数 schema。
- 命名采用 `definitions`（而非 `types`），用于区分“可创建定义”与实例字段 `type`，避免语义歧义。

请求：

```json
{}
```

响应（示意）：

```json
{
  "definitions": [
    {
      "channel_type": "ring",
      "display_name": "Ring Stream",
      "allowed_roles": ["source", "sink", "both"],
      "option_schema": [
        {"key":"ring_mode","type":"enum","enum_values":["spsc","spmc","mpsc","mpmc"],"default_value":"spsc","required":true},
        {"key":"ring_size","type":"int","default_value":"256","required":true,"min_value":2,"power_of_two":true},
        {"key":"overflow","type":"enum","enum_values":["drop","block"],"default_value":"drop","required":true},
        {"key":"finite","type":"bool","default_value":"false","required":false}
      ]
    },
    {
      "channel_type": "stream_hub",
      "display_name": "Stream Hub",
      "allowed_roles": ["source", "sink", "both"],
      "option_schema": [
        {"key":"mode","type":"enum","enum_values":["split","merge"],"default_value":"split","required":true},
        {"key":"partition_count","type":"int","default_value":"4","required":true,"min_value":1},
        {"key":"partition_ring_mode","type":"enum","enum_values":["spsc","spmc","mpsc","mpmc"],"default_value":"spsc","required":true},
        {"key":"partition_ring_size","type":"int","default_value":"256","required":true,"min_value":2,"power_of_two":true}
      ]
    }
  ]
}
```

现有接口入参改为结构化：

1. `POST /channels/stream/add`
2. `POST /channels/stream/modify`

请求：

```json
{
  "type": "stream_hub",
  "name": "npm_hub",
  "role": "both",
  "options": {
    "mode": "split",
    "partition_count": 16,
    "partition_ring_mode": "spsc",
    "partition_ring_size": 1024
  }
}
```

`POST /channels/stream/query` 返回增强字段：

1. `type/name/role/status/in_use`
2. `option_json`
3. `capacity/size/is_finite/is_finished`
4. `derived_channels`（Hub 子通道信息，含分区索引与运行状态；`mode=split` 返回分区列表，`mode=merge` 返回空数组）

### 14.12.2 前端通道管理改造

`Channels.vue` 在 Stream 区域新增：

1. “新增 Stream 通道”按钮；
2. 类型下拉（由 `/api/channels/stream/definitions/query` 填充）；
3. 动态参数表单（按 `option_schema` 渲染）；
4. 角色选择（`source/sink/both`）；
5. 提交时发送结构化 `options`。

字段控件映射：

1. `enum` -> 下拉框
2. `int` -> 数字输入 + 范围校验
3. `bool` -> 开关
4. `array` -> 标签输入（用于数组类型扩展参数）

### 14.12.3 链式处理语义（重点）

#### A. Source 语法

按 mode 区分：

1. `mode=split` 支持：
- `stream.<hub>`（根通道，允许自动补 `[*]`）
- `stream.<hub>[*]`（全部子通道）
- `stream.<hub>[i]`（单个子通道）
2. `mode=merge` 仅支持：
- `stream.<hub>`（根通道）
- `stream.<hub>[*]`、`stream.<hub>[i]` 在语义阶段一律报错（`STREAM_HUB_SELECTOR_NOT_ALLOWED_MERGE`）

#### B. 自动补 `[*]`

当且仅当满足以下条件时自动展开：

1. 该引用出现在 `FROM`（source）位置；
2. 命中的通道类型为 `stream_hub` 且 `mode=split`；
3. 未显式写 `[i]` 或 `[*]`。

则 `stream.<hub>` 自动等价为 `stream.<hub>[*]`。

#### C. INTO 语义

1. `INTO stream.<hub>` 在 `mode=split|merge` 下都合法（写入 root sink）。
2. `INTO stream.<hub>[*]`、`INTO stream.<hub>[i]` 在 `mode=split|merge` 下都一律报错（语义不允许）。

#### D. 可观测性

`stream execute/status` 补充：

1. `resolved_sources`：实际展开后的 source 列表（例如 16 个分区）。
2. `source_expand_rule`：`auto_wildcard` 或 `explicit`。

### 14.12.4 Parser 与调度层改造

1. SQL 解析器：
- 使用 `ReadChannelRef()` 解析 `FROM/INTO` 的 channel selector；
- 不改 `ReadIdentifier()` 全局词法规则。

2. Scheduler source 解析：
- 新增统一入口 `ResolveStreamSourceBindings(...)`；
- `selector` 缺省且通道为 `stream_hub(split)` 时自动等价 `[*]`；
- `[*]` 展开为全部子通道，`[i]` 绑定单分区；
- `stream_hub(merge)` 仅允许 root source（`stream.<hub>`）；出现 `[*]` 或 `[i]` 直接返回 `STREAM_HUB_SELECTOR_NOT_ALLOWED_MERGE`；
- selector 越界/格式错误返回 `BAD_REQUEST`。

3. 调度分类与执行一致性：
- `ClassifySqlTaskKind` 与 `ExecuteStreamTask` 强制复用同一 source 解析函数；
- 禁止“分类通过但执行阶段因 selector 语义差异失败”的不一致。

4. `FindChannel` 改造：
- `stream.<name>` 作为 source 时，支持按名称跨 type 解析（与 sink 解析语义一致）。

5. 租约键展开：
- `stream_hub(split)`：`[*]` 展开后按子通道分别占用租约键，保持冲突检测准确；
- `stream_hub(merge)`：始终占用 hub root 租约键，不做子通道展开。

6. 运行态观测增强：
- `stream execute` 返回 `task_id/runtime_task_id/resolved_sources/source_expand_rule`；
- `stream status/list` 返回与任务绑定的一致展开信息，便于排查冲突与误配置。

---

## Story 14.13 详细设计：流式 Group DAG（同源并发 + 串行链式 + 组合）

### 14.13.1 目标与边界

目标：

1. `group` 作为“任务编排形态”，不再等价于某一种执行拓扑；
2. 在同一 `group` 内同时支持：
- 串行链式（`serial chain`）；
- 同源广播并发（`same source broadcast`）；
- 串并组合（先并后串、先串后并）。
3. 在“同源广播”分支上保证 source 读取一次、各分支数据集合一致（允许全分支一致丢弃）。

Sprint 14 边界：

1. 仅支持“单任务内 DAG 编排”，不支持“跨任务共享 source”；
2. 不支持 late join（运行中动态加节点）；
3. 不支持回放/持久化重放。

### 14.13.2 API 与任务契约（复用现有 URI）

不新增路由，复用：

1. `POST /tasks/stream/execute`
2. `POST /scheduler/stream/execute`

通过请求体显式区分（禁止隐式猜测）：

1. `execution_kind=single`
- 请求：
```json
{
  "execution_kind": "single",
  "sql_text": "SELECT ...",
  "timeout_s": 0
}
```

2. `execution_kind=group`
- 请求：
```json
{
  "execution_kind": "group",
  "group_mode": "dag",
  "sql_text": "SELECT ...; SELECT ...;",
  "timeout_s": 0
}
```

`sql_text` 解析规则（强制）：

1. 多 SQL 只允许使用分号 `;` 切分；换行仅用于排版，不参与语义切分。
2. 后端按词法状态机切分（字符串/注释中的 `;` 不切分）：
- 状态：`normal`、单引号、双引号、反引号、行注释 `--`、块注释 `/* */`。
3. 仅在 `normal` 状态命中 `;` 才切分语句。
4. 尾部分号允许；中间空语句（如 `;;`）直接拒绝并返回语句序号。
5. `single`：切分后语句数必须为 `1`；`group`：切分后语句数必须 `>=2`。

字段规则：

1. `sql_text`：必填、非空。
2. `group_mode`：仅 `group` 需要，当前固定为 `dag`。
3. `timeout_s`：可选，默认 `0`；`0` 表示不设组级超时，`>0` 表示秒级超时，需满足 `0 <= timeout_s <= max_stream_group_timeout_s`（配置项，默认 `86400`）。
4. `share_set_ready_timeout_s`：可选，默认 `30`；`timeout_s>0` 时取 `min(share_set_ready_timeout_s, timeout_s)`。
5. DAG 规模需满足护栏：`nodes <= max_group_nodes`、`edges <= max_group_edges`、`source_share_sets <= max_group_share_sets`、`sum(sql_bytes) <= max_group_sql_bytes`（默认建议：`64/256/16/262144`）。

严格校验（不做隐式猜测）：

1. `single` 必须携带 `sql_text`，不得携带 `group_mode/dag/sql/sqls`。
2. `group` 必须携带 `group_mode=dag` 与 `sql_text`，不得携带 `dag/nodes/source_share_sets/sql/sqls`。
3. 后端负责 DAG 构建与校验；前端不参与 DAG 语义构建。
4. `timeout_s` 非法（负数、超上限、非整数）直接 `BAD_REQUEST`。
5. DAG 规模越界直接 `STREAM_GROUP_DAG_TOO_LARGE`。
6. 多节点共享同一 `resolved_sink_key` 时按 sink 能力做并发写预算校验（能力不足才拒绝，不做“一刀切单写者”限制）：
- `stream sink`：严格按 `IStreamChannel::Capabilities().concurrency` 判定（`put_mode/max_producers`）；
- `dataframe sink`：默认允许多写（append 语义）；
- `database sink`：默认允许多写（并发表写入）；若后续驱动声明并发写上限，则按上限判定。

说明：

1. `stream stop/status/list` 继续沿用现有路由；
2. `task_id` 仍由 `TaskPlugin` 生成，`runtime_task_id` 仍由 `Scheduler` 生成；
3. 外部仍不可自定义 `task_id/runtime_task_id`。
4. `task_id` 是对外唯一主键：`stop/status` 入参统一使用 `task_id`。
5. `execute/status/list` 响应统一同时返回 `task_id` 与 `runtime_task_id`（后者仅用于观测与排障，不作为外部操作键）。
6. 响应统一补充：`runtime_kind=single|group`、`group_mode`（group 时返回）。

### 14.13.3 调度模型

新增核心对象：

1. `StreamTaskGroup`
- 字段：`group_id/status/nodes/edges/source_keys/sink_keys/error_info`
- 方法：`RequestStop/Join/Snapshot`

2. `DagNodeRuntime`
- 每个节点绑定一条 SQL 的 stream pipeline；
- 字段：`node_id/task_ptr/state/depends_on/dependents/start_condition`

3. `BroadcastHub`
- 用于 `source_share_set` 内共享 source；
- 单 reader：从共享 source 拉取 `PollEvent`；
- 多 writer：将同一 `StreamBatch` 分发到 set 成员的入口通道；
- 入口通道按节点并发能力选择：
  - 节点并行度 `=1` -> `ring(spsc)`；
  - 节点并行度 `>1` -> `ring(spmc)`；
- `overflow` 默认 `drop`，但采用“全分支一致丢弃（coordinated drop）”。

### 14.13.4 DAG 归一化与校验细则

在执行前，统一做 `NormalizeDagPlan`：

1. 从 `sql_text` 切分得到 `sqls[]`，按输入顺序生成节点 `n1..nN`；
2. 逐节点解析 SQL 并提取：
- `resolved_source_bindings`
- `resolved_source_keys`
- `resolved_sink_key`
- `operator_ref`
3. 自动构建依赖边：
- 若 `node_j` 的 source 引用了前序节点 `node_i` 写入的 `stream sink`，则加边 `i -> j`，`start_condition=on_running`；
- 若同一 source 可匹配多个前序生产者，直接拒绝并返回 `STREAM_GROUP_DAG_INVALID`（歧义上游）；
- 无上游依赖的节点作为 DAG root。
4. 自动构建 `source_share_sets`：
- 在 root 节点中，按 canonical `resolved_source_keys` 分组；
- 同组且成员数 `>=2` 自动生成一个 share set；
- share set 成员 `start_condition` 固定 `on_running`。
5. 构建邻接表与入度表，Kahn 拓扑排序校验无环。
6. 校验 share set（按 canonical 集合比较）：
- `members` 全部存在；
- 对每个成员节点：将 `resolved_source_keys` 做“去重 + 字典序排序”，得到 `canonical_keys(node)`；
- `canonical_keys(node)` 在同一 set 内必须完全相等（顺序不同视为相等）；
- 返回差异明细：`missing_keys/extra_keys`；
- `source_ref`（若由规则推导）同样按 canonical 校验；
- 成员节点在 set 外不得再声明“同源广播”输入；
- 成员节点 `start_condition` 必须为 `on_running`。
7. 校验共享 sink 写预算（按 `resolved_sink_key` 聚合）：
- `required_writers = group 中写该 sink_key 的 node 数`；
- `stream sink`：`put_mode` 必须为 `MULTI` 或 `required_writers==1`，且 `max_producers==0 || max_producers>=required_writers`；
- `dataframe/database sink`：Sprint 14 默认 `required_writers` 不设上限（后续可由驱动/策略收紧）；
- 预算不满足返回 `STREAM_GROUP_SINK_CAPABILITY_MISMATCH`，并回传 `sink_key/required/actual`。

失败即拒绝，不进入运行态。

### 14.13.5 DAG 语义定义

1. `depends_on`：
- `A -> B` 表示 B 的启动受 A 约束。

2. `start_condition`：
- `on_running`：依赖节点进入 running 即可启动（流式链式衔接）；
- `on_finished`：依赖节点结束后启动（严格串行）。
- Sprint 14 限制：属于 `source_share_sets` 的节点仅允许 `on_running`。

3. `source_share_sets`：
- 描述同源广播组；
- 同一 set 内成员节点必须解析到同一个 `resolved_source_keys` 集合；
- 不在 set 内的节点按自身 source 独立消费。

4. 共享 sink 写语义（能力驱动）：
- 多个节点写同一 sink 在能力满足时允许（平台能力优先，不做静态禁用）；
- `stream sink` 共享写时，顺序语义按通道能力降级为 `PER_PRODUCER_FIFO`，不承诺全局 FIFO；
- `dataframe/database sink` 共享写时，不承诺跨节点 wall-clock 顺序；
- 上层可通过任务模板/审批/配额策略约束使用，不在内核做语义外兜底限制。

5. 特例映射：
- 纯串行链式：DAG 中仅边、无 `source_share_sets`；
- 纯同源并发：多个入度为 0 节点 + 同一个 `source_share_set`；
- 串并组合：同时存在依赖边和 share set。

### 14.13.6 执行流程（Scheduler）

1. 解析与拓扑校验：
- 对 `sql_text` 切分后的每条 SQL 做解析；
- 每个节点必须是合法 stream SQL；
- 校验 DAG 无环、依赖合法、自动构建后的 `source_share_sets` 合法。

2. source/sink 解析与租约：
- 对每个节点执行 `ResolveStreamSourceBindings(..., stage=execute)`；
- 对每个节点执行 `ResolveStreamSink(...)`；
- `source_share_set` 成员按 canonical `resolved_source_keys` 做同源一致性校验（错误返回 `missing_keys/extra_keys` 差异明细）；
- 按 `resolved_sink_key` 聚合执行写预算校验（`STREAM_GROUP_SINK_CAPABILITY_MISMATCH`）；
- 在 `stream_channel_refs_mu_` 下执行“版本快照复核 + 冲突检测 + 引用登记”原子提交，失败即整体拒绝（TOCTOU 防护）；
- 组级一次性申请 source/sink 租约并集（`lease_owner_id=runtime_task_id`），失败则整批回滚并返回冲突；
- group 内部节点执行复用组级租约，禁止逐节点二次租约申请（避免部分成功导致占用残留）。

3. 运行装配：
- 为每个 DAG 节点创建 `DagNodeRuntime`；
- 为每个 `source_share_set` 创建 `BroadcastHub`；
- 为共享 `stream sink` 创建 `SharedStreamSinkWriterGroup`（引用计数 close 协议）：
  - 每个 node 注入独立 writer view；
  - writer view 的 `CloseStream()` 仅做引用递减，最后一个 writer 才关闭底层 sink；
  - 防止“先结束节点提前关闭 sink”导致其余节点写失败；
- 将 set 成员节点输入绑定到 `BroadcastHub` 视图；
- 非 set 节点沿用现有 source 直连。

4. 启动推进：
- 启动所有入度为 0 的节点；
- 节点状态变化时推进后继节点；
- 组完成条件：所有节点进入终态。

关键实现约束：

1. 组内任一节点启动失败，整体回滚（停止已启动节点并释放租约）；
2. `BroadcastHub` 仅在 share set 全成员 ready 后才开始消费 source（防“晚加入”丢数据）；
3. 组级租约申请与释放必须成对（异常路径同样释放）。
4. share set ready 屏障必须有超时（`share_set_ready_timeout_s`，默认 `30`；若 `timeout_s>0`，取 `min(share_set_ready_timeout_s, timeout_s)`）；超时直接 fail-fast 并返回 `STREAM_GROUP_SHARE_SET_READY_TIMEOUT`。
5. 共享 `stream sink` 在最后一个 writer 结束前不得进入 closed 状态，避免误触发 `ECANCELED/EOF`。
6. `timeout/share_set_ready_timeout` 统一写入语义错误码（`STREAM_GROUP_TIMEOUT` / `STREAM_GROUP_SHARE_SET_READY_TIMEOUT`），不得仅返回数值 errno。
7. group 停止序列固定为“先停 hub，再停 node，再 join，再释放租约”。

### 14.13.7 一致性与完整性保证

1. 零拷贝原则：
- 广播时复用同一 `std::shared_ptr<arrow::RecordBatch>`；
- 仅复制 `StreamBatch` 元数据，不深拷贝 batch 数据。

2. 一致性语义（仅对 share set）：
- 目标是“多分支消费到同一批数据集合”，不是“绝对不丢包”；
- 默认策略 `coordinated drop`：
  - 分发前检查 set 全成员是否可写；
  - 任一成员不可写时，本批次对全成员一致 drop；
  - 仅当全成员可写时，才对全成员写入；
- 禁止部分分支写入、部分分支丢弃，避免分支视图不一致。

`coordinated drop` 参考算法：

```text
for each input batch from shared source:
  seq = next_broadcast_seq++
  input_batches++
  input_rows += batch.rows

  if any(member_queue is unavailable or full):
      dropped_batches_shared++
      dropped_rows_shared += batch.rows
      last_dropped_seq = seq
      continue

  dispatch_ok = true
  // all writable, then fanout
  for each member_queue:
      rc = Put(batch)
      if rc != 0:
          // invariant break: fail-fast to avoid partial visibility ambiguity
          dispatch_ok = false
          mark_share_set_failed(seq, rc)
          fail_group()
          break

  if dispatch_ok:
      delivered_batches++
      delivered_rows += batch.rows
      last_delivered_seq = seq
```

实现约束：

1. `BroadcastHub` 是 share set 唯一生产者，避免多写者竞争破坏一致性；
2. 每个 share set 维护单调 `broadcast_seq`，用于诊断和审计；
3. 若出现“预检查可写但写入失败”，按系统异常处理（fail-fast），不尝试半成功补偿。
4. `delivered_*` 仅在“全分支写入成功”时累计；任何写入失败批次不得计入 delivered 统计。

3. 高流量行为：
- 在背压高峰时允许按批次丢弃（全分支一致）；
- 不采用默认 `block`，避免整体阻塞放大风险。

4. 顺序语义：
- 每个节点输入保持到达顺序（per-node FIFO）；
- 不承诺跨节点 wall-clock 顺序，仅保证集合一致。

### 14.13.8 状态机与故障语义

1. fail-fast：
- 任一节点失败可配置两种策略（Sprint 14 固定为 `group_fail_fast=true`）；
- 固定策略下：任一节点 `failed` -> group `failed` -> 请求停止其余节点。

2. stop/cancel：
- `stream/stop(group_id)`：先停所有 `BroadcastHub`，再停节点任务，等待 join；
- 正常 stop 终态为 `stopped`；
- 用户取消与故障失败语义分离（`cancelled` vs `failed`）。
- 实现约束：`StreamTaskGroup` 需提供“停 node 前置回调”，由 Scheduler 注入并执行“停 hub”动作，禁止在 node stop 之后再停 hub。

3. timeout：
- `timeout_s=0`：不启用组级超时；
- `timeout_s>0`：从 group 进入 `preparing` 开始计时，超时触发与 fail-fast 同级收敛（先停 hub，再停 node）；
- 超时终态统一为 `failed`，错误码 `STREAM_GROUP_TIMEOUT`，错误信息包含超时秒数与未完成节点列表。
- `share_set_ready_timeout_s` 触发时终态统一为 `failed`，错误码 `STREAM_GROUP_SHARE_SET_READY_TIMEOUT`。

4. 资源回收：
- group 终态后统一释放 source/sink 租约；
- 清理 node 运行时与 hub，防 in-use 假占用。

状态定义（强制）：

1. group：`created -> preparing -> running -> stopping -> {stopped|failed|cancelled}`
2. node：`pending -> ready -> running -> stopping -> {stopped|failed}`，或 `pending/ready -> skipped`

事件优先级（并发事件冲突时按高到低裁决）：

1. `failed`（`node_failed/dispatch_fail/timeout/share_set_ready_timeout`）
2. `cancelled`（用户取消）
3. `stopped`（用户 stop 正常收敛）

转移规则（必须）：

1. group 一旦进入终态（`stopped|failed|cancelled`）不得再迁移；
2. 当 `stop/cancel` 与 `node_failed/timeout` 并发发生时，按优先级落为 `failed`；
3. `skipped` 仅用于 group 进入 `failed/cancelled` 前尚未启动的节点，不可由 `running` 转入；
4. group 进入终态后统一触发“停 hub -> 停 node -> join -> 释放租约 -> 回收运行时”。

### 14.13.9 可观测性与接口返回

`execute` 返回（group）：

```json
{
  "status": "submitted",
  "task_id": "task_1712200000123_1",
  "runtime_task_id": "stream_group_...",
  "runtime_kind": "group",
  "group_mode": "dag",
  "node_count": 4,
  "share_set_count": 1
}
```

`status/list` 增强字段（group）：

1. `task_id/runtime_task_id`
2. `group_status`
3. `nodes`（每个 node 的 `id/status/error/processed_rows/output_rows`）
4. `share_sets`（`id/source_ref/members/input_batches/delivered_batches/dropped_batches_shared/drop_ratio/last_delivered_seq/last_dropped_seq`）
5. `resolved_sources`（按 node 展示）
6. `error_code` 统一为语义字符串错误码（如 `STREAM_GROUP_TIMEOUT`）；可选保留 `error_no`（整数 errno）用于底层诊断。

`POST /scheduler/stream/status` 返回样例（group 明细）：

```json
{
  "task_id": "task_1712200000123_1",
  "runtime_task_id": "stream_group_1712200000123_1",
  "runtime_kind": "group",
  "group_mode": "dag",
  "status": "running",
  "group_status": "running",
  "stop_requested": false,
  "joined": false,
  "node_count": 4,
  "active_nodes": 3,
  "started_ms": 1712200000123,
  "last_active_ms": 1712200002456,
  "finished_ms": 0,
  "error_code": "",
  "error_message": "",
  "resolved_sources": [
    {
      "node_id": "n1",
      "sources": ["netcard.eth1"],
      "expand_rule": "explicit"
    },
    {
      "node_id": "n2",
      "sources": ["stream.npm_hub[*]"],
      "expand_rule": "auto_wildcard"
    }
  ],
  "nodes": [
    {
      "id": "n1",
      "status": "running",
      "depends_on": [],
      "start_condition": "on_running",
      "processed_batches": 1200,
      "processed_rows": 96000,
      "output_batches": 1188,
      "output_rows": 95040,
      "dropped_batches": 12,
      "poll_timeouts": 4,
      "poll_errors": 0,
      "last_error": ""
    },
    {
      "id": "n2",
      "status": "running",
      "depends_on": ["n1"],
      "start_condition": "on_running",
      "processed_batches": 1188,
      "processed_rows": 95040,
      "output_batches": 1188,
      "output_rows": 95040,
      "dropped_batches": 0,
      "poll_timeouts": 3,
      "poll_errors": 0,
      "last_error": ""
    },
    {
      "id": "n3",
      "status": "failed",
      "depends_on": ["n1"],
      "start_condition": "on_running",
      "processed_batches": 1179,
      "processed_rows": 94320,
      "output_batches": 1179,
      "output_rows": 94320,
      "dropped_batches": 0,
      "poll_timeouts": 1,
      "poll_errors": 1,
      "last_error": "operator custom.storage failed"
    }
  ],
  "share_sets": [
    {
      "id": "s1",
      "source_ref": "netcard.eth1",
      "members": ["n2", "n3"],
      "input_batches": 1200,
      "delivered_batches": 1188,
      "dropped_batches_shared": 12,
      "drop_ratio": 0.01,
      "input_rows": 96000,
      "delivered_rows": 95040,
      "dropped_rows_shared": 960,
      "last_delivered_seq": 1187,
      "last_dropped_seq": 1199
    }
  ]
}
```

`POST /scheduler/stream/list` 返回样例（group 摘要列表）：

```json
{
  "tasks": [
    {
      "task_id": "task_1712200000123_1",
      "runtime_task_id": "stream_group_1712200000123_1",
      "runtime_kind": "group",
      "group_mode": "dag",
      "status": "running",
      "group_status": "running",
      "node_count": 4,
      "active_nodes": 3,
      "share_set_count": 1,
      "processed_rows": 189360,
      "output_rows": 188400,
      "dropped_batches_shared": 12,
      "drop_ratio": 0.01,
      "started_ms": 1712200000123,
      "last_active_ms": 1712200002456,
      "finished_ms": 0
    },
    {
      "task_id": "task_1712199988000_2",
      "runtime_task_id": "stream_group_1712199988000_2",
      "runtime_kind": "group",
      "group_mode": "dag",
      "status": "stopped",
      "group_status": "stopped",
      "node_count": 3,
      "active_nodes": 0,
      "share_set_count": 1,
      "processed_rows": 120000,
      "output_rows": 119400,
      "dropped_batches_shared": 6,
      "drop_ratio": 0.005,
      "started_ms": 1712199988000,
      "last_active_ms": 1712199994200,
      "finished_ms": 1712199995000
    }
  ]
}
```

### 14.13.10 对现有调度与 SQL parser 的影响评估

1. 调度影响：
- `Scheduler` 在现有 `/scheduler/stream/execute` 中新增 `execution_kind` 分发；
- `single` 与 `group` 均统一接收 `sql_text`；
- `single`：`sql_text` 切分后必须为单语句，复用现有单任务执行链路；
- `group`：由后端执行“切分 + DAG 归一化 + 执行编排”（`StreamTaskGroup`、`BroadcastHub`）。

2. parser 影响：
- 不新增 SQL 语法；
- 复用现有 parser 对每条切分后的 SQL 独立解析；
- 新增 `sql_text` 切分器（词法状态机），仅负责语句边界判定；
- 不改 `SqlParser` 核心 token 规则（如 `ReadIdentifier/ReadChannelRef`），避免核心解析回归风险。

### 14.13.11 前端与交互影响

1. `Tasks.vue`：
- 流式执行时显式提交 `execution_kind`；
- 单 SQL -> `single`；
- 多 SQL -> 提交 `group`，但不再由前端构建 `dag`；
- 前端仅提交 `sql_text`，DAG 结构由后端构建并回传用于展示。

2. 分号输入说明：
- 多 SQL 必须用分号 `;` 分隔；换行不作为切分依据；
- 前端需给出明确提示（占位符/帮助文案）；
- 最终执行语义由后端解析与 DAG 归一化决定。

3. 任务展示：
- 列表保留现有 `task_id/runtime_task_id/status`；
- 详情页展示后端返回的 DAG 摘要与明细（`nodes/share_sets/resolved_sources`），前端不做二次推导。

4. 前端请求模型与默认值（无歧义约束）：
- `single` 请求：
- 必填：`execution_kind="single"`、`sql_text`；
- 可选：`timeout_s`（默认 `0`）；
- 禁止：`group_mode`、`dag`、`sql`、`sqls`。
- `group` 请求：
- 必填：`execution_kind="group"`、`group_mode="dag"`、`sql_text`；
- 可选：`timeout_s`（默认 `0`）、`share_set_ready_timeout_s`（默认 `30`）；
- 禁止发送 `dag/nodes/source_share_sets/sql/sqls/null`，避免双语义与歧义分支。

5. 前端本地校验（提交前）：
- 仅做输入级校验：`sql_text` 非空；
- `group` 模式需在本地检测“分号切分后语句数 >= 2”；
- 不在前端做 DAG 语义校验（依赖、share set、环检测等全部交给后端）。

6. 错误码到 UI 提示映射（最小闭环）：
- `STREAM_GROUP_SQL_TEXT_INVALID`：多 SQL 切分或语句边界非法（提示用户使用分号分隔）；
- `STREAM_GROUP_DAG_INVALID`：DAG 配置非法（节点/依赖/字段）；
- `STREAM_GROUP_DAG_CYCLE_DETECTED`：存在依赖环；
- `STREAM_GROUP_SOURCE_MISMATCH`：share set 同源不一致；
- `STREAM_GROUP_SINK_CAPABILITY_MISMATCH`：共享 sink 并发写能力不足（展示 `sink_key/required/actual`）；
- `STREAM_GROUP_SHARE_SET_READY_TIMEOUT`：广播组就绪超时；
- `STREAM_GROUP_TIMEOUT`：组级执行超时；
- 其他错误码统一回退为“执行失败”，并显示原始 `error_code/error_message`。

7. 执行交互与状态轮询：
- 提交后立即进入“已提交/准备中”态；
- `running/preparing/stopping` 期间按固定周期轮询 `status`（建议 1s）；
- 终态（`stopped/failed/cancelled`）停止轮询；
- `stop` 按钮在 `running/preparing/stopping` 可见，提交中与终态禁用；
- 详情页固定展示：
  - `nodes`（`id/status/processed_rows/output_rows/last_error`）；
  - `share_sets`（`input_batches/delivered_batches/dropped_batches_shared/drop_ratio`）；
  - `resolved_sources/source_expand_rule`（排障信息）。

8. 组件落位建议（实现指引）：
- `Tasks.vue` 保留现有列表与单任务入口；
- `StreamTasks.vue`（或等价子组件）承载 group 详情与观测，不承载 DAG 语义编辑；
- 若不拆页，需在 `Tasks.vue` 内按 `runtime_kind` 分区渲染，避免 batch/stream/group 状态字段混淆。

### 14.13.12 关键错误码（新增）

1. `STREAM_GROUP_SQL_TEXT_INVALID`
2. `STREAM_GROUP_MODE_INVALID`
3. `STREAM_GROUP_DAG_INVALID`
4. `STREAM_GROUP_DAG_CYCLE_DETECTED`
5. `STREAM_GROUP_NODE_NOT_FOUND`
6. `STREAM_GROUP_SOURCE_MISMATCH`
7. `STREAM_GROUP_BRANCH_BUILD_FAILED`
8. `STREAM_GROUP_DISPATCH_FAILED`
9. `STREAM_GROUP_STOP_FAILED`
10. `STREAM_GROUP_MIXED_TASK_KIND`
11. `STREAM_GROUP_TIMEOUT`
12. `STREAM_GROUP_SHARE_SET_READY_TIMEOUT`
13. `STREAM_GROUP_DAG_TOO_LARGE`
14. `STREAM_GROUP_SINK_CAPABILITY_MISMATCH`

### 14.13.13 文档同步要求

1. Sprint 14 功能验收通过后，必须同步更新功能 README：
- 多 SQL 输入必须使用分号 `;` 切分（换行不切分）；
- `single/group` 请求体示例（统一 `sql_text`）；
- 常见错误码与排障示例（含 `STREAM_GROUP_TIMEOUT`、`STREAM_GROUP_SHARE_SET_READY_TIMEOUT`、`STREAM_GROUP_SQL_TEXT_INVALID`）。

---

## 错误码约定（新增）

1. `STREAM_CHANNEL_TYPE_NOT_FOUND`
2. `STREAM_CHANNEL_OPTION_INVALID`
3. `STREAM_CHANNEL_ROLE_MISMATCH`
4. `STREAM_HUB_SELECTOR_INVALID`
5. `STREAM_HUB_SELECTOR_OUT_OF_RANGE`
6. `STREAM_HUB_SELECTOR_NOT_ALLOWED_INTO`
7. `STREAM_HUB_SELECTOR_NOT_ALLOWED_MERGE`
8. `STREAM_GROUP_SQL_TEXT_INVALID`
9. `STREAM_GROUP_MODE_INVALID`
10. `STREAM_GROUP_DAG_INVALID`
11. `STREAM_GROUP_DAG_CYCLE_DETECTED`
12. `STREAM_GROUP_NODE_NOT_FOUND`
13. `STREAM_GROUP_SOURCE_MISMATCH`
14. `STREAM_GROUP_BRANCH_BUILD_FAILED`
15. `STREAM_GROUP_DISPATCH_FAILED`
16. `STREAM_GROUP_STOP_FAILED`
17. `STREAM_GROUP_MIXED_TASK_KIND`
18. `STREAM_GROUP_TIMEOUT`
19. `STREAM_GROUP_SHARE_SET_READY_TIMEOUT`
20. `STREAM_GROUP_DAG_TOO_LARGE`
21. `STREAM_GROUP_SINK_CAPABILITY_MISMATCH`

---

## 测试设计

### 14.11 测试

1. 注册中心：
- 重复注册冲突；
- 缺失类型；
- 非法参数校验失败。

2. StreamPlugin：
- 通过描述符构建 `ring`、`tcp_session_mock` 成功；
- 未注册类型失败。

3. CatalogPlugin：
- 内置算子批量注册成功；
- `builtin.*` 别名可创建实例。

4. 调度一致性：
- `classify` 与 `execute` 对同一 selector SQL 的判定一致；
- `resolved_sources` 字段符合自动/显式展开规则。
- `modify/remove` 与 `execute` 并发下，展开 key 的 in-use 判定无遗漏。

### 14.12 测试

1. API：
- `definitions/query` 返回 schema；
- `add/modify` 结构化参数校验；
- `query` 返回增强字段。
- `stream_hub(split)` 返回 `derived_channels` 分区列表，`stream_hub(merge)` 返回空数组。

2. 调度语义：
- `FROM stream.hub` 自动展开 `[*]`；
- `FROM stream.hub[*]` 与 `FROM stream.hub[0]` 正确；
- `INTO stream.hub[0]` 报错。
- `mode=merge` 下 `FROM stream.hub` root 读取正确，且不自动展开；
- `mode=merge` 下 `FROM stream.hub[*]`、`FROM stream.hub[0]` 报 `STREAM_HUB_SELECTOR_NOT_ALLOWED_MERGE`。
- `max_resolved_sources` 护栏生效（超阈值拒绝）。
- `stream status/list` 可见 `resolved_sources/source_expand_rule`。

3. SQL parser 回归：
- 历史 SQL 核心用例 AST 不变；
- selector 语法新增/非法用例覆盖完整。
- `SELECT` 列表达式、`WHERE`、`USING THEN WITH` 的既有解析行为不退化。

4. 前端：
- 动态表单渲染；
- 参数与角色校验；
- 提交成功与错误提示链路。

### 14.13 测试

1. API 契约：
- `execution_kind=single` + `sql_text`（单语句）成功；
- `execution_kind=group` + `group_mode=dag` + `sql_text`（多语句）成功；
- `group` 传 `dag/nodes/source_share_sets/sql/sqls` 必须拒绝；
- `single` 传 `group_mode/dag` 必须拒绝；
- `execute/status/list` 均返回 `task_id/runtime_task_id`，且 `stop/status` 仅接受 `task_id`。
- `stop/status` 传 `runtime_task_id` 必须拒绝（`BAD_REQUEST` 或等价错误码），避免主键语义漂移。

2. `sql_text` 切分与解析：
- 多 SQL 必须用分号切分；换行不切分；
- 字符串/注释中的分号不得误切分；
- 中间空语句（`;;`）拒绝并返回 `STREAM_GROUP_SQL_TEXT_INVALID`；
- 语句级解析失败返回 `sql_index`（统一 `0-based`）与原始错误上下文。

3. DAG 构建与校验：
- 后端按 `sql_text` 自动构建 DAG（节点顺序稳定）；
- 依赖推导正确（前序 sink -> 后续 source）；
- 同源 share set 自动识别正确；
- share set 同源比较按 canonical 集合执行（顺序差异视为相等；缺失/多余 key 报 `STREAM_GROUP_SOURCE_MISMATCH` 并返回 `missing_keys/extra_keys`）。

4. 纯串行链式：
- 线性链（A->B->C）按依赖推进；
- `on_running` 语义符合预期（本 Sprint 自动构建不引入前端 `on_finished` 配置入口）。

5. 纯同源广播：
- 同一 `source_share_set` 多节点共享 source；
- 固定 N 批输入下各节点消费数据集合一致（允许全分支一致 drop）；
- 高压下允许丢批，但必须“全分支一致 drop”（各节点 `delivered_batches` 一致，`dropped_batches_shared` 一致）。
- 校验 `last_delivered_seq/last_dropped_seq` 单调递增且跨分支一致。
- 校验 `coordinated drop` 下 `delivered_*` 仅在全分支写入成功时累计。

6. 串并组合：
- 覆盖“先并后串、先串后并”两类拓扑；
- 校验组级状态推进与最终收敛正确。

7. 异常与收敛：
- 任一节点失败触发 group fail-fast；
- stop/cancel 后其余节点可收敛到终态；
- source/sink 租约全部释放。
- `timeout_s` 触发后终态为 `failed`，错误码为 `STREAM_GROUP_TIMEOUT`（语义字符串）。
- `share_set_ready_timeout_s` 触发后错误码为 `STREAM_GROUP_SHARE_SET_READY_TIMEOUT`（语义字符串）。
- `stop/cancel/node_failed/timeout` 并发竞态按优先级收敛（`failed > cancelled > stopped`）。
- group 进入终态后不可再次迁移（终态幂等校验）；
- `skipped` 仅出现在 group `failed/cancelled` 时未启动节点，且不得由 `running` 转入。
- 停止顺序校验：`stop/timeout/fail-fast` 路径均满足“先停 hub，再停 node”。

8. 冲突与并发：
- group 运行时对 source/相关 sink `modify/remove` 返回 in-use；
- 同源冲突 group 提交返回冲突。
- `execute` 与 `modify/remove` 并发下无 TOCTOU 误判（版本校验与租约登记原子）。
- 组级一次性租约并集申请成功/失败语义正确；失败时无残留租约。
- `stream_hub(split)` 冲突粒度按分区键生效（不同分区可并发、相同分区冲突）；
- `stream_hub(merge)` 冲突粒度按 hub root 键生效（root 独占冲突）。
- 共享 sink 能力矩阵：
  - `ring(spsc/spmc)` 被多个 node 共享写时拒绝（`STREAM_GROUP_SINK_CAPABILITY_MISMATCH`）；
  - `ring(mpsc/mpmc)` 被多个 node 共享写时通过；
  - `database` 同表多 node 写默认通过；
  - `dataframe` 同名多 node append 默认通过（顺序不保证）。
- 共享 `stream sink` close 协议校验：
  - 任一 node 先结束不得提前关闭底层 sink；
  - 最后一个 node 结束后 sink 才进入 closed/finished。

9. DAG 护栏与可启动性：
- `max_group_nodes/max_group_edges/max_group_share_sets/max_group_sql_bytes` 超限拒绝；
- share set ready 屏障超时返回 `STREAM_GROUP_SHARE_SET_READY_TIMEOUT`。
- `timeout_s` 边界矩阵：
  - `timeout_s < 0` / 非整数 / 超上限拒绝；
  - `timeout_s = 0` 时不触发组级超时；
  - `timeout_s > 0` 时按秒触发并返回 `STREAM_GROUP_TIMEOUT`；
  - 当 `timeout_s > 0` 且 `share_set_ready_timeout_s` 更大时，屏障按 `min(...)` 生效。

10. parser 无回归：
- node SQL 仍复用既有 parser，历史 SQL 解析行为保持不变。

11. 可观测性校验：
- `share_sets.input_batches/delivered_batches/dropped_batches_shared/drop_ratio` 字段完整且数值自洽；
- 校验 `input_batches = delivered_batches + dropped_batches_shared`。
- `error_code` 为语义字符串；可选 `error_no` 为整数诊断字段。

12. 错误码映射回归（关键码）：
- `STREAM_HUB_SELECTOR_INVALID`、`STREAM_HUB_SELECTOR_OUT_OF_RANGE`、`STREAM_HUB_SELECTOR_NOT_ALLOWED_INTO`、`STREAM_HUB_SELECTOR_NOT_ALLOWED_MERGE`；
- `STREAM_GROUP_DAG_INVALID`、`STREAM_GROUP_DAG_CYCLE_DETECTED`、`STREAM_GROUP_SOURCE_MISMATCH`、`STREAM_GROUP_SINK_CAPABILITY_MISMATCH`；
- `STREAM_GROUP_SQL_TEXT_INVALID`、`STREAM_GROUP_TIMEOUT`、`STREAM_GROUP_SHARE_SET_READY_TIMEOUT`、`STREAM_GROUP_DAG_TOO_LARGE`；
- 每个关键错误码至少 1 个稳定触发用例，并校验错误信息包含定位上下文（node/source_ref/selector）。

13. 前端交互回归：
- 多 SQL 提交仅发送 `execution_kind/group_mode/sql_text`，不发送 `dag`；
- 前端显示“多 SQL 必须分号分隔”；换行不作为切分；
- 本地校验仅覆盖输入级规则（非空、多语句数量）；
- 错误码映射提示准确，`STREAM_GROUP_SINK_CAPABILITY_MISMATCH` 可展示能力差异；
- 提交/轮询/停止按钮状态机正确（终态停止轮询）；
- 详情页 `nodes/share_sets/resolved_sources` 展示完整且字段不缺失。

---

## 实施顺序（与 Planning 对齐）

1. 先实现 `BuiltinRegistry` 与后端注册改造（Story 14.11）。
2. 再实现 `definitions/query`、结构化通道管理 API 与动态表单联调（Story 14.12 后端+前端）。
3. 实现 `StreamTaskGroup + DagNodeRuntime + BroadcastHub` 与组级执行/停止/状态（Story 14.13 后端）。
4. 最后实现 `sql_text` 提交交互（前端不构建 DAG）与组级观测展示（Story 14.13 前端），并完成全链路回归。
