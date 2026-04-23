project(test_baseline)

add_executable(${PROJECT_NAME} test_baseline_plugin.cpp)
add_executable(test_baseline_value_task
    test_baseline_value_task.cpp
    ${CMAKE_SOURCE_DIR}/plugins/baseline/detector/value_detector_core.cpp
    ${CMAKE_SOURCE_DIR}/plugins/baseline/model/formal_predictor.cpp
    ${CMAKE_SOURCE_DIR}/plugins/baseline/model/series_store.cpp
)
add_executable(test_baseline_ratio_task
    test_baseline_ratio_task.cpp
    ${CMAKE_SOURCE_DIR}/plugins/baseline/detector/ratio_detector_core.cpp
    ${CMAKE_SOURCE_DIR}/plugins/baseline/model/formal_predictor.cpp
    ${CMAKE_SOURCE_DIR}/plugins/baseline/model/series_store.cpp
)
add_executable(test_baseline_relation_task
    test_baseline_relation_task.cpp
    ${CMAKE_SOURCE_DIR}/plugins/baseline/config_parser.cpp
    ${CMAKE_SOURCE_DIR}/plugins/baseline/relation/relation_basis.cpp
    ${CMAKE_SOURCE_DIR}/plugins/baseline/relation/relation_router.cpp
    ${CMAKE_SOURCE_DIR}/plugins/baseline/relation/relation_summary_extractor.cpp
)
add_executable(test_baseline_rebuild test_baseline_rebuild.cpp)
add_executable(test_baseline_concurrency test_baseline_concurrency.cpp)

target_include_directories(${PROJECT_NAME} PUBLIC ${CMAKE_SOURCE_DIR})
target_include_directories(test_baseline_value_task PUBLIC ${CMAKE_SOURCE_DIR})
target_include_directories(test_baseline_ratio_task PUBLIC ${CMAKE_SOURCE_DIR})
target_include_directories(test_baseline_relation_task PUBLIC ${CMAKE_SOURCE_DIR})
target_include_directories(test_baseline_rebuild PUBLIC ${CMAKE_SOURCE_DIR})
target_include_directories(test_baseline_concurrency PUBLIC ${CMAKE_SOURCE_DIR})

add_dependencies(${PROJECT_NAME} flowsql_common flowsql_baseline)
add_dependencies(test_baseline_value_task flowsql_common)
add_dependencies(test_baseline_ratio_task flowsql_common)
add_dependencies(test_baseline_relation_task flowsql_common)
add_dependencies(test_baseline_rebuild flowsql_common flowsql_baseline)
add_dependencies(test_baseline_concurrency flowsql_common flowsql_baseline)
target_link_libraries(${PROJECT_NAME} flowsql_common)
target_link_libraries(test_baseline_value_task flowsql_common)
target_link_libraries(test_baseline_ratio_task flowsql_common)
target_link_libraries(test_baseline_relation_task flowsql_common)
target_link_libraries(test_baseline_rebuild flowsql_common)
target_link_libraries(test_baseline_concurrency flowsql_common)

target_compile_options(${PROJECT_NAME} PRIVATE -UNDEBUG)
target_compile_options(test_baseline_value_task PRIVATE -UNDEBUG)
target_compile_options(test_baseline_ratio_task PRIVATE -UNDEBUG)
target_compile_options(test_baseline_relation_task PRIVATE -UNDEBUG)
target_compile_options(test_baseline_rebuild PRIVATE -UNDEBUG)
target_compile_options(test_baseline_concurrency PRIVATE -UNDEBUG)

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
set_target_properties(test_baseline_rebuild PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/output
)
set_target_properties(test_baseline_concurrency PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/output
)
