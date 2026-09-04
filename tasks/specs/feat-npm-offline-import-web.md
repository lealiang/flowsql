# Feature: NPM 离线文件上传与通道管理

状态：`[-]` 进行中
优先级：P0
前置 Feature：`npm-offline-import`（已完成）

## 业务意图

用户可在 Web 通道管理页面选择一个本地 `.pcap`/`.pcapng` 文件，配置通道名称与回放参数，
一次提交完成“上传文件 + 创建 `pcapfile` source 通道”。浏览器不需要、也不能填写服务器文件路径。

本 Feature 只补齐用户入口、受管文件存储和生产部署链路；文件格式解析、packet 字段、有限流、
回放、EOF 和 Scheduler block source 执行继续复用已完成的 `npm-offline-import` 契约。

## Non-Goals

- 不实现多文件批量上传、跨文件排序、断点续传、分片合并或上传进度持久化。
- 不实现浏览器 packet 预览、协议分析、过滤表达式或导入结果可视化。
- 不实现压缩文件自动解压，也不扩展 pcap/pcapng 格式支持矩阵。
- 不实现对象存储、跨主机文件复制或远程 URL 导入；首版要求 Web 与 Scheduler 可见同一文件系统目录。
- 不允许浏览器提交任意服务器路径，不提供通用文件浏览器。
- 不通过“编辑通道”替换已上传文件；需要替换时删除通道后重新上传。
- 不改变 `IBlockStreamChannel`、`IBlockStreamFactory`、`IBlockStreamManager`、packet Schema 或 NPI ABI。
- 不改变 `pcapfile` 通道配置跨进程重启不持久化的既有边界。

## 架构平面与路由边界

| 链路 | 归类 | 冻结边界 |
| --- | --- | --- |
| 浏览器 → Web `POST /api/channels/pcapfile/upload` | 北向文件入口 | multipart 仅由 WebServer 对外 `httplib::Server`（默认 8081）接收 |
| Web → Gateway → Scheduler `/channels/stream/add|remove|query` | 控制面 | 只传通道名、类型、参数和服务器绝对路径等小型 JSON |
| Scheduler → `PcapFilePlugin` | 进程内控制面 | 通过 `IQuerier`/IID 获取 block stream provider，不依赖具体实现类 |
| `PcapFileChannel` → Scheduler operator | 数据面 | packet 只通过 `IBlockStreamChannel` 和 Arrow `RecordBatch` 传输 |

上传路由是 Web 的北向大文件入口，不是 C++ 服务间运行时数据面。该路由必须直接绑定 WebServer 的
外部 HTTP server，禁止通过 `WebPlugin::EnumRoutes()`/`WebServer::EnumApiRoutes()` 注册到
RouterAgency（默认 18802），Gateway/Router 不得接收 capture 请求体。RouterAgency 现有 1 MiB 请求体
限制保持不变；上传完成后才进入既有 HTTP 控制面。

## HTTP 与数据契约

### 上传并创建通道

浏览器直接调用 WebPlugin 的 8081 北向入口，不经 Gateway/Router 传输文件内容：

```http
POST /api/channels/pcapfile/upload
Content-Type: multipart/form-data
```

| 字段 | 类型 | 必填 | 默认值 | 约束 |
| --- | --- | --- | --- | --- |
| `file` | file | 是 | - | 文件名以 `.pcap` 或 `.pcapng` 结尾，内容由 `PcapFilePlugin` 按文件头最终校验 |
| `name` | string | 是 | - | 非空逻辑通道名；不得包含路径分隔符、`.` 或 `..` 路径片段 |
| `format` | string | 否 | `auto` | `auto\|pcap\|pcapng` |
| `batch_packets` | uint32 | 否 | `256` | 大于 0 |
| `replay_mode` | string | 否 | `fast` | `fast\|timestamp` |
| `replay_speed_milli` | uint32 | 否 | `1000` | 大于 0 |

单文件默认上限为 1 GiB，可由 WebPlugin 的 `pcap_upload_max_bytes` 配置覆盖。超过限制返回 HTTP 413，
且不得保留临时文件。Web 必须分块写入磁盘，禁止把完整 capture 读入单个 `std::string` 或 JSON/Base64。

成功时 Web 向 Scheduler 发送现有控制面请求：

