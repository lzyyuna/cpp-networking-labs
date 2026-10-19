cmake_minimum_required(VERSION 3.16)

project(telemetry_lab LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

find_package(Threads REQUIRED)

add_library(telemetry_core STATIC
    src/statistics.cpp
    src/bounded_queue.cpp
    src/pipeline.cpp
)

target_include_directories(telemetry_core PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/include
)

add_executable(telemetry_analyze
    src/main.cpp
)

target_link_libraries(telemetry_analyze PRIVATE
    telemetry_core
    Threads::Threads
)
