project(test_baseline)

add_executable(${PROJECT_NAME} test_baseline_plugin.cpp)
add_executable(test_baseline_task_headers test_baseline_task_headers.cpp)
add_executable(test_baseline_link_bootstrap_eval test_baseline_link_bootstrap_eval.cpp)

target_include_directories(${PROJECT_NAME} PUBLIC ${CMAKE_SOURCE_DIR})
target_include_directories(test_baseline_task_headers PUBLIC ${CMAKE_SOURCE_DIR})
target_include_directories(test_baseline_link_bootstrap_eval PUBLIC ${CMAKE_SOURCE_DIR})

add_dependencies(${PROJECT_NAME} flowsql_common flowsql_baseline)
add_dependencies(test_baseline_task_headers flowsql_common flowsql_baseline)
add_dependencies(test_baseline_link_bootstrap_eval flowsql_common flowsql_baseline)

target_link_libraries(${PROJECT_NAME} flowsql_common flowsql_baseline)
target_link_libraries(test_baseline_task_headers flowsql_common flowsql_baseline)
target_link_libraries(test_baseline_link_bootstrap_eval flowsql_common flowsql_baseline)

target_compile_options(${PROJECT_NAME} PRIVATE -UNDEBUG)
target_compile_options(test_baseline_task_headers PRIVATE -UNDEBUG)
target_compile_options(test_baseline_link_bootstrap_eval PRIVATE -UNDEBUG)

add_thirddepen(${PROJECT_NAME} rapidjson)
add_thirddepen(test_baseline_task_headers rapidjson)

set_target_properties(${PROJECT_NAME} PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/output
)
set_target_properties(test_baseline_task_headers PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/output
)
set_target_properties(test_baseline_link_bootstrap_eval PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/output
)
