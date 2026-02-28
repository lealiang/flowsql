# Vue.js 前端验证指南

## 启动服务器

```bash
cd /mnt/d/working/flowSQL/src/build/output
./flowsql_web
```

服务器将在 `http://localhost:8081` 启动。

## 访问前端

在浏览器中打开：`http://localhost:8081`

## 功能验证

### 1. 仪表盘（Dashboard）

访问 `http://localhost:8081/dashboard`

验证内容：
- 显示通道总数（应该显示 2：test.data 和 example.memory）
- 显示算子统计（活跃/总数）
- 显示任务统计（成功/总数）
- 显示最近任务列表（最新 5 条）

### 2. 通道列表（Channels）

访问 `http://localhost:8081/channels`

验证内容：
- 表格显示所有通道
- test.data 通道应显示 7 个字段（src_ip, dst_ip, src_port, dst_port, protocol, bytes_sent, bytes_recv）
- 鼠标悬停在 Schema 标签上可查看字段详情
- 搜索框可过滤通道

### 3. 算子管理（Operators）

访问 `http://localhost:8081/operators`

验证内容：
- 表格显示所有算子
- example.passthrough 算子应显示为活跃状态
- 点击"停用"按钮可停用算子
- 点击"激活"按钮可重新激活算子
- 点击"上传算子"可上传新的 Python 算子文件（.py）

### 4. SQL 工作台（Tasks）

访问 `http://localhost:8081/tasks`

验证内容：

#### 测试 SQL 1：基本查询
```sql
SELECT * FROM test.data USING example.passthrough
```

预期结果：
- 显示 20 行数据
- 7 列：src_ip, dst_ip, src_port, dst_port, protocol, bytes_sent, bytes_recv
- 数据包含网络流量信息

#### 测试 SQL 2：带参数的查询（需要先上传 Python 算子）
```sql
SELECT * FROM test.data USING explore.chisquare WITH threshold=0.05
```

注意：需要先在"算子管理"页面上传 `explore.chisquare` 算子。

#### 任务历史
- 下方表格显示所有历史任务
- 点击"查看结果"按钮可在弹窗中查看任务结果
- 任务状态显示为"success"或"failed"

## 开发模式

如果需要修改前端代码并实时预览：

```bash
cd /mnt/d/working/flowSQL/src/web-ui
npm run dev
```

开发服务器将在 `http://localhost:5173` 启动，API 请求会自动代理到 `http://localhost:8081`。

## 生产构建

修改完成后，重新构建并部署：

```bash
cd /mnt/d/working/flowSQL/src/web-ui
npm run build
cp -r dist/* ../web/static/
```

## 技术栈

- Vue 3（Composition API）
- Vite（构建工具）
- Vue Router（路由）
- Axios（HTTP 客户端）
- Element Plus（UI 组件库）

## 目录结构

```
src/web-ui/
├── src/
│   ├── main.js              # 入口文件
│   ├── App.vue              # 根组件
│   ├── router/index.js      # 路由配置
│   ├── api/index.js         # API 封装
│   ├── views/               # 页面组件
│   │   ├── Dashboard.vue
│   │   ├── Channels.vue
│   │   ├── Operators.vue
│   │   └── Tasks.vue
│   └── components/          # 公共组件
│       └── Sidebar.vue
├── package.json
├── vite.config.js
└── index.html
```

## 已知问题

1. 上传算子功能需要确保 `/tmp/flowsql_operators/` 目录存在且可写
2. Element Plus 打包后体积较大（~350KB），可考虑按需引入优化
3. SQL 编辑器目前使用简单 textarea，后续可升级为 Monaco Editor

## 后续优化建议

1. 添加代码分割（Code Splitting）减小初始加载体积
2. 添加 SQL 语法高亮和自动补全
3. 添加任务执行进度显示
4. 添加数据可视化图表
5. 添加用户认证和权限管理
