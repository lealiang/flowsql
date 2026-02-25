
include(ExternalProject)

set(PROJECT_NAME gflags)

ExternalProject_Add(${PROJECT_NAME}
    PREFIX ${CMAKE_BINARY_DIR}/${PROJECT_NAME}-prefix
    URL "https://github.com/gflags/gflags/archive/v2.3.0.tar.gz"
    CMAKE_ARGS -DCMAKE_INSTALL_PREFIX:PATH=${CMAKE_BINARY_DIR}/${PROJECT_NAME}
    )
set(gflags_LINK_INC ${CMAKE_BINARY_DIR}/${PROJECT_NAME}/include)
set(gflags_LINK_DIR ${CMAKE_BINARY_DIR}/${PROJECT_NAME}/lib)
set(gflags_LINK_TAR -lgflags)