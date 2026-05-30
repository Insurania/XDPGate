add_executable(xdpg_toy_client
    src/toy_client.cpp
)

target_link_libraries(xdpg_toy_client
    PRIVATE
        xdpg_protocol
        xdpg_net
)

target_compile_features(xdpg_toy_client PRIVATE cxx_std_17)

if(MSVC)
    target_compile_options(xdpg_toy_client PRIVATE /W4 /utf-8)
else()
    target_compile_options(xdpg_toy_client PRIVATE -Wall -Wextra -Wpedantic)
endif()
