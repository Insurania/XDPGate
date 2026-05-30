set(XDPG_ODE_SOURCE_DIR "${PROJECT_SOURCE_DIR}/third_party/ode/src")
set(XDPG_ODE_BUILD_DIR "${PROJECT_SOURCE_DIR}/third_party/ode/build")

if(EXISTS "${XDPG_ODE_SOURCE_DIR}/drawstuff/src/drawstuff.cpp"
   AND EXISTS "${XDPG_ODE_BUILD_DIR}/ode/src/config.h")
    find_package(OpenGL QUIET)

    if(OpenGL_FOUND)
        add_library(xdpg_drawstuff STATIC
            ${XDPG_ODE_SOURCE_DIR}/drawstuff/src/drawstuff.cpp
        )

        if(WIN32)
            target_sources(xdpg_drawstuff PRIVATE
                ${XDPG_ODE_SOURCE_DIR}/drawstuff/src/windows.cpp
                ${XDPG_ODE_SOURCE_DIR}/drawstuff/src/resources.rc
            )
        elseif(APPLE)
            target_sources(xdpg_drawstuff PRIVATE
                ${XDPG_ODE_SOURCE_DIR}/drawstuff/src/osx.cpp
            )
        else()
            target_sources(xdpg_drawstuff PRIVATE
                ${XDPG_ODE_SOURCE_DIR}/drawstuff/src/x11.cpp
            )
        endif()

        target_include_directories(xdpg_drawstuff
            PUBLIC
                ${XDPG_ODE_SOURCE_DIR}/include
            PRIVATE
                ${XDPG_ODE_SOURCE_DIR}/drawstuff/src
                ${XDPG_ODE_BUILD_DIR}/ode/src
                ${XDPG_ODE_BUILD_DIR}/include
                ${XDPG_ODE_SOURCE_DIR}/ode/src
        )

        target_compile_definitions(xdpg_drawstuff
            PUBLIC
                DS_LIB
                DRAWSTUFF_TEXTURE_PATH="${XDPG_ODE_SOURCE_DIR}/drawstuff/textures"
        )

        target_link_libraries(xdpg_drawstuff PUBLIC ODE::ODE OpenGL::GL)

        if(TARGET OpenGL::GLU)
            target_link_libraries(xdpg_drawstuff PUBLIC OpenGL::GLU)
        elseif(WIN32)
            target_link_libraries(xdpg_drawstuff PUBLIC glu32)
        endif()

        if(WIN32)
            target_link_libraries(xdpg_drawstuff PRIVATE winmm)
        endif()

        if(MSVC)
            target_compile_options(xdpg_drawstuff PRIVATE /W3 /utf-8)
        else()
            target_compile_options(xdpg_drawstuff PRIVATE -Wall -Wextra)
        endif()

        add_subdirectory(ode_world)
    else()
        message(STATUS "OpenGL not found: local ODE viewer skipped")
    endif()
else()
    message(STATUS "ODE source/build tree not found: local ODE viewer skipped")
endif()
