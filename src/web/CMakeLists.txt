project(flowsql_web)

file(GLOB_RECURSE DIR_SRCS *.cpp)

add_executable(${PROJECT_NAME} ${DIR_SRCS})

target_include_directories(${PROJECT_NAME} PUBLIC ${CMAKE_SOURCE_DIR})

add_thirddepen(${PROJECT_NAME} arrow httplib rapidjson sqlite)

add_dependencies(${PROJECT_NAME} flowsql_framework)
target_link_libraries(${PROJECT_NAME} flowsql_framework)

set_target_properties(${PROJECT_NAME} PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/output
)

# 构建后将 web/static 复制到 output/static（前端静态文件）
add_custom_command(TARGET ${PROJECT_NAME} POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_directory
        ${CMAKE_SOURCE_DIR}/web/static
        ${CMAKE_BINARY_DIR}/output/static
    COMMENT "Copying web static files to output/static"
)
