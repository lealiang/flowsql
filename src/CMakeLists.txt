cmake_minimum_required(VERSION 3.12)

project(npi_test)

# 设置编译参数
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -g -fno-strict-aliasing -Wno-invalid-offsetof -std=c++17 -Wno-deprecated -Wno-unused-value")
set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -Wno-unused-variable -Wno-unused-but-set-variable -Wno-array-bounds")
set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -ldl -lm")
set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -fvisibility=hidden -Wno-parentheses -Wno-sign-compare -Wno-unknown-pragmas -Wno-format")

# TODO: 代码开发阶段采用调试模式
SET(CMAKE_CXX_FLAGS_DEBUG "${CMAKE_CXX_FLAGS} -fPIC -O0 -Wall -g -ggdb ")
SET(CMAKE_CXX_FLAGS_RELEASE "${CMAKE_CXX_FLAGS} -fPIC -O3 -Wall -g ")

SET(CMAKE_C_FLAGS_DEBUG "${CMAKE_C_FLAGS} -fPIC -O0 -Wall -g -ggdb")
SET(CMAKE_C_FLAGS_RELEASE "${CMAKE_C_FLAGS} -fPIC -O3 -Wall -g")

IF(DEBUG_mode)
    add_definitions(-DDEBUG -D_DEBUG)
else()
    add_definitions(-DNDEBUG)
endif()

if(CMAKE_CXX_COMPILER_VERSION VERSION_GREATER 4.8.1)
    set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -Wno-literal-suffix -Wno-unused-local-typedefs")
endif()

# 设置库输出目录
set(LIBRARY_OUTPUT_PATH ${CMAKE_BINARY_DIR}/output)
set(EXECUTABLE_OUTPUT_PATH  ${CMAKE_BINARY_DIR}/output)

# 设置 thirdparts 路径
set(THIRDPARTS_DIR ${CMAKE_SOURCE_DIR}/thirdparts)

# 子项目
include(${CMAKE_SOURCE_DIR}/subjects.cmake)
add_thirdparts()

# 添加外部子项目
add_subdirectory(${CMAKE_SOURCE_DIR}/plugins/protocol/npi ${CMAKE_BINARY_DIR}/npi_plugin)
add_subdirectory(${CMAKE_SOURCE_DIR}/tests/test_npi ${CMAKE_BINARY_DIR}/test_npi)
