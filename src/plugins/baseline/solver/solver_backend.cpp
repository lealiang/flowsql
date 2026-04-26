/*
 * Copyright (C) 2026 LIHUO
 *
 * Licensed under the MIT License. See LICENSE file in the project root
 * for full license information.
 *
 */

#include "solver_backend.h"

#include <common/error_code.h>

#include <algorithm>
#include <cmath>
#include <limits>
#include <vector>

#include <Eigen/Core>
#include <Eigen/Cholesky>
#include <Eigen/Eigenvalues>
#include <Eigen/QR>

#include "plugins/baseline/config/runtime_config.h"

namespace flowsql {
namespace baseline {

namespace {

using RowMajorMatrixXd =
    Eigen::Matrix<double, Eigen::Dynamic, Eigen::Dynamic, Eigen::RowMajor>;

constexpr double kConditionFloor = 1e-12;

double UpperMedian(std::vector<double>* values) {
    if (!values || values->empty()) return 0.0;
    auto middle = values->begin() + static_cast<std::ptrdiff_t>(values->size() / 2);
    std::nth_element(values->begin(), middle, values->end());
    return *middle;
}

double EstimateRobustScale(const Eigen::VectorXd& residual,
                           double scale_floor) {
    if (residual.size() == 0) return scale_floor;

    std::vector<double> values(residual.data(), residual.data() + residual.size());
    const double median = UpperMedian(&values);

    std::vector<double> deviation;
    deviation.reserve(values.size());
    for (double value : values) {
        deviation.push_back(std::fabs(value - median));
    }
    return std::max(scale_floor, 1.4826 * UpperMedian(&deviation));
}

double ComputeHuberObjective(const Eigen::VectorXd& residual,
                             const Eigen::VectorXd& sample_weight,
                             const Eigen::VectorXd& ridge_diag,
                             const Eigen::VectorXd& beta,
                             double huber_delta) {
    double objective = 0.0;
    for (Eigen::Index i = 0; i < residual.size(); ++i) {
        const double abs_residual = std::fabs(residual[i]);
        const double loss =
            abs_residual <= huber_delta
                ? 0.5 * abs_residual * abs_residual
                : huber_delta * (abs_residual - 0.5 * huber_delta);
        objective += sample_weight[i] * loss;
    }
    objective += 0.5 * (ridge_diag.array() * beta.array().square()).sum();
    return objective;
}

bool SolveWeightedRidge(const Eigen::Ref<const Eigen::VectorXd>& y,
                        const Eigen::Ref<const RowMajorMatrixXd>& x,
                        const Eigen::Ref<const Eigen::VectorXd>& weight,
                        const Eigen::Ref<const Eigen::VectorXd>& ridge_diag,
                        Eigen::VectorXd* out_beta,
                        double* out_condition_est) {
    if (!out_beta) return false;

    const Eigen::ArrayXd sqrt_weight = weight.array().sqrt();
    const RowMajorMatrixXd weighted_x = x.array().colwise() * sqrt_weight;
    const Eigen::VectorXd weighted_y = y.array() * sqrt_weight;

    Eigen::MatrixXd normal = weighted_x.transpose() * weighted_x;
    normal.diagonal().array() += ridge_diag.array();
    const Eigen::VectorXd rhs = weighted_x.transpose() * weighted_y;

    double condition_est = 0.0;
    if (normal.rows() > 0) {
        Eigen::SelfAdjointEigenSolver<Eigen::MatrixXd> eigen_solver(normal);
        if (eigen_solver.info() == Eigen::Success) {
            const auto eigenvalues = eigen_solver.eigenvalues();
            const double max_eval = eigenvalues.maxCoeff();
            const double min_eval = std::max(kConditionFloor, eigenvalues.minCoeff());
            condition_est =
                (std::isfinite(max_eval) && std::isfinite(min_eval) && min_eval > 0.0)
                    ? (max_eval / min_eval)
                    : std::numeric_limits<double>::infinity();
        }
    }

    Eigen::LDLT<Eigen::MatrixXd> ldlt(normal);
    if (ldlt.info() == Eigen::Success) {
        Eigen::VectorXd beta = ldlt.solve(rhs);
        if (ldlt.info() == Eigen::Success && beta.array().isFinite().all()) {
            *out_beta = std::move(beta);
            if (out_condition_est) *out_condition_est = condition_est;
            return true;
        }
    }

    Eigen::ColPivHouseholderQR<Eigen::MatrixXd> qr(normal);
    Eigen::VectorXd beta = qr.solve(rhs);
    if (!beta.array().isFinite().all()) return false;

    *out_beta = std::move(beta);
    if (out_condition_est) *out_condition_est = condition_est;
    return true;
}

bool ValidateBlockSpec(const BlockFitSpec& spec) {
    if (spec.row_count == 0 || spec.col_count == 0) return false;
    if (spec.y_target.size() != spec.row_count) return false;
    if (spec.x_matrix.size() != spec.row_count * spec.col_count) return false;
    if (spec.sample_weight.size() != spec.row_count) return false;
    if (spec.ridge_diag.size() != spec.col_count) return false;
    if (!spec.init_beta.empty() && spec.init_beta.size() != spec.col_count) return false;
    if (!spec.col_roles.empty() && spec.col_roles.size() != spec.col_count) return false;
    return true;
}

}  // namespace

const char* BlockFitStatusName(BlockFitStatus status) {
    switch (status) {
        case BlockFitStatus::kOk:
            return "ok";
        case BlockFitStatus::kDegraded:
            return "degraded";
        case BlockFitStatus::kFailed:
            return "failed";
    }
    return "failed";
}

BlockSolverConfig DefaultBlockSolverConfig() {
    BlockSolverConfig config;
    (void)TryGetBlockSolverConfigOverride(&config);
    return config;
}

bool SolverBackend::IsAvailable() {
    return true;
}

int SolverBackend::FitWeightedHuberRidgeBlock(const BlockFitSpec& spec,
                                              const BlockSolverConfig& config,
                                              FitBlockResult* out) {
    if (!out) return error::BAD_REQUEST;
    *out = FitBlockResult{};
    if (!ValidateBlockSpec(spec)) return error::BAD_REQUEST;
    if (config.max_iter_fit == 0 || config.s_min_fit <= 0.0 || config.c_huber <= 0.0) {
        return error::BAD_REQUEST;
    }

    Eigen::Map<const Eigen::VectorXd> y(spec.y_target.data(), static_cast<Eigen::Index>(spec.row_count));
    Eigen::Map<const RowMajorMatrixXd> x(
        spec.x_matrix.data(),
        static_cast<Eigen::Index>(spec.row_count),
        static_cast<Eigen::Index>(spec.col_count));
    Eigen::Map<const Eigen::VectorXd> sample_weight(
        spec.sample_weight.data(), static_cast<Eigen::Index>(spec.row_count));
    Eigen::Map<const Eigen::VectorXd> ridge_diag(
        spec.ridge_diag.data(), static_cast<Eigen::Index>(spec.col_count));

    if (!(y.array().isFinite().all() && x.array().isFinite().all() &&
          sample_weight.array().isFinite().all() && ridge_diag.array().isFinite().all())) {
        return error::BAD_REQUEST;
    }
    if ((sample_weight.array() <= 0.0).any() || (ridge_diag.array() < 0.0).any()) {
        return error::BAD_REQUEST;
    }

    Eigen::VectorXd beta(static_cast<Eigen::Index>(spec.col_count));
    if (!spec.init_beta.empty()) {
        Eigen::Map<const Eigen::VectorXd> init_beta(
            spec.init_beta.data(), static_cast<Eigen::Index>(spec.col_count));
        if (!init_beta.array().isFinite().all()) return error::BAD_REQUEST;
        beta = init_beta;
    } else {
        beta.setZero();
    }

    double condition_est = 0.0;
    if (!SolveWeightedRidge(y, x, sample_weight, ridge_diag, &beta, &condition_est)) {
        return error::UNAVAILABLE;
    }

    Eigen::VectorXd residual = y - x * beta;
    double scale = EstimateRobustScale(residual, config.s_min_fit);
    double huber_delta = config.c_huber * scale;
    double last_objective =
        ComputeHuberObjective(residual, sample_weight, ridge_diag, beta, huber_delta);
    bool converged = false;

    for (uint32_t iter = 0; iter < config.max_iter_fit; ++iter) {
        Eigen::ArrayXd robust_weight = Eigen::ArrayXd::Ones(residual.size());
        for (Eigen::Index i = 0; i < residual.size(); ++i) {
            const double abs_residual = std::fabs(residual[i]);
            if (abs_residual > huber_delta && abs_residual > 0.0) {
                robust_weight[i] = huber_delta / abs_residual;
            }
        }

        const Eigen::VectorXd total_weight =
            (sample_weight.array() * robust_weight).matrix();
        Eigen::VectorXd next_beta;
        double next_condition_est = condition_est;
        if (!SolveWeightedRidge(y, x, total_weight, ridge_diag, &next_beta, &next_condition_est)) {
            return error::UNAVAILABLE;
        }

        const Eigen::VectorXd next_residual = y - x * next_beta;
        scale = EstimateRobustScale(next_residual, config.s_min_fit);
        huber_delta = config.c_huber * scale;
        const double next_objective =
            ComputeHuberObjective(next_residual, sample_weight, ridge_diag, next_beta, huber_delta);
        const double objective_denom = std::max(1.0, std::fabs(last_objective));
        const double rel_objective = std::fabs(last_objective - next_objective) / objective_denom;
        const double beta_inf = (next_beta - beta).lpNorm<Eigen::Infinity>();

        beta = std::move(next_beta);
        residual = std::move(next_residual);
        condition_est = next_condition_est;
        last_objective = next_objective;
        out->iter_count = iter + 1;

        if (rel_objective <= config.tol_obj_rel && beta_inf <= config.tol_beta_inf) {
            converged = true;
            break;
        }
    }

    if (!beta.array().isFinite().all() || !std::isfinite(last_objective)) {
        return error::UNAVAILABLE;
    }

    out->beta.assign(beta.data(), beta.data() + beta.size());
    out->objective = last_objective;
    out->condition_est = condition_est;
    out->status =
        (converged && std::isfinite(condition_est) && condition_est <= config.cond_max)
            ? BlockFitStatus::kOk
            : BlockFitStatus::kDegraded;
    return error::OK;
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
