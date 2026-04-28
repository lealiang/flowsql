project(test_baseline)
find_package(ICU REQUIRED COMPONENTS uc i18n)

add_executable(${PROJECT_NAME} test_baseline_plugin.cpp)
add_executable(test_baseline_value_task
    test_baseline_value_task.cpp
    ${CMAKE_SOURCE_DIR}/plugins/baseline/config/runtime_config.cpp
    ${CMAKE_SOURCE_DIR}/plugins/baseline/detector/value_detector_core.cpp
    ${CMAKE_SOURCE_DIR}/plugins/baseline/model/calendar_feature_helper.cpp
    ${CMAKE_SOURCE_DIR}/plugins/baseline/model/event_calendar_matcher.cpp
    ${CMAKE_SOURCE_DIR}/plugins/baseline/model/formal_predictor.cpp
    ${CMAKE_SOURCE_DIR}/plugins/baseline/model/readiness_helper.cpp
    ${CMAKE_SOURCE_DIR}/plugins/baseline/model/series_store.cpp
)
add_executable(test_baseline_ratio_task
    test_baseline_ratio_task.cpp
    ${CMAKE_SOURCE_DIR}/plugins/baseline/config/runtime_config.cpp
    ${CMAKE_SOURCE_DIR}/plugins/baseline/detector/ratio_detector_core.cpp
    ${CMAKE_SOURCE_DIR}/plugins/baseline/model/calendar_feature_helper.cpp
    ${CMAKE_SOURCE_DIR}/plugins/baseline/model/event_calendar_matcher.cpp
    ${CMAKE_SOURCE_DIR}/plugins/baseline/model/formal_predictor.cpp
    ${CMAKE_SOURCE_DIR}/plugins/baseline/model/readiness_helper.cpp
    ${CMAKE_SOURCE_DIR}/plugins/baseline/model/series_store.cpp
)
add_executable(test_baseline_relation_task
    test_baseline_relation_task.cpp
    ${CMAKE_SOURCE_DIR}/plugins/baseline/config/runtime_config.cpp
    ${CMAKE_SOURCE_DIR}/plugins/baseline/config_parser.cpp
    ${CMAKE_SOURCE_DIR}/plugins/baseline/fusion/key_risk_fusion.cpp
    ${CMAKE_SOURCE_DIR}/plugins/baseline/fusion/relation_pattern_fusion.cpp
    ${CMAKE_SOURCE_DIR}/plugins/baseline/model/calendar_feature_helper.cpp
    ${CMAKE_SOURCE_DIR}/plugins/baseline/model/event_calendar_matcher.cpp
    ${CMAKE_SOURCE_DIR}/plugins/baseline/model/formal_predictor.cpp
    ${CMAKE_SOURCE_DIR}/plugins/baseline/model/readiness_helper.cpp
    ${CMAKE_SOURCE_DIR}/plugins/baseline/model/series_store.cpp
    ${CMAKE_SOURCE_DIR}/plugins/baseline/rebuild/candidate_builder.cpp
    ${CMAKE_SOURCE_DIR}/plugins/baseline/rebuild/candidate_validator.cpp
    ${CMAKE_SOURCE_DIR}/plugins/baseline/rebuild/formal_model_trainer.cpp
    ${CMAKE_SOURCE_DIR}/plugins/baseline/rebuild/rebuild_queue.cpp
    ${CMAKE_SOURCE_DIR}/plugins/baseline/rebuild/rebuild_worker.cpp
    ${CMAKE_SOURCE_DIR}/plugins/baseline/rebuild/replay_runner.cpp
    ${CMAKE_SOURCE_DIR}/plugins/baseline/relation/relation_basis.cpp
    ${CMAKE_SOURCE_DIR}/plugins/baseline/relation/relation_router.cpp
    ${CMAKE_SOURCE_DIR}/plugins/baseline/relation/relation_summary_extractor.cpp
    ${CMAKE_SOURCE_DIR}/plugins/baseline/solver/solver_backend.cpp
    ${CMAKE_SOURCE_DIR}/plugins/baseline/task/baseline_task_base.cpp
    ${CMAKE_SOURCE_DIR}/plugins/baseline/task/relation_task.cpp
    ${CMAKE_SOURCE_DIR}/plugins/baseline/task/task_registry.cpp
    ${CMAKE_SOURCE_DIR}/plugins/baseline/detector/value_detector_core.cpp
    ${CMAKE_SOURCE_DIR}/plugins/baseline/detector/ratio_detector_core.cpp
)
add_executable(test_baseline_model_helpers
    test_baseline_model_helpers.cpp
    ${CMAKE_SOURCE_DIR}/plugins/baseline/config/runtime_config.cpp
    ${CMAKE_SOURCE_DIR}/plugins/baseline/config_parser.cpp
    ${CMAKE_SOURCE_DIR}/plugins/baseline/fusion/relation_pattern_fusion.cpp
    ${CMAKE_SOURCE_DIR}/plugins/baseline/model/calendar_feature_helper.cpp
    ${CMAKE_SOURCE_DIR}/plugins/baseline/model/event_calendar_matcher.cpp
    ${CMAKE_SOURCE_DIR}/plugins/baseline/model/formal_predictor.cpp
    ${CMAKE_SOURCE_DIR}/plugins/baseline/model/readiness_helper.cpp
    ${CMAKE_SOURCE_DIR}/plugins/baseline/relation/relation_basis.cpp
    ${CMAKE_SOURCE_DIR}/plugins/baseline/rebuild/candidate_builder.cpp
    ${CMAKE_SOURCE_DIR}/plugins/baseline/rebuild/candidate_validator.cpp
    ${CMAKE_SOURCE_DIR}/plugins/baseline/rebuild/formal_model_trainer.cpp
    ${CMAKE_SOURCE_DIR}/plugins/baseline/solver/solver_backend.cpp
)
add_executable(test_baseline_task_headers test_baseline_task_headers.cpp)
add_executable(test_baseline_rebuild test_baseline_rebuild.cpp)
add_executable(test_baseline_concurrency test_baseline_concurrency.cpp)
add_executable(test_baseline_value_replay_example
    test_baseline_value_replay_example.cpp
)

