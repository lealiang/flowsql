/*
 * Copyright (C) 2026 LIHUO
 *
 * Licensed under the MIT License. See LICENSE file in the project root
 * for full license information.
 *
 */

#ifndef _FLOWSQL_SERVICES_BUILTIN_BUILTIN_REGISTRY_PLUGIN_H_
#define _FLOWSQL_SERVICES_BUILTIN_BUILTIN_REGISTRY_PLUGIN_H_

#include <common/iplugin.h>
#include <framework/interfaces/ibuiltin_registry.h>

namespace flowsql {
namespace builtin {

class __attribute__((visibility("default"))) BuiltinRegistryPlugin : public IPlugin,
                                                                      public IBuiltinRegistry {
 public:
    BuiltinRegistryPlugin() = default;
    ~BuiltinRegistryPlugin() override = default;

    // IPlugin
    int Load(IQuerier* querier) override;
    int Unload() override;

    // IBuiltinRegistry
    int FindStreamChannelType(const std::string& type,
                              StreamChannelTypeDescriptor* out) override;
    void ListStreamChannelTypes(
        std::function<void(const StreamChannelTypeDescriptor&)> callback) override;

    void ListBuiltinOperators(
        std::function<void(const BuiltinOperatorDescriptor&)> callback) override;
    IOperator* CreateBuiltinOperator(const std::string& category,
                                     const std::string& name) override;

 private:
    IQuerier* querier_ = nullptr;
};

}  // namespace builtin
}  // namespace flowsql

#endif  // _FLOWSQL_SERVICES_BUILTIN_BUILTIN_REGISTRY_PLUGIN_H_
