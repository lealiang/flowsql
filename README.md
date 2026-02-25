# FlowSQL

基于SQL语法的网络流量分析共创平台

## 项目简介

FlowSQL 是一个全栈式网络流量分析平台，通过扩展的SQL语法提供从数据采集、流量分析到数据探索的完整能力。用户无需深入了解底层技术（如DPDK、Hyperscan），只需使用熟悉的SQL语句即可构建自己的流量分析系统。

## 核心特性

- **数据采集**：通过SQL语句从网卡或存储空间采集网络流量
- **NPM分析**：实时或事后的网络性能监控分析
- **数据查询**：标准SQL查询语法支持
- **统计分析**：描述性统计分析能力
- **探索性分析**：支持PCA、朴素贝叶斯等机器学习算法
- **模型助手**：提供数据预处理和神经网络模型训练

## 快速开始

### 环境要求

- CMake 3.12+
- C++17 编译器
- Linux 系统

### 编译

```bash
cd src
mkdir -p build
cd build
cmake ..
make
```

## SQL语法示例

### 数据采集
```sql
select *packet* from *netcard.nic1, netcard.nic2* into *ss.packets*
```

### NPM分析
```sql
npm.basic packet from *netcard.nic1, netcard.nic2* into *ts.npm*
```

### 数据查询
```sql
select * from *ts.npm.tcp_session* where ip = '8.8.8.8'
```

### 统计分析
```sql
statistic.hist bps from *ts.npm.tcp_session*
where time = '[2024/07/14 00:00:00 - 2024/07/14 23:59:59]'
group by application granularity 60
```

### 探索性分析
```sql
explore.pca bps,rss,connect_suc_rate from *ts.npm.tcp_session*
where time = '[2024/07/14 00:00:00 - 2024/07/14 23:59:59]'
group by application granularity 60
```

## 扩展关键字

| 关键字 | 描述 |
|--------|------|
| 通道 | netcard、kafka、ss、ts、smq |
| 算子 | npm、statistic、explore、model |
| granularity | 数据聚合粒度（秒） |
| into | 指定数据输出目标 |
| label | 模型训练标签字段 |
| with | 算子或模型附加参数 |

## 项目结构

```
flowSQL/
├── src/              # 源代码
│   ├── common/       # 公共模块
│   ├── plugins/      # 插件模块
│   ├── tests/        # 测试代码
│   └── thirdparts/   # 第三方依赖
├── docs/             # 文档
└── CLAUDE.md         # AI助手指引
```

## 文档

详细设计文档请参考 [docs/design.md](docs/design.md)

## 许可证

本项目采用 MIT 许可证 - 详见 [LICENSE](LICENSE) 文件

## 贡献

欢迎提交 Issue 和 Pull Request

## 联系方式

项目地址：https://github.com/lealiang/flowsql
