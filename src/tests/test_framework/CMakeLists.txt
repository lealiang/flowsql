project(test_framework)

file(GLOB_RECURSE DIR_SRCS *.cpp)

add_executable(${PROJECT_NAME} ${DIR_SRCS})

target_include_directories(${PROJECT_NAME} PUBLIC ${CMAKE_SOURCE_DIR})

add_thirddepen(${PROJECT_NAME} arrow rapidjson)

add_dependencies(${PROJECT_NAME} flowsql_framework flowsql_example)
target_link_libraries(${PROJECT_NAME} flowsql_framework)

set_target_properties(${PROJECT_NAME} PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/output
)
