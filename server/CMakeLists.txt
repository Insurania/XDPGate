add_executable(xdpg_server
    src/main.cpp
    src/app/ds_server.cpp
    src/net/udp_socket.cpp
)

target_include_directories(xdpg_server
    PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/include
)

target_link_libraries(xdpg_server
    PRIVATE
        xdpg_protocol
        xdpg_physics
)

target_compile_features(xdpg_server PRIVATE cxx_std_17)

if(WIN32)
    target_link_libraries(xdpg_server PRIVATE ws2_32)
endif()

if(MSVC)
    target_compile_options(xdpg_server PRIVATE /W4 /utf-8)
else()
    target_compile_options(xdpg_server PRIVATE -Wall -Wextra -Wpedantic)
endif()

