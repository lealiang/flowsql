/*
 * Copyright (C) 2026 LIHUO
 *
 * Licensed under the MIT License. See LICENSE file in the project root
 * for full license information.
 *
 */

#ifndef _FLOWSQL_PLUGINS_BASELINE_SOLVER_SOLVER_BACKEND_H_
#define _FLOWSQL_PLUGINS_BASELINE_SOLVER_SOLVER_BACKEND_H_

#include <cstddef>

namespace flowsql {
namespace baseline {

struct WeightedInterceptFitResult {
    double intercept = 0.0;
    double total_weight = 0.0;
};

class SolverBackend {
 public:
    static bool IsAvailable();
    static int FitWeightedIntercept(const double* values,
                                    const double* weights,
                                    std::size_t count,
                                    WeightedInterceptFitResult* out);
};

}  // namespace baseline
}  // namespace flowsql

#endif  // _FLOWSQL_PLUGINS_BASELINE_SOLVER_SOLVER_BACKEND_H_
