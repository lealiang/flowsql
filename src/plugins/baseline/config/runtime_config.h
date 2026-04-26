/*
 * Copyright (C) 2026 LIHUO
 *
 * Licensed under the MIT License. See LICENSE file in the project root
 * for full license information.
 *
 */

#ifndef _FLOWSQL_PLUGINS_BASELINE_CONFIG_RUNTIME_CONFIG_H_
#define _FLOWSQL_PLUGINS_BASELINE_CONFIG_RUNTIME_CONFIG_H_

#include <cstddef>
#include <cstdint>
#include <string>

namespace flowsql {
namespace baseline {

struct SharedProfileConfig;
struct ValueSampledProfileConfig;
struct RatioProfileConfig;
struct BlockSolverConfig;

int LoadBaselineRuntimeConfigFromYaml(const std::string& file_path,
                                      bool strict,
                                      std::string* err);
void ResetBaselineRuntimeConfig();

bool TryGetSharedProfileConfigOverride(SharedProfileConfig* out);
bool TryGetValueSampledProfileConfigOverride(const std::string& profile_name,
                                             ValueSampledProfileConfig* out);
bool TryGetRatioProfileConfigOverride(const std::string& profile_name,
                                      RatioProfileConfig* out);
bool TryGetRatioGlobalNumericalOverride(double* eps_logit,
                                        double* m_floor,
                                        double* v_floor);
bool TryGetBlockSolverConfigOverride(BlockSolverConfig* out);

std::string BaselineDefaultTimezone();
int64_t RuntimeIdlePruneBucketGap();
std::size_t RuntimeIdlePruneScanLimit();

double ScoreWarn();
double ScoreCrit();
double ConfidenceFormalBase();
double ConfidenceSourceBase();
double ConfidenceShadowBase();

double ValueShadowConfidenceCap();
double ValueShadowSigmaScale();
double RatioShadowConfidenceCap();
double RatioShadowScoreScale();

double CandidateHuberDelta();
double CandidateShadowAlpha();
double CandidateRatioVarianceFloor();
double CandidateSwitchLossAbsTol();
std::size_t CandidateMinTrainPointCount();

double KeyFusionPersistenceWindow();
std::size_t KeyFusionWindowLimit();
double RelationPatternLambdaSup();
double RelationPatternLambdaOpp();
double RelationPatternPersistenceWindow();
std::size_t RelationMinReplayForHoldout();
std::size_t RelationSwitchValidationTail();

}  // namespace baseline
}  // namespace flowsql

#endif  // _FLOWSQL_PLUGINS_BASELINE_CONFIG_RUNTIME_CONFIG_H_
