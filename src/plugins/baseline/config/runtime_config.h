/*
 * Copyright (C) 2026 LIHUO
 *
 * Licensed under the MIT License. See LICENSE file in the project root
 * for full license information.
 *
 */

#ifndef _FLOWSQL_PLUGINS_BASELINE_CONFIG_RUNTIME_CONFIG_H_
#define _FLOWSQL_PLUGINS_BASELINE_CONFIG_RUNTIME_CONFIG_H_

#include <memory>
#include <string>

namespace flowsql {
namespace baseline {

struct BaselineCalendarRef;
struct CompiledEventCalendar;
struct SharedProfileConfig;
struct ValueSampledProfileConfig;
struct RatioProfileConfig;
struct BlockSolverConfig;
struct BaselineRollingConfig;

int __attribute__((visibility("default"))) LoadBaselineRuntimeConfigFromYaml(
    const std::string& file_path,
    bool strict,
    std::string* err);
void __attribute__((visibility("default"))) ResetBaselineRuntimeConfig();

bool TryGetSharedProfileConfigOverride(SharedProfileConfig* out);
bool TryGetValueSampledProfileConfigOverride(const std::string& profile_name,
                                             ValueSampledProfileConfig* out);
bool TryGetRatioProfileConfigOverride(const std::string& profile_name,
                                      RatioProfileConfig* out);
bool TryGetRatioGlobalNumericalOverride(double* eps_logit,
                                        double* m_floor,
                                        double* v_floor);
bool TryGetBlockSolverConfigOverride(BlockSolverConfig* out);
bool TryGetBaselineRollingConfigOverride(BaselineRollingConfig* out);

std::string BaselineDefaultTimezone();
std::shared_ptr<const CompiledEventCalendar> FindBaselineEventCalendar(
    const BaselineCalendarRef& calendar_ref);

}  // namespace baseline
}  // namespace flowsql

#endif  // _FLOWSQL_PLUGINS_BASELINE_CONFIG_RUNTIME_CONFIG_H_
