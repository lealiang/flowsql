# Active Task

Feature：`npm-offline-import-web`
原子任务：T0.1 控制面/数据面与部署依赖规格修订
状态：已完成

## 业务意图

- 按 `docs/framework.md` 与现有代码实现复核 `npm-offline-import-web` 规格，使浏览器上传、服务间控制请求、
  Scheduler 数据消费三段链路明确落在各自架构平面。
- 补齐会阻断生产闭环的 NPI provider、协议定义文件、插件批次生命周期、共享绝对路径和受管文件所有权契约，
  作为后续 Contract/Test First 的实施边界。

## Non-Goals

- 不修改 PluginLoader、Web、Scheduler、pcapfile、NPI、Router、部署配置、前端或测试代码。
- 不实现上传、删除、路径脱敏、插件生命周期修复或 T1～T5 中任一生产功能。
- 不改变任何公共 C++ ABI、packet Schema 或现有 HTTP 路由实现。
- 不重新打开已归档并按原 Non-Goals 完成的 `npm-offline-import` Feature。
- 不执行 `git commit`/`git push`。

## 边界

- 上传路由只绑定 WebServer 对外监听的 `httplib::Server`（默认 8081），不得通过 `EnumApiRoutes()` 注册到
  RouterAgency（默认 18802）；capture 文件内容只终止于 Web 北向入口。
- Web 只通过现有 HTTP URI 向 Scheduler 发送小型通道 JSON；packet 运行时数据继续使用
  `IBlockStreamChannel`/Arrow batch，不经过 HTTP。
- `pcap_upload_dir` 必须在启动时解析为绝对规范路径；Web 与 Scheduler 在单进程、Guardian 和 Docker
  部署中必须看到相同绝对目录，禁止依赖当前工作目录碰巧一致。
- Web 的受管文件存储拥有 `.part`、最终文件、失败回滚和安全删除生命周期；`PcapFileChannel` 只拥有打开的
  文件句柄及读取/batch 状态，不主动删除文件。
- 生产闭环必须同时部署 NPI provider、`protocols.yml` 和有效 `ldfile`，再加载 pcapfile provider；
  PluginLoader 必须先满足“所有 Option → 所有 Load → 所有 Start”且任意非零生命周期返回值均失败。

## 允许修改的文件

- `tasks/active_task.md`：冻结并记录本次文档原子任务状态。
- `tasks/specs/feat-npm-offline-import-web.md`：修订精益规格和任务/测试锚点。

## 验收

- 规格明确给出北向上传入口、服务间控制面和 Arrow 数据面的归类，禁止文件请求进入 RouterAgency。
- 规格冻结相同绝对共享目录，以及 Web 受管文件存储与 `PcapFileChannel` 的所有权和删除顺序。
- 规格将 NPI provider、可部署 `protocols.yml`、有效 `ldfile` 和 pcapfile provider 纳入正式部署验收。
- 规格将 PluginLoader 批次生命周期与“任意非零返回即失败”列为 P1 前置修复，并有顺序无关/失败阻断测试锚点。
- Web 对外列表和上传响应不暴露 `options.path`，Scheduler 内部控制 JSON 仍保留真实绝对路径。
- `git diff HEAD --check` 通过；本任务新增 patch 只落在两个允许文件。

## 验收结果

- 已按 `docs/framework.md` 和当前实现复核并冻结北向上传入口、HTTP 控制面、进程内 IID 调用及 Arrow
  数据面边界；上传路由明确不得注册到 RouterAgency。
- 已明确 Web `ManagedCaptureStore` 与 `PcapFileChannel` 的所有权边界、删除顺序、外部路径脱敏和三类部署的
  相同绝对目录要求。
- 已将 NPI provider、可部署 `protocols.yml`、有效 `ldfile`、pcapfile provider 及 PluginLoader P1 修复
  纳入 T1～T5 任务和机器可执行测试锚点。
- `git diff HEAD --check` 通过；新规格另以
  `git diff --no-index --check /dev/null tasks/specs/feat-npm-offline-import-web.md` 检查，无空白错误输出。
- 本任务只修改 `tasks/active_task.md` 和 `tasks/specs/feat-npm-offline-import-web.md`，未开始 T1 或生产代码修复。

## 时间盒与停止条件

- 时间盒：20 分钟。
- 规格修订、diff 边界检查和文档自检完成后，把本工作台标记为已完成并立即停止，不开始 T1 或生产修复。
- 若复核发现必须改变现有 `pcapfile` ABI、通过 Router 传输文件或实现跨主机文件复制才能满足主链路，
  以“被明确问题阻塞”停止。