target_include_directories(${PROJECT_NAME} PUBLIC ${CMAKE_SOURCE_DIR})
target_include_directories(test_baseline_value_task PUBLIC ${CMAKE_SOURCE_DIR})
target_include_directories(test_baseline_ratio_task PUBLIC ${CMAKE_SOURCE_DIR})
target_include_directories(test_baseline_relation_task PUBLIC ${CMAKE_SOURCE_DIR})
target_include_directories(test_baseline_model_helpers PUBLIC ${CMAKE_SOURCE_DIR})
target_include_directories(test_baseline_task_headers PUBLIC ${CMAKE_SOURCE_DIR})
target_include_directories(test_baseline_rebuild PUBLIC ${CMAKE_SOURCE_DIR})
target_include_directories(test_baseline_concurrency PUBLIC ${CMAKE_SOURCE_DIR})
target_include_directories(test_baseline_value_replay_example PUBLIC ${CMAKE_SOURCE_DIR})
target_include_directories(test_baseline_value_task PRIVATE ${ICU_INCLUDE_DIRS})
target_include_directories(test_baseline_ratio_task PRIVATE ${ICU_INCLUDE_DIRS})
target_include_directories(test_baseline_relation_task PRIVATE ${ICU_INCLUDE_DIRS})
target_include_directories(test_baseline_model_helpers PRIVATE ${ICU_INCLUDE_DIRS})

add_dependencies(${PROJECT_NAME} flowsql_common flowsql_baseline)
add_dependencies(test_baseline_value_task flowsql_common)
add_dependencies(test_baseline_ratio_task flowsql_common)
add_dependencies(test_baseline_relation_task flowsql_common)
add_dependencies(test_baseline_model_helpers flowsql_common)
add_dependencies(test_baseline_task_headers flowsql_common)
add_dependencies(test_baseline_rebuild flowsql_common flowsql_baseline)
add_dependencies(test_baseline_concurrency flowsql_common flowsql_baseline)
add_dependencies(test_baseline_value_replay_example flowsql_common flowsql_baseline)
target_link_libraries(${PROJECT_NAME} flowsql_common flowsql_baseline)
target_link_libraries(test_baseline_value_task flowsql_common ${ICU_LIBRARIES})
target_link_libraries(test_baseline_ratio_task flowsql_common ${ICU_LIBRARIES})
target_link_libraries(test_baseline_relation_task flowsql_common ${ICU_LIBRARIES})
target_link_libraries(test_baseline_model_helpers flowsql_common ${ICU_LIBRARIES})
target_link_libraries(test_baseline_task_headers flowsql_common)
target_link_libraries(test_baseline_rebuild flowsql_common)
target_link_libraries(test_baseline_concurrency flowsql_common)
target_link_libraries(test_baseline_value_replay_example flowsql_common flowsql_baseline)

target_compile_options(${PROJECT_NAME} PRIVATE -UNDEBUG)
target_compile_options(test_baseline_value_task PRIVATE -UNDEBUG)
target_compile_options(test_baseline_ratio_task PRIVATE -UNDEBUG)
target_compile_options(test_baseline_relation_task PRIVATE -UNDEBUG)
target_compile_options(test_baseline_model_helpers PRIVATE -UNDEBUG)

add_thirddepen(test_baseline_model_helpers eigen rapidjson)
add_thirddepen(test_baseline_relation_task eigen rapidjson yaml-cpp)
add_thirddepen(test_baseline_model_helpers yaml-cpp)
add_thirddepen(test_baseline_value_task yaml-cpp)
add_thirddepen(test_baseline_ratio_task yaml-cpp)
add_thirddepen(test_baseline_value_replay_example rapidjson)
target_compile_options(test_baseline_task_headers PRIVATE -UNDEBUG)
target_compile_options(test_baseline_rebuild PRIVATE -UNDEBUG)
target_compile_options(test_baseline_concurrency PRIVATE -UNDEBUG)
target_compile_options(test_baseline_value_replay_example PRIVATE -UNDEBUG)

set_target_properties(${PROJECT_NAME} PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/output
)
set_target_properties(test_baseline_value_task PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/output
)
set_target_properties(test_baseline_ratio_task PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/output
)
set_target_properties(test_baseline_relation_task PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/output
)
set_target_properties(test_baseline_model_helpers PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/output
)
set_target_properties(test_baseline_task_headers PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/output
)
set_target_properties(test_baseline_rebuild PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/output
)
set_target_properties(test_baseline_concurrency PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/output
)
set_target_properties(test_baseline_value_replay_example PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/output
)
