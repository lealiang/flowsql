# --- flowsql 可执行文件（通用入口） ---
add_executable(flowsql main.cpp ${CMAKE_SOURCE_DIR}/services/gateway/service_client.cpp)
target_include_directories(flowsql PUBLIC ${CMAKE_SOURCE_DIR})
add_thirddepen(flowsql httplib rapidjson)
# 主程序不再链接 flowsql_common（PluginLoader 是 header-only）
set_target_properties(flowsql PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/output
)

# 构建后将 web/static 复制到 output/static（前端静态文件）
add_custom_command(TARGET flowsql POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_directory
        ${CMAKE_SOURCE_DIR}/services/web/static
        ${CMAKE_BINARY_DIR}/output/static
    COMMENT "Copying web static files to output/static"
)
