/*
 * Copyright (C) 2026 LIHUO
 *
 * Licensed under the MIT License. See LICENSE file in the project root
 * for full license information.
 *
 */

#ifndef _FLOWSQL_SERVICES_BUILTIN_BUILTIN_REGISTRY_H_
#define _FLOWSQL_SERVICES_BUILTIN_BUILTIN_REGISTRY_H_

#include <framework/interfaces/ibuiltin_registry.h>

#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

namespace flowsql {

class BuiltinRegistry {
 public:
    static BuiltinRegistry& Instance();

    int RegisterStreamChannelType(const StreamChannelTypeDescriptor& desc);
    int RegisterBuiltinOperator(const BuiltinOperatorDescriptor& desc);

    bool FindStreamChannelType(const std::string& type,
                               StreamChannelTypeDescriptor* out) const;
    std::vector<StreamChannelTypeDescriptor> ListStreamChannelTypes() const;
    std::vector<BuiltinOperatorDescriptor> ListBuiltinOperators() const;

 private:
    BuiltinRegistry() = default;

    mutable std::mutex mu_;
    std::unordered_map<std::string, StreamChannelTypeDescriptor> stream_types_;
    std::vector<BuiltinOperatorDescriptor> operators_;
    std::unordered_map<std::string, size_t> operator_index_;
};

void EnsureBuiltinRegistryInitialized();
std::string NormalizeRole(const std::string& role);
bool IsRoleAllowed(const std::string& role,
                   const std::vector<std::string>& allowed_roles);

}  // namespace flowsql

#endif  // _FLOWSQL_SERVICES_BUILTIN_BUILTIN_REGISTRY_H_
