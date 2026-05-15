cmake_minimum_required(VERSION 3.10)
project(rtsp-relay LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

set(CMAKE_CXX_FLAGS_RELEASE "-O2 -DNDEBUG")
set(CMAKE_CXX_FLAGS_DEBUG "-g -O0 -DDEBUG")

set(SOURCES
    src/main.cpp
    src/event_loop.cpp
    src/tcp_connection.cpp
    src/tcp_server.cpp
    src/tcp_client.cpp
    src/rtsp_protocol.cpp
    src/rtsp_session.cpp
    src/rtsp_source.cpp
    src/relay_manager.cpp
    src/http_server.cpp
    src/config.cpp
)

add_executable(rtsp-relay ${SOURCES})

if(APPLE)
    target_link_libraries(rtsp-relay PRIVATE pthread)
elseif(UNIX)
    target_link_libraries(rtsp-relay PRIVATE pthread)
endif()

target_include_directories(rtsp-relay PRIVATE ${CMAKE_SOURCE_DIR}/src)
