project(flowsql_web_all)

# --- libflowsql_web.so（Web 插件） ---
file(GLOB WEB_PLUGIN_SRCS
    web_server.cpp
    web_plugin.cpp
    web_plugin_register.cpp
    db/database.cpp
)

add_library(flowsql_web SHARED ${WEB_PLUGIN_SRCS})
target_include_directories(flowsql_web PUBLIC ${CMAKE_SOURCE_DIR})
add_thirddepen(flowsql_web httplib rapidjson sqlite)
# Web 不需要 flowsql_common（不使用 DataFrame 等公共基础设施）
set_target_properties(flowsql_web PROPERTIES
    LIBRARY_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/output
)
