add_executable(xdpg_ode_world_viewer
    ode_world_viewer.cpp
)

target_link_libraries(xdpg_ode_world_viewer
    PRIVATE
        xdpg_physics
        xdpg_drawstuff
)

if(MSVC)
    target_compile_options(xdpg_ode_world_viewer PRIVATE /W4 /utf-8)
else()
    target_compile_options(xdpg_ode_world_viewer PRIVATE -Wall -Wextra -Wpedantic)
endif()

