# 消除 libflowsql_framework.so — 回归纯插件架构

## 完成清单

- [x] Step 1: 接口头文件拆分（iplugin.h / iquerier.hpp / loader.hpp）
- [x] Step 2: IPlugin::Load(IQuerier*) 签名变更
- [x] Step 3: Scheduler 通道管理内部化 + 算子查找改造
- [x] Step 4: 消除 PluginRegistry + 库重命名（flowsql_framework → flowsql_common）
- [x] Step 5: 编译验证 + 测试（test_framework + test_bridge 全部通过）

## 变更摘要

| 文件 | 操作 |
|------|------|
| common/iplugin.h | 新建：IPlugin、IRegister、注册宏 |
| common/iquerier.hpp | 改造：删除 IQuerierGetter/QuerierGuard，fntraverse 改为 std::function |
| common/loader.hpp | 改造：include iplugin.h，Load 传 IQuerier*，删除 getiquerier() |
| framework/core/plugin_registry.h/.cpp | 删除 |
| framework/core/service.h/.cpp | 删除 |
| framework/CMakeLists.txt | 改造：flowsql_framework → flowsql_common |
| services/scheduler/* | 改造：内部通道管理 + IQuerier 查询算子 |
| services/bridge/* | 适配：Load(IQuerier*)，通过 IRegister 注册算子 |
| services/gateway/* | 适配：Load(IQuerier*)，不再链接 flowsql_common |
| services/web/* | 适配：移除 PluginRegistry 依赖 |
| plugins/example/* | 适配：Load(IQuerier*) |
| plugins/npi/* | 适配：Load(IQuerier*) |
| app/main.cpp | 改造：PluginLoader 替代 PluginRegistry |
| tests/test_framework/main.cpp | 适配：PluginLoader 替代 PluginRegistry |
