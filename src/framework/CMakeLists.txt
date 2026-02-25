project(flowsql_framework)

file(GLOB_RECURSE DIR_SRCS core/*.cpp)

add_library(${PROJECT_NAME} STATIC ${DIR_SRCS})

target_include_directories(${PROJECT_NAME} PUBLIC ${CMAKE_SOURCE_DIR})

add_thirddepen(${PROJECT_NAME} arrow rapidjson)

set_target_properties(${PROJECT_NAME} PROPERTIES
    ARCHIVE_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/output
)
