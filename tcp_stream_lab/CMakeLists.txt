cmake_minimum_required(VERSION 3.16)

project(tcp_stream_lab LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

find_package(Threads REQUIRED)

if(WIN32)
    add_executable(tcp_server_win apps/tcp_server_win.cpp)
    target_link_libraries(tcp_server_win PRIVATE ws2_32 Threads::Threads)
    target_compile_options(tcp_server_win PRIVATE /W4 /permissive-)

    add_executable(tcp_server_gui_win WIN32 apps/tcp_server_gui_win.cpp)
    target_link_libraries(tcp_server_gui_win PRIVATE ws2_32 Threads::Threads)
    target_compile_definitions(tcp_server_gui_win PRIVATE UNICODE _UNICODE)
    target_compile_options(tcp_server_gui_win PRIVATE /W4 /permissive- /utf-8)
elseif(CMAKE_SYSTEM_NAME STREQUAL "Linux")
    add_executable(tcp_client_linux apps/tcp_client_linux.cpp)
    target_link_libraries(tcp_client_linux PRIVATE Threads::Threads)
    target_compile_options(tcp_client_linux PRIVATE -Wall -Wextra -Wpedantic)
else()
    message(FATAL_ERROR "This learning project supports Windows Server and Linux Client builds only.")
endif()
