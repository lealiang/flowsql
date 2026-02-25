project(test_npi)

cmake_minimum_required(VERSION 3.12)

file(GLOB_RECURSE DIR_SRCS *.cc *.cpp)

# 设置 thirdparts 路径
set(THIRDPARTS_DIR ${CMAKE_SOURCE_DIR}/../../thirdparts)

# 头文件目录
include_directories(${CMAKE_SOURCE_DIR})
include_directories(${CMAKE_SOURCE_DIR}/../common)
include_directories(${CMAKE_SOURCE_DIR}/../plugins)
include_directories(${CMAKE_SOURCE_DIR}/../plugins/protocol)
include_directories(${THIRDPARTS_DIR})

# 库文件目录
link_directories(${CMAKE_SOURCE_DIR}/../../lib)

# 生成可执行文件
add_executable(${PROJECT_NAME} ${DIR_SRCS})

# 依赖库
add_thirddepen(${PROJECT_NAME} gflags glog yaml-cpp rapidjson)

# 链接 nPI 插件库
find_library(NPI_LIBRARY NAMES flowsql_npi PATHS ${CMAKE_SOURCE_DIR}/../../lib)
if(NPI_LIBRARY)
    target_link_libraries(${PROJECT_NAME} ${NPI_LIBRARY})
endif()
