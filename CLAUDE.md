# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## 交流规则

1. 全程使用中文交流
2. 需要编译时直接编译，无需确认
3. 移动或删除目录前需要用户确认
4. 不要自动提交代码（git commit/push），等用户明确发起提交指令后再执行

## 构建

```bash
cd src/build
cmake ..
make -j$(nproc)
```

构建产物（可执行文件和 .so）输出到 `src/build/output/`。首次构建会通过 ExternalProject 下载第三方依赖，需要网络。

清理重建：`cd src/build && rm -rf * && cmake .. && make -j$(nproc)`

## 运行测试

```bash
./src/build/output/test_npi
```

## 架构

FlowSQL 是基于 SQL 语法的网络流量分析平台，采用插件化架构。

### 插件系统（两套机制）

**轻量级 PluginLoader**（`common/loader.hpp`）— 当前 NPI 插件使用此机制：
- 插件编译为 .so，导出 `pluginregist` / `pluginunregist` 符号
- 通过 GUID 注册接口，`IQuerier` 按 GUID 查询/遍历接口实例
- 注册宏：`BEGIN_PLUGIN_REGIST(ClassName)` → `____INTERFACE(IID, InterfaceName)` → `END_PLUGIN_REGIST()`

**模块驱动 MDriver**（`common/modularity/modularity.h`）— 更完整的生命周期管理：
- `InterfaceRegister` 管理模块和接口的注册/查询，支持 shared_ptr 生命周期
- `MDriver` 负责加载 .so、调用 `RegisterInterface` 导出函数、驱动 Load/Unload

### NPI 插件（协议识别）

位于 `src/plugins/protocol/npi/`，实现 `IProtocol` 接口（IID_PROTOCOL）：
- `Engine` — 协议识别引擎，组合三种匹配器
- `NetworkLayer` — 网络层解析（Ethernet → IP → TCP/UDP 等）
- `Config` — 从 YAML（`conf/protocols.yml`）加载协议定义
- 三种匹配策略：`BitmapMatch`（位图）、`EnumerateMatch`（枚举）、`RegexMatch`（Hyperscan 正则）

### 第三方依赖

通过 `src/thirdparts/` 下的 `*-config.cmake` 文件以 ExternalProject_Add 管理。`subjects.cmake` 中的 `add_thirddepen(TARGET lib1 lib2 ...)` 宏自动处理 include/link 目录和库链接。

依赖列表：gflags、glog、hyperscan（正则引擎）、rapidjson、yaml-cpp。

### 关键目录

- `src/common/` — 公共库：算法（AhoCorasick、RadixTree、Bitmap）、网络包结构、IPC、共享内存、线程安全原语、反射
- `src/plugins/` — 插件实现，按功能分目录
- `src/tests/` — 测试程序
- `src/data/packets/` — 测试用 pcap 抓包文件

## 代码风格

- C++17，命名空间 `flowsql`（子空间如 `flowsql::protocol`）
- clang-format 配置在 `src/.clang-format`：Google 风格基础上 4 空格缩进、120 列宽
- `interface` 宏定义为 `struct`（见 `common/define.h`），用于声明纯虚接口
