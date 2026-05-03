/*
 * Copyright (C) 2026 LIHUO
 *
 * Licensed under the MIT License. See LICENSE file in the project root
 * for full license information.
 *
 */

#ifndef _FLOWSQL_PLUGINS_BASELINE_RELATION_ROUTED_SUMMARY_H_
#define _FLOWSQL_PLUGINS_BASELINE_RELATION_ROUTED_SUMMARY_H_

#include <framework/interfaces/ibaseline_types.h>

#include <cstdint>
#include <string>
#include <string_view>

#include "plugins/baseline/bootstrap/bootstrap_types.h"
#include "plugins/baseline/model/task_spec.h"

namespace flowsql {
namespace baseline {

struct RelationRoutedSummaryIdentity {
    std::string source_series_key;
    std::string metric_name;
    std::string summary_name;
    std::string feature_type;
    bool basis_scoped = false;
    uint64_t basis_version = 0;
    std::string routed_series_key;
};

struct RelationRoutedBootstrapSeedMaterialization {
    std::string routed_series_key;
    BaselineTaskSpec task_spec;
    BootstrapSeed seed;
};

const char* RelationSummaryFeatureType(BaselineTaskKind task_kind);
bool IsBasisScopedRelationSummary(std::string_view summary_name);

RelationRoutedSummaryIdentity MakeRelationRoutedSummaryIdentity(
    std::string_view source_series_key,
    std::string_view metric_name,
    std::string_view summary_name,
    BaselineTaskKind task_kind,
    uint64_t basis_version);

BaselineTaskSpec MakeRoutedSummaryTaskSpec(
    const RelationTaskCreateSpec& spec,
    const std::string& metric_name,
    const std::string& summary_name,
    BaselineTaskKind task_kind);

BaselineStatus MaterializeRelationRoutedBootstrapSeed(
    const RelationTaskCreateSpec& spec,
    std::string_view source_series_key,
    const RelationRoutedBootstrapSeed& routed_seed,
    uint64_t fallback_basis_version,
    RelationRoutedBootstrapSeedMaterialization* out);

}  // namespace baseline
}  // namespace flowsql

#endif  // _FLOWSQL_PLUGINS_BASELINE_RELATION_ROUTED_SUMMARY_H_
