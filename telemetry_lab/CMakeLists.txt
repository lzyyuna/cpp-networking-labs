cmake_minimum_required(VERSION 3.16)

project(telemetry_lab LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

find_package(Threads REQUIRED)

# Core library
add_library(telemetry_core STATIC
    src/bounded_queue.cpp
    src/statistics.cpp
    src/file_pipeline.cpp
)

target_include_directories(telemetry_core PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/include
)

target_link_libraries(telemetry_core PUBLIC
    Threads::Threads
)

# File analyzer
add_executable(telemetry_analyze
    apps/file_analyzer.cpp
)

target_link_libraries(telemetry_analyze PRIVATE
    telemetry_core
)

# Windows UDP applications
if(WIN32)
    add_executable(telemetry_udp_sender
        apps/udp_sender.cpp
    )

    target_link_libraries(telemetry_udp_sender PRIVATE
        Ws2_32
    )

    add_executable(telemetry_udp_receiver
        apps/udp_receiver.cpp
    )

    target_link_libraries(telemetry_udp_receiver PRIVATE
        telemetry_core
        Ws2_32
    )
endif()