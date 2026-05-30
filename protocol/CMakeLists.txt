add_library(xdpg_protocol
    xdg_protocol.cpp
)

target_include_directories(xdpg_protocol
    PUBLIC
        ${PROJECT_SOURCE_DIR}
)

target_compile_features(xdpg_protocol PUBLIC cxx_std_17)

if(MSVC)
    target_compile_options(xdpg_protocol PRIVATE /W4 /utf-8)
else()
    target_compile_options(xdpg_protocol PRIVATE -Wall -Wextra -Wpedantic)
endif()

add_executable(xdg_protocol_test
    xdg_protocol_test.cpp
)

target_link_libraries(xdg_protocol_test PRIVATE xdpg_protocol)

if(MSVC)
    target_compile_options(xdg_protocol_test PRIVATE /W4 /utf-8)
else()
    target_compile_options(xdg_protocol_test PRIVATE -Wall -Wextra -Wpedantic)
endif()

add_test(NAME xdg_protocol_test COMMAND xdg_protocol_test)

