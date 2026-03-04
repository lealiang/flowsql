project(flowsql_database)

file(GLOB DIR_SRCS *.cpp)
file(GLOB DRIVER_SRCS drivers/*.cpp)

add_library(${PROJECT_NAME} SHARED ${DIR_SRCS} ${DRIVER_SRCS})

target_include_directories(${PROJECT_NAME} PUBLIC ${CMAKE_SOURCE_DIR})

add_thirddepen(${PROJECT_NAME} arrow rapidjson sqlite)

add_dependencies(${PROJECT_NAME} flowsql_common)
target_link_libraries(${PROJECT_NAME} flowsql_common)

set_target_properties(${PROJECT_NAME} PROPERTIES
    LIBRARY_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/output
)
