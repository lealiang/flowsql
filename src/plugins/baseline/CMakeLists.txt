project(flowsql_baseline)
find_package(ICU REQUIRED COMPONENTS uc i18n)

add_library(${PROJECT_NAME} SHARED
    baseline_plugin.cpp
    config/runtime_config.cpp
    config_parser.cpp
    detector/ratio_detector_core.cpp
    detector/value_detector_core.cpp
    fusion/key_risk_fusion.cpp
    fusion/relation_pattern_fusion.cpp
    model/calendar_feature_helper.cpp
    model/event_calendar_matcher.cpp
    model/formal_predictor.cpp
    model/readiness_helper.cpp
    model/series_store.cpp
    relation/relation_basis.cpp
    relation/relation_router.cpp
    relation/relation_summary_extractor.cpp
    rebuild/candidate_builder.cpp
    rebuild/candidate_validator.cpp
    rebuild/formal_model_trainer.cpp
    rebuild/rebuild_queue.cpp
    rebuild/replay_runner.cpp
    rebuild/rebuild_worker.cpp
    solver/solver_backend.cpp
    task/baseline_task_base.cpp
    task/relation_task.cpp
    task/ratio_task.cpp
    task/task_registry.cpp
    task/value_task.cpp
)

target_include_directories(${PROJECT_NAME} PUBLIC
    ${CMAKE_SOURCE_DIR}
    ${ICU_INCLUDE_DIRS}
)

add_thirddepen(${PROJECT_NAME} eigen rapidjson yaml-cpp)
target_link_libraries(${PROJECT_NAME} ${ICU_LIBRARIES})

set_target_properties(${PROJECT_NAME} PROPERTIES
    LIBRARY_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/output
)