```json
{
  "type": "pcapfile",
  "name": "capture_01",
  "role": "source",
  "options": {
    "path": "/absolute/managed/path/capture.pcap",
    "format": "auto",
    "batch_packets": 256,
    "replay_mode": "fast",
    "replay_speed_milli": 1000
  }
}
```

HTTP 200 响应不向浏览器暴露受管绝对路径：

```json
{
  "type": "pcapfile",
  "name": "capture_01",
  "role": "source",
  "status": "running",
  "filename": "capture.pcap",
  "size_bytes": 4096
}
```

Web 对外的 `/api/channels/stream/query`、聚合列表和上传响应都不得返回 `pcapfile` 的原始 `option`
或 `option_json.path`；可改为返回安全的 `filename`/`size_bytes`。Scheduler 内部控制面
`/channels/stream/query` 继续保留真实 `options.path`，供 Web 删除受管文件和 Scheduler 诊断使用。

固定错误映射：字段或文件格式错误为 400；重名为 409；文件过大为 413；Scheduler/provider
不可用为 503；磁盘写入或内部错误为 500。所有失败响应为 JSON，并至少包含 `error` 字段。

### 受管文件与删除

- Web 使用 `<upload_dir>/pcapfile/` 作为受管根目录；启动时创建目录并解析为绝对规范路径。
- 服务端生成唯一临时文件名和最终文件名，不直接采用客户端路径；先写 `.part`，完整落盘后在同一目录
  原子重命名，再调用 `/channels/stream/add`。
- 写入、重命名或通道创建失败时，Web 删除本次 `.part`/最终文件；不得影响既有通道文件。
- Web 的私有 `ManagedCaptureStore` 拥有 `.part`、最终文件、失败回滚和安全删除生命周期；
  `PcapFileChannel` 只拥有打开的文件句柄和读取/batch 状态，不主动 `unlink` 文件。
- 删除顺序固定为：Web 从 Scheduler 内部查询取得受管路径 → Scheduler 成功执行 RemoveChannel 并关闭通道/
  outstanding batch → Web 按路径组件校验 canonical path 位于受管根内 → 删除文件。Scheduler 删除失败时保留文件；
  非受管路径永不由 Web 删除。
- `pcap_upload_dir` 必须在 Web 启动时解析为绝对规范路径。单进程和 Guardian 使用同一宿主机绝对目录，
  禁止依赖进程当前工作目录；Docker 将同一 volume 挂载到 Web/Scheduler 的相同绝对位置，例如
  `/opt/flowsql/uploads/pcapfile`。

内部核心引用仅供 Web 使用，不新增公共 ABI：

```cpp
struct ManagedCaptureRef {
    std::string channel_name;
    std::string original_filename;
    std::filesystem::path canonical_path;
    uint64_t size_bytes;
};
```

### Provider 与框架前置条件

- Scheduler 正式运行环境必须同时具备 `libflowsql_npi.so`、可部署的 `protocols.yml` 和
  `libflowsql_pcapfile.so`；NPI 必须收到指向该协议文件的有效 `ldfile` 配置。
- `PcapFilePlugin::Load()` 依赖进程内 `IID_PROTOCOL`。在开始 Web 上传生产实现前，PluginLoader 必须先修复为
  批次执行“所有插件注册/Option → 所有 Load → 所有 Start”，且 Option/Load/Start 任意非零返回值均阻止启动。
- NPI 与 pcapfile 的配置顺序不得影响加载结果；不得以“把 NPI 写在 pcapfile 前面”代替生命周期修复。

## 页面契约

- Stream 通道区域新增“上传 PCAP”入口，打开专用表单：文件、通道名称、格式、batch 大小、回放模式和速度。
- 文件选择限制为 `.pcap,.pcapng`；提交期间禁用重复提交，并展示后端返回的中文可读错误。
- 创建成功后关闭对话框并刷新现有 Stream 通道列表；`pcapfile` 行显示为 `source` 和有限流状态。
- `pcapfile` 行隐藏不适用的通用“编辑”和“重置”，保留删除；删除走受管文件清理语义。
- 普通 Stream、DataFrame 和数据库通道页面行为保持不变。

## 主链路

### 上传成功并创建通道

