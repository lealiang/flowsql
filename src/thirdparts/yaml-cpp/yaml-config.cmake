include(ExternalProject)
ExternalProject_Add(yaml-cpp
    URL "https://github.com/jbeder/yaml-cpp/archive/yaml-cpp-0.9.0.tar.gz"
    CMAKE_ARGS -DYAML_BUILD_SHARED_LIBS=1 -DCMAKE_INSTALL_PREFIX:PATH=${CMAKE_BINARY_DIR}/yaml-cpp
    )
set(yaml-cpp_LINK_INC ${CMAKE_BINARY_DIR}/yaml-cpp/include)
set(yaml-cpp_LINK_DIR ${CMAKE_BINARY_DIR}/yaml-cpp/lib)
set(yaml-cpp_LINK_TAR -lyaml-cpp)