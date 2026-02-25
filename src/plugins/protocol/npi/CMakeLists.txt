project(flowsql_npi)

cmake_minimum_required(VERSION 3.12)

file(GLOB_RECURSE DIR_SRCS *.cc *.cpp)

# 设置 thirdparts 路径 (相对于 src 目录)
set(THIRDPARTS_DIR ${CMAKE_SOURCE_DIR}/../../thirdparts)

# 头文件目录
include_directories(${CMAKE_SOURCE_DIR})
include_directories(${CMAKE_SOURCE_DIR}/../../common)

# 生成动态库
add_library(${PROJECT_NAME} SHARED ${DIR_SRCS})

# 依赖库
add_thirddepen(${PROJECT_NAME} yaml-cpp rapidjson hyperscan)

# 设置输出目录
set_target_properties(${PROJECT_NAME} PROPERTIES
    LIBRARY_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/output
)
