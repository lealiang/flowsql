
include(ExternalProject)

ExternalProject_Add(rapidjson
        URL "https://github.com/Tencent/rapidjson/archive/refs/tags/v1.1.0.tar.gz"
        CMAKE_ARGS -DCMAKE_INSTALL_PREFIX:PATH=${CMAKE_BINARY_DIR}/rapidjson -DRAPIDJSON_BUILD_THIRDPARTY_GTEST=OFF -DRAPIDJSON_BUILD_EXAMPLES=OFF -DCMAKE_CXX_FLAGS="-Wno-class-memaccess"
        )
set(rapidjson_LINK_INC ${CMAKE_BINARY_DIR}/rapidjson/include)
set(rapidjson_LINK_DIR ${CMAKE_BINARY_DIR}/rapidjson/lib)
set(rapidjson_LINK_TAR )