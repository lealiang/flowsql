/*
 * Copyright (C) 2026 LIHUO
 *
 * Licensed under the MIT License. See LICENSE file in the project root
 * for full license information.
 *
 */

#include "solver_backend.h"

#include <common/error_code.h>

#include <cmath>

#include <Eigen/Core>

namespace flowsql {
namespace baseline {

bool SolverBackend::IsAvailable() {
    return true;
}

int SolverBackend::FitWeightedIntercept(const double* values,
                                        const double* weights,
                                        std::size_t count,
                                        WeightedInterceptFitResult* out) {
    if (!values || !weights || !out || count == 0) return error::BAD_REQUEST;

    Eigen::Map<const Eigen::ArrayXd> value_vec(values, static_cast<Eigen::Index>(count));
    Eigen::Map<const Eigen::ArrayXd> weight_vec(weights, static_cast<Eigen::Index>(count));

    if (!(value_vec.isFinite().all() && weight_vec.isFinite().all())) {
        return error::BAD_REQUEST;
    }
    if ((weight_vec <= 0.0).any()) return error::BAD_REQUEST;

    const double total_weight = weight_vec.sum();
    if (!(std::isfinite(total_weight) && total_weight > 0.0)) {
        return error::BAD_REQUEST;
    }

    out->total_weight = total_weight;
    out->intercept = (value_vec * weight_vec).sum() / total_weight;
    return std::isfinite(out->intercept) ? error::OK : error::BAD_REQUEST;
}

}  // namespace baseline
}  // namespace flowsql
