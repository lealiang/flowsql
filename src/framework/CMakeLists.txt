project(flowsql_framework)

file(GLOB_RECURSE DIR_SRCS core/*.cpp)

add_library(${PROJECT_NAME} SHARED ${DIR_SRCS})

target_include_directories(${PROJECT_NAME} PUBLIC ${CMAKE_SOURCE_DIR})

add_thirddepen(${PROJECT_NAME} arrow rapidjson)

set_target_properties(${PROJECT_NAME} PROPERTIES
    LIBRARY_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/output
    ARCHIVE_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/output
    POSITION_INDEPENDENT_CODE ON
    CXX_VISIBILITY_PRESET default
)

# 覆盖全局 -fvisibility=hidden，共享库需要导出符号
target_compile_options(${PROJECT_NAME} PRIVATE -fvisibility=default)
