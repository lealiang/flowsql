# Feature: 通用基线检测

状态：`[x]` 已完成

## 业务意图

提供可复用的 Value、Ratio、Relation 基线检测插件，支持可选 bootstrap、在线 rolling、成熟度/可信度和关系模式风险融合。

## Non-Goals

- 不把历史重建、shadow/candidate 切换作为在线主路径。
- 不让基线算法承担业务告警处置或具体领域单位判断。

## 公共契约

`IBaselineService` 接收带稳定 `task/key/feature` 身份的 observation，输出 `score`、`band`、`confidence`、`maturity`、`trust` 和 evidence。Value/Ratio/Relation 共享任务边界，但各自维护明确的数学状态和 basis。

## 主链路

1. 可选 bootstrap 导入兼容 seed，或直接以空状态启动 rolling。
2. 按 `predict → band → score → gate_update → update_state` 更新在线状态，Relation 结果再进入模式融合。

## 完成任务

- `[x]` 实现 Value/Ratio/Relation 的 bootstrap、rolling 和 snapshot。
- `[x]` 实现 maturity、score trust、band calibration 和 readiness。
- `[x]` 实现 Relation routed summary、basis 和 pattern fusion。
- `[x]` 完成高基数状态清理、批量预测和并发测试。
