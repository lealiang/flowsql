project(test_scheduler_e2e)

add_executable(${PROJECT_NAME} test_scheduler_e2e.cpp)

target_include_directories(${PROJECT_NAME} PUBLIC ${CMAKE_SOURCE_DIR})

add_thirddepen(${PROJECT_NAME} arrow rapidjson)

add_dependencies(${PROJECT_NAME}
    flowsql_common
    flowsql_catalog
    flowsql_scheduler
    flowsql_database
    flowsql_stream
    flowsql_web
)
target_link_libraries(${PROJECT_NAME} flowsql_common)

target_compile_options(${PROJECT_NAME} PRIVATE -UNDEBUG)

set_target_properties(${PROJECT_NAME} PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/output
)

add_test(NAME test_scheduler_e2e COMMAND ${CMAKE_BINARY_DIR}/output/test_scheduler_e2e)
