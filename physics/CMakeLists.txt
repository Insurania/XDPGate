add_library(xdpg_physics
    src/ode_world.cpp
)

target_include_directories(xdpg_physics
    PUBLIC
        ${CMAKE_CURRENT_SOURCE_DIR}/include
)

target_compile_features(xdpg_physics PUBLIC cxx_std_17)
target_link_libraries(xdpg_physics PUBLIC ODE::ODE)

if(MSVC)
    target_compile_options(xdpg_physics PRIVATE /W4 /utf-8)
else()
    target_compile_options(xdpg_physics PRIVATE -Wall -Wextra -Wpedantic)
endif()

add_executable(xdpg_physics_test
    tests/ode_world_test.cpp
)

target_link_libraries(xdpg_physics_test PRIVATE xdpg_physics)

if(MSVC)
    target_compile_options(xdpg_physics_test PRIVATE /W4 /utf-8)
else()
    target_compile_options(xdpg_physics_test PRIVATE -Wall -Wextra -Wpedantic)
endif()

add_test(NAME xdpg_physics_test COMMAND xdpg_physics_test)

