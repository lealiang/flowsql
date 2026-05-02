/*
 * Copyright (C) 2026 LIHUO
 *
 * Licensed under the MIT License. See LICENSE file in the project root
 * for full license information.
 *
 */

#include <cassert>
#include <cmath>

#include <plugins/baseline/rolling/drift_adapt.h>
#include <plugins/baseline/rolling/residual_scale.h>
#include <plugins/baseline/rolling/update_gate.h>

using namespace flowsql;
using namespace flowsql::baseline;

namespace {

void AssertNear(double actual, double expected, double eps = 1.0e-12) {
    assert(std::fabs(actual - expected) < eps);
}

void TestUpdateGateWeights() {
    BaselineRollingConfig config;
    config.z_downweight = 3.0;
    config.z_skip = 5.0;
    config.small_update_weight = 0.2;
    config.skip_relax = 2.0;

    UpdateGateResult normal = ComputeUpdateGate(2.0, 0.0, config);
    assert(!normal.skip_update);
    assert(!normal.downweight_update);
    AssertNear(normal.gate_update_weight, 1.0);
    AssertNear(normal.skip_threshold, 5.0);

    UpdateGateResult downweighted = ComputeUpdateGate(3.5, 0.0, config);
    assert(!downweighted.skip_update);
    assert(downweighted.downweight_update);
    AssertNear(downweighted.gate_update_weight, 0.2);

    UpdateGateResult skipped = ComputeUpdateGate(5.1, 0.0, config);
    assert(skipped.skip_update);
    AssertNear(skipped.gate_update_weight, 0.0);

    UpdateGateResult relaxed = ComputeUpdateGate(5.1, 1.0, config);
    assert(!relaxed.skip_update);
    assert(relaxed.downweight_update);
    AssertNear(relaxed.skip_threshold, 7.0);

    UpdateGateResult drift_learning = ComputeUpdateGate(9.0, 1.0, config);
    assert(!drift_learning.skip_update);
    assert(drift_learning.downweight_update);
    AssertNear(drift_learning.gate_update_weight, 0.2);
}

void TestDriftEvidenceUsesShortLongEwma() {
    BaselineRollingConfig config;
    config.alpha_short = 0.5;
    config.alpha_long = 0.1;
    config.z_cap = 5.0;
    config.drift_start = 0.2;
    config.drift_full = 0.8;

    RollingState state;
    DriftAdaptResult result;
    assert(UpdateDriftEvidence(2.0, 1.0, true, config, &state, &result) ==
           BaselineStatus::kOk);
    AssertNear(state.short_ewma, 1.0);
    AssertNear(state.long_ewma, 0.2);
    AssertNear(state.drift_evidence, 0.8);
    AssertNear(result.adapt_boost, 1.0);

    assert(UpdateDriftEvidence(100.0, 1.0, true, config, &state, &result) ==
           BaselineStatus::kOk);
    assert(std::fabs(result.resid_norm) <= config.z_cap);
}

void TestDriftEvidenceCanBeDisabledWhenScoreUnavailable() {
    BaselineRollingConfig config;
    RollingState state;
    state.short_ewma = 1.0;
    state.long_ewma = 0.5;
    state.drift_evidence = 0.5;

    DriftAdaptResult result;
    assert(UpdateDriftEvidence(10.0, 1.0, false, config, &state, &result) ==
           BaselineStatus::kOk);
    AssertNear(state.short_ewma, 1.0);
    AssertNear(state.long_ewma, 0.5);
    AssertNear(result.adapt_boost, 0.0);
}

void TestLevelShiftEvidenceAccumulatesSameSideResiduals() {
    BaselineRollingConfig config;
    config.alpha_short = 0.02;
    config.alpha_long = 0.01;
    config.z_cap = 5.0;
    config.drift_start = 1.0;
    config.drift_full = 2.0;
    config.level_shift_reference_z = 0.5;
    config.level_shift_cusum_decay = 0.98;
    config.level_shift_cusum_threshold = 4.0;

    RollingState state;
    DriftAdaptResult result;
    for (int i = 0; i < 8; ++i) {
        assert(UpdateDriftEvidence(1.5, 1.0, true, config, &state, &result) ==
               BaselineStatus::kOk);
    }

    assert(std::fabs(state.drift_evidence) < config.drift_start);
    assert(state.level_shift_evidence >= config.drift_start);
    assert(result.level_shift_evidence >= config.drift_start);
    assert(result.combined_drift_evidence == result.level_shift_evidence);
    assert(result.adapt_boost > 0.0);

    assert(UpdateDriftEvidence(-2.0, 1.0, true, config, &state, &result) ==
           BaselineStatus::kOk);
    assert(std::isfinite(result.level_shift_evidence));
}

void TestResidualScaleUpdate() {
    BaselineRollingConfig config;
    config.alpha_sigma = 0.5;
    config.c_sigma = 2.0;
    config.sigma_floor = 0.1;

    RollingState state;
    state.sigma = 1.0;
    ResidualScaleResult result;
    assert(UpdateResidualScale(10.0, 1.0, config, &state, &result) == BaselineStatus::kOk);
    AssertNear(result.clipped_resid2, 4.0);
    AssertNear(state.sigma, std::sqrt(2.5));

    const double before = state.sigma;
    assert(UpdateResidualScale(0.0, 0.0, config, &state, &result) == BaselineStatus::kOk);
    AssertNear(state.sigma, before);

    state.sigma = 0.11;
    assert(UpdateResidualScale(0.0, 1.0, config, &state, &result) == BaselineStatus::kOk);
    assert(state.sigma >= config.sigma_floor);
}

}  // namespace

int main() {
    TestUpdateGateWeights();
    TestDriftEvidenceUsesShortLongEwma();
    TestDriftEvidenceCanBeDisabledWhenScoreUnavailable();
    TestLevelShiftEvidenceAccumulatesSameSideResiduals();
    TestResidualScaleUpdate();
    return 0;
}
