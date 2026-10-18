cmake_minimum_required(VERSION 3.16)

project(telemetry_lab)

add_library(telemetry_core STATIC
    src/statistics.cpp
)

target_include_directories(telemetry_core PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/include
)

add_executable(telemetry_analyze
    src/main.cpp
)

target_link_libraries(telemetry_analyze PRIVATE
    telemetry_core
)