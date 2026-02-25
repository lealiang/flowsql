include(ExternalProject)

set(PROJECT_NAME glog)

ExternalProject_Add(${PROJECT_NAME}
        PREFIX ${CMAKE_BINARY_DIR}/${PROJECT_NAME}-prefix
        URL "https://github.com/google/glog/archive/v0.4.0.tar.gz"
        CMAKE_ARGS -DWITH_GFLAGS=0 -DCMAKE_INSTALL_PREFIX:PATH=${CMAKE_BINARY_DIR}/${PROJECT_NAME}
        #DEPENDS gflags
        )
set(glog_LINK_INC ${CMAKE_BINARY_DIR}/${PROJECT_NAME}/include)
set(glog_LINK_DIR ${CMAKE_BINARY_DIR}/${PROJECT_NAME}/lib64 ${CMAKE_BINARY_DIR}/${PROJECT_NAME}/lib)
set(glog_LINK_TAR -lglog)