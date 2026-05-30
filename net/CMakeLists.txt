add_library(xdpg_net
    src/udp_socket.cpp
)

target_include_directories(xdpg_net
    PUBLIC
        ${CMAKE_CURRENT_SOURCE_DIR}/include
)

target_compile_features(xdpg_net PUBLIC cxx_std_17)

if(WIN32)
    target_link_libraries(xdpg_net PUBLIC ws2_32)
endif()

if(MSVC)
    target_compile_options(xdpg_net PRIVATE /W4 /utf-8)
else()
    target_compile_options(xdpg_net PRIVATE -Wall -Wextra -Wpedantic)
endif()

