set(PROJECT_NAME "hyperscan")

include(ExternalProject)

ExternalProject_Add(hyperscan
    URL "https://github.com/intel/hyperscan/archive/refs/tags/v5.4.2.tar.gz"
    CMAKE_ARGS -DCMAKE_POSITION_INDEPENDENT_CODE=ON -DCMAKE_INSTALL_PREFIX:PATH=${CMAKE_BINARY_DIR}/hyperscan
)

set(hyperscan_LINK_INC ${CMAKE_BINARY_DIR}/hyperscan/include/hs)
set(hyperscan_LINK_DIR ${CMAKE_BINARY_DIR}/hyperscan/lib)
set(hyperscan_LINK_TAR -lhs -lhs_runtime)
