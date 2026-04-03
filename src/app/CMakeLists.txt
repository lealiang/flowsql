# --- flowsql 可执行文件（通用入口） ---
# Guardian 模式需要解析 gateway.yaml，因此链接 gateway 的 config.cpp 和 yaml-cpp
add_executable(flowsql
    main.cpp
    ${CMAKE_SOURCE_DIR}/services/gateway/config.cpp
)
target_include_directories(flowsql PUBLIC ${CMAKE_SOURCE_DIR})
add_thirddepen(flowsql httplib yaml-cpp)

set_target_properties(flowsql PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/output
)

# 构建后同步运行时配置到 output/config，避免使用旧副本导致行为与仓库配置不一致。
add_custom_command(TARGET flowsql POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E make_directory
        ${CMAKE_BINARY_DIR}/output/config
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
        ${CMAKE_SOURCE_DIR}/../config/flowsql.yml
        ${CMAKE_BINARY_DIR}/output/config/flowsql.yml
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
        ${CMAKE_SOURCE_DIR}/../config/deploy-single.yaml
        ${CMAKE_BINARY_DIR}/output/config/deploy-single.yaml
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
        ${CMAKE_SOURCE_DIR}/../config/deploy-multi.yaml
        ${CMAKE_BINARY_DIR}/output/config/deploy-multi.yaml
    COMMENT "Syncing runtime config files to output/config"
)

# 构建后将 web/static 复制到 output/static（前端静态文件）
# flowsql_web 目标已负责同步静态资源；这里避免并发 copy_directory 造成竞态失败。
if(NOT TARGET flowsql_web)
    add_custom_command(TARGET flowsql POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_directory
            ${CMAKE_SOURCE_DIR}/services/web/static
            ${CMAKE_BINARY_DIR}/output/static
        COMMENT "Copying web static files to output/static"
    )
endif()
