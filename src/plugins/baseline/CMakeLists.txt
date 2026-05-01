project(flowsql_baseline)
find_package(ICU REQUIRED COMPONENTS uc i18n)

add_library(${PROJECT_NAME} SHARED
    baseline_plugin.cpp
    bootstrap/bootstrap_engine.cpp
    config/runtime_config.cpp
    config_parser.cpp
    model/calendar_feature_helper.cpp
    model/event_calendar_matcher.cpp
    model/formal_predictor.cpp
    model/readiness_helper.cpp
    bootstrap/formal_model_trainer.cpp
    relation/relation_basis.cpp
    solver/solver_backend.cpp
    task/baseline_task_base.cpp
    task/bootstrap_task_store.cpp
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
