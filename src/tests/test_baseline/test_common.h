/*
 * Copyright (C) 2026 LIHUO
 *
 * Licensed under the MIT License. See LICENSE file in the project root
 * for full license information.
 *
 */

#ifndef _FLOWSQL_TESTS_TEST_BASELINE_TEST_COMMON_H_
#define _FLOWSQL_TESTS_TEST_BASELINE_TEST_COMMON_H_

#include <cassert>
#include <chrono>
#include <string>
#include <thread>

#include <common/loader.hpp>
#include <framework/interfaces/ibaseline_service.h>
#include <rapidjson/document.h>

namespace flowsql {
namespace baseline_test {

struct LoadedBaselineService {
    PluginLoader* loader = nullptr;
    IBaselineService* service = nullptr;

    ~LoadedBaselineService() {
        if (!loader) return;
        loader->StopAll();
        loader->Unload();
    }
};

inline LoadedBaselineService LoadBaselineService() {
    LoadedBaselineService env;
    env.loader = PluginLoader::Single();

    std::string plugin_dir = get_absolute_process_path();
    std::string plugin_name = "libflowsql_baseline.so";
    const char* relapath[] = {plugin_name.c_str()};
    const char* options[] = {nullptr};

    const int ret = env.loader->Load(plugin_dir.c_str(), relapath, options, 1);
    assert(ret == 0);
    assert(env.loader->StartAll() == 0);

    env.service = static_cast<IBaselineService*>(env.loader->First(IID_BASELINE_SERVICE));
    assert(env.service != nullptr);
    return env;
}

template <typename Predicate>
inline bool WaitUntil(Predicate&& pred, int timeout_ms = 2000) {
    const auto deadline = std::chrono::steady_clock::now() +
                          std::chrono::milliseconds(timeout_ms);
    while (std::chrono::steady_clock::now() < deadline) {
        if (pred()) return true;
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    return pred();
}

inline rapidjson::Document ParseJson(const std::string& json) {
    rapidjson::Document doc;
    doc.Parse(json.c_str());
    assert(!doc.HasParseError());
    assert(doc.IsObject());
    return doc;
}

}  // namespace baseline_test
}  // namespace flowsql

#endif  // _FLOWSQL_TESTS_TEST_BASELINE_TEST_COMMON_H_
