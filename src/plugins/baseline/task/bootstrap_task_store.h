/*
 * Copyright (C) 2026 LIHUO
 *
 * Licensed under the MIT License. See LICENSE file in the project root
 * for full license information.
 *
 */

#ifndef _FLOWSQL_PLUGINS_BASELINE_TASK_BOOTSTRAP_TASK_STORE_H_
#define _FLOWSQL_PLUGINS_BASELINE_TASK_BOOTSTRAP_TASK_STORE_H_

#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

#include "plugins/baseline/bootstrap/bootstrap_engine.h"
#include "plugins/baseline/model/task_spec.h"

namespace flowsql {
namespace baseline {

using BootstrapArtifactStore = std::unordered_map<std::string, BootstrapArtifact>;
using BootstrapSeedStore = std::unordered_map<std::string, BootstrapSeed>;

BaselineStatus StoreBootstrapArtifact(std::string_view series_key,
                                      BootstrapArtifact artifact,
                                      const BootstrapEngine& engine,
                                      BootstrapArtifactStore* artifacts,
                                      BootstrapSeedStore* seeds);

const BootstrapArtifact* FindBootstrapArtifact(
    const BootstrapArtifactStore& artifacts,
    std::string_view series_key);

std::vector<const BootstrapArtifact*> SortedBootstrapArtifacts(
    const BootstrapArtifactStore& artifacts);

BaselineSerializationResult ExportBootstrapArtifactStore(
    const BootstrapArtifactStore& artifacts,
    const BootstrapEngine& engine,
    BaselineSerializationFormat format);

BaselineSerializationResult ExportBootstrapSeedStore(
    const BootstrapSeedStore& seeds,
    const BootstrapEngine& engine,
    BaselineSerializationFormat format);

BaselineStatus LoadBootstrapArtifactStore(
    std::string_view content,
    BaselineSerializationFormat format,
    const BootstrapEngine& engine,
    const BaselineTaskSpec& spec,
    BootstrapArtifactKind expected_kind,
    BootstrapArtifactStore* artifacts,
    BootstrapSeedStore* seeds);

BaselineStatus LoadRelationBootstrapArtifactStore(
    std::string_view content,
    BaselineSerializationFormat format,
    const BootstrapEngine& engine,
    const RelationTaskCreateSpec& spec,
    BootstrapArtifactStore* artifacts,
    BootstrapSeedStore* seeds);

}  // namespace baseline
}  // namespace flowsql

#endif  // _FLOWSQL_PLUGINS_BASELINE_TASK_BOOTSTRAP_TASK_STORE_H_
