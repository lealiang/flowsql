/*
 * Copyright (C) 2026 LIHUO
 *
 * Licensed under the MIT License. See LICENSE file in the project root
 * for full license information.
 *
 */

#ifndef _FLOWSQL_PLUGINS_BASELINE_ROLLING_MATH_UTILS_H_
#define _FLOWSQL_PLUGINS_BASELINE_ROLLING_MATH_UTILS_H_

#include <algorithm>
#include <cmath>

#include "plugins/baseline/rolling/rolling_config.h"

namespace flowsql {
namespace baseline {

inline double Clamp(double value, double lo, double hi) {
    return std::max(lo, std::min(hi, value));
}

inline double Square(double value) { return value * value; }

inline double CovarianceFloor(const BaselineRollingConfig& config) {
    return config.p_floor_scale * config.sigma_floor * config.sigma_floor;
}

inline double CovarianceCap(const BaselineRollingConfig& config) {
    return config.p_cap_scale * config.sigma_floor * config.sigma_floor;
}

inline double ClampCovariance(double value, const BaselineRollingConfig& config) {
    return Clamp(value, CovarianceFloor(config), CovarianceCap(config));
}

inline double ClampMultiplier(double value,
                              const BaselineRollingConfig& config,
                              double nonfinite_fallback) {
    if (!std::isfinite(value)) return nonfinite_fallback;
    return Clamp(value, config.calibration_multiplier_min, config.calibration_multiplier_max);
}

inline double ClampMultiplier(double value, const BaselineRollingConfig& config) {
    return ClampMultiplier(value, config, config.calibration_multiplier_min);
}

}  // namespace baseline
}  // namespace flowsql

#endif  // _FLOWSQL_PLUGINS_BASELINE_ROLLING_MATH_UTILS_H_
