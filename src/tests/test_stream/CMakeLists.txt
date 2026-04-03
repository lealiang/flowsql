project(test_stream)

add_executable(${PROJECT_NAME}
    test_stream.cpp
    ${CMAKE_SOURCE_DIR}/services/scheduler/stream_task.cpp
    ${CMAKE_SOURCE_DIR}/services/scheduler/stream_runtime.cpp
)

target_include_directories(${PROJECT_NAME} PUBLIC ${CMAKE_SOURCE_DIR})

add_thirddepen(${PROJECT_NAME} arrow rapidjson)

add_dependencies(${PROJECT_NAME} flowsql_common)
target_link_libraries(${PROJECT_NAME} flowsql_common)

target_compile_options(${PROJECT_NAME} PRIVATE -UNDEBUG)

set_target_properties(${PROJECT_NAME} PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/output
)

add_test(NAME test_stream COMMAND ${CMAKE_BINARY_DIR}/output/test_stream)

add_executable(benchmark_stream_ring
    benchmark_ring_modes.cpp
)

target_include_directories(benchmark_stream_ring PUBLIC ${CMAKE_SOURCE_DIR})
add_thirddepen(benchmark_stream_ring arrow rapidjson)
add_dependencies(benchmark_stream_ring flowsql_common)
target_link_libraries(benchmark_stream_ring flowsql_common)

set_target_properties(benchmark_stream_ring PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/output
)
