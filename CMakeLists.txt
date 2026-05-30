cmake_minimum_required(VERSION 3.16)

project(XDPGate LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

add_library(xdpg_protocol
    protocol/xdg_protocol.cpp
)

target_include_directories(xdpg_protocol
    PUBLIC
        ${CMAKE_CURRENT_SOURCE_DIR}
)

target_compile_features(xdpg_protocol PUBLIC cxx_std_17)

if(MSVC)
    target_compile_options(xdpg_protocol PRIVATE /W4 /utf-8)
else()
    target_compile_options(xdpg_protocol PRIVATE -Wall -Wextra -Wpedantic)
endif()

add_executable(xdg_protocol_test
    protocol/xdg_protocol_test.cpp
)

target_link_libraries(xdg_protocol_test PRIVATE xdpg_protocol)

if(MSVC)
    target_compile_options(xdg_protocol_test PRIVATE /W4 /utf-8)
else()
    target_compile_options(xdg_protocol_test PRIVATE -Wall -Wextra -Wpedantic)
endif()

enable_testing()
add_test(NAME xdg_protocol_test COMMAND xdg_protocol_test)