1. 用户在 Channels 页面选择文件、填写名称和回放参数并提交 multipart 请求。
2. Web 校验字段和大小，在受管目录分块写入临时文件，完成后原子重命名。
3. Web 以绝对受管路径构造小型 `pcapfile` source JSON，经 Gateway 的现有控制面调用 Scheduler。
4. Scheduler 进程内 NPI 已注册 `IID_PROTOCOL`，`PcapFilePlugin` 校验文件头并打开通道；成功后 Web 返回
   已脱敏的通道摘要，页面刷新列表。
5. 后续 Scheduler SQL 通过既有 block stream 链路消费该通道，不经过 Web 传输 packet 数据。

### 失败回滚与删除

1. 上传中断、超过大小或磁盘失败时，Web 终止请求并清理 `.part`；不会调用 Scheduler。
2. Scheduler 拒绝重名、格式错误或 provider 不可用时，Web 删除本次最终文件并透传规范错误。
3. 删除受管 `pcapfile` 通道时，Web 先保存 Scheduler 内部返回的 canonical path；Scheduler 删除成功并释放
   文件句柄后，Web 再校验该路径位于受管根目录并删除 capture。Scheduler 删除失败时保留文件和通道状态。

## 原子任务

- `[ ]` T1：冻结上传/删除 HTTP 契约、`ManagedCaptureRef`/受管路径 helper 接口，先增加字段校验、路径逃逸、
  大小限制、失败回滚和响应脱敏测试锚点；不实现路由。
- `[ ]` T2（P1 前置）：先以 framework 测试锚定插件顺序无关和 `Load()==ENODEV` 阻止启动，再修复
  PluginLoader 为所有 Option → 所有 Load → 所有 Start，并统一按任意非零返回失败。
- `[ ]` T3：实现仅绑定 Web 8081 的 multipart 分块上传、原子创建、失败回滚、受管删除和列表路径脱敏；
  Web 只通过现有小型 JSON 控制请求调用 Scheduler。
- `[ ]` T4：实现前端上传 API 与专用对话框，刷新列表并限制 `pcapfile` 的编辑/重置操作；执行前端构建。
- `[ ]` T5：补齐单进程、Guardian 和 Docker 的 NPI/协议文件/pcapfile provider 与绝对共享目录部署；完成
  真实 pcap/pcapng 上传→创建→Scheduler 消费→删除闭环，更新 `docs/framework.md` 并运行全量回归。

## 测试锚点

| 验收面 | 必测断言 |
| --- | --- |
| HTTP 契约 | multipart 必填字段、默认值、非法 enum/整数、扩展名、重名与 HTTP/JSON 错误映射稳定 |
| 平面隔离 | 上传路由仅存在于 Web 8081 的外部 server，不出现在 `EnumApiRoutes`；Gateway/Router 不接收文件体 |
| 文件所有权 | 分块落盘；成功前只存在 `.part`；超限/中断/写入失败/通道创建失败不遗留文件 |
| 路径安全 | 客户端文件名不能逃逸受管根；只删除规范路径位于受管根内的文件；所有 Web 响应不暴露绝对路径 |
| 插件生命周期 | 所有 Option 先于所有 Load、所有 Load 先于所有 Start；NPI/pcapfile 顺序互换均成功；任意非零返回阻止启动 |
| 端到端 | 上传真实 fixture 后列表可见，Scheduler 可消费并到达 completed；坏格式不会创建通道 |
| 部署回归 | 三类部署加载 NPI/协议定义/pcapfile 且使用同一绝对路径；普通通道与既有 CTest 不回归 |

## 完成出口

1. T1～T5 全部完成，浏览器无需服务器路径即可上传单个 pcap/pcapng 并创建 `pcapfile` source。
2. 大文件按块落盘；失败回滚、受管删除和路径逃逸测试全部通过。
3. 插件生命周期批次执行且非零失败门禁生效；正式单进程、Guardian 和 Docker 均加载 NPI、协议定义和
   pcapfile provider，Web/Scheduler 对同一绝对上传路径具有一致可见性。
4. Web 外部响应不泄露受管绝对路径，Router/Gateway 不承载 capture 文件体。
5. `docs/framework.md` 与实现一致；前端生产构建、标准 CMake 全量构建、相关 CTest 和
   `git diff --check` 全部通过。
