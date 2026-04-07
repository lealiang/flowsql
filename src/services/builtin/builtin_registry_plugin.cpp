/*
 * Copyright (C) 2026 LIHUO
 *
 * Licensed under the MIT License. See LICENSE file in the project root
 * for full license information.
 *
 */

#include "builtin_registry_plugin.h"

#include "builtin_registry.h"

#include <algorithm>
#include <cctype>

namespace flowsql {
namespace builtin {

namespace {

std::string ToLowerAscii(std::string s) {
    std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c) {
        return static_cast<char>(std::tolower(c));
    });
    return s;
}

}  // namespace

int BuiltinRegistryPlugin::Load(IQuerier* querier) {
    querier_ = querier;
    EnsureBuiltinRegistryInitialized();
    return 0;
}

int BuiltinRegistryPlugin::Unload() {
    querier_ = nullptr;
    return 0;
}

int BuiltinRegistryPlugin::FindStreamChannelType(const std::string& type,
                                                 StreamChannelTypeDescriptor* out) {
    if (!out) return -1;
    return BuiltinRegistry::Instance().FindStreamChannelType(type, out) ? 0 : -1;
}

void BuiltinRegistryPlugin::ListStreamChannelTypes(
    std::function<void(const StreamChannelTypeDescriptor&)> callback) {
    if (!callback) return;
    const auto items = BuiltinRegistry::Instance().ListStreamChannelTypes();
    for (const auto& item : items) {
        callback(item);
    }
}

void BuiltinRegistryPlugin::ListBuiltinOperators(
    std::function<void(const BuiltinOperatorDescriptor&)> callback) {
    if (!callback) return;
    const auto items = BuiltinRegistry::Instance().ListBuiltinOperators();
    for (const auto& item : items) {
        callback(item);
    }
}

IOperator* BuiltinRegistryPlugin::CreateBuiltinOperator(const std::string& category,
                                                        const std::string& name) {
    auto items = BuiltinRegistry::Instance().ListBuiltinOperators();
    const std::string target = category + "." + name;
    const std::string target_lower = ToLowerAscii(target);
    for (const auto& item : items) {
        if (ToLowerAscii(item.category + "." + item.name) == target_lower) {
            return item.factory ? item.factory() : nullptr;
        }
    }
    return nullptr;
}

}  // namespace builtin
}  // namespace flowsql
