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
#include <cstdint>
#include <string>
#include <vector>

namespace flowsql {
namespace baseline {

enum class BlockColumnRole : int32_t {
    kUnknown = 0,
    kIntercept = 1,
    kTrend = 2,
    kDaySin = 3,
    kDayCos = 4,
    kWeekSin = 5,
    kWeekCos = 6,
    kMonthDom = 7,
    kMonthDme = 8,
    kMonthLwd = 9,
    kEvent = 10,
};

enum class BlockFitStatus : int32_t {
    kFailed = 0,
    kOk = 1,
    kDegraded = 2,
};

const char* BlockFitStatusName(BlockFitStatus status);

struct BlockFitSpec {
    std::string block_name;
    std::size_t row_count = 0;
    std::size_t col_count = 0;
    std::vector<double> y_target;
    std::vector<double> x_matrix;      // row-major
    std::vector<double> sample_weight;
    std::vector<double> ridge_diag;
    std::vector<double> init_beta;
    std::vector<BlockColumnRole> col_roles;
};

struct BlockSolverConfig {
    std::string solver_name = "weighted_huber_ridge_irls";
    double c_huber = 1.5;
    double s_min_fit = 1e-3;
    uint32_t max_iter_fit = 15;
    double tol_obj_rel = 1e-4;
    double tol_beta_inf = 1e-5;
    double cond_max = 1e8;
};

BlockSolverConfig DefaultBlockSolverConfig();

struct FitBlockResult {
    BlockFitStatus status = BlockFitStatus::kFailed;
    std::vector<double> beta;
    double objective = 0.0;
    double condition_est = 0.0;
    uint32_t iter_count = 0;
};

struct WeightedInterceptFitResult {
    double intercept = 0.0;
    double total_weight = 0.0;
};

class SolverBackend {
 public:
    static bool IsAvailable();
    static int FitWeightedHuberRidgeBlock(const BlockFitSpec& spec,
                                          const BlockSolverConfig& config,
                                          FitBlockResult* out);
    static int FitWeightedIntercept(const double* values,
                                    const double* weights,
                                    std::size_t count,
                                    WeightedInterceptFitResult* out);
};

}  // namespace baseline
}  // namespace flowsql

#endif  // _FLOWSQL_PLUGINS_BASELINE_SOLVER_SOLVER_BACKEND_H_
