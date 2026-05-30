cmake_minimum_required(VERSION 3.16)

project(XDPGate LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

list(PREPEND CMAKE_PREFIX_PATH "${CMAKE_CURRENT_SOURCE_DIR}/third_party/ode/install")

enable_testing()

add_subdirectory(protocol)

find_package(ode CONFIG QUIET)

if(NOT TARGET ODE::ODE)
    find_package(PkgConfig QUIET)
    if(PkgConfig_FOUND)
        pkg_check_modules(ODE_PKG QUIET IMPORTED_TARGET ode)
        if(TARGET PkgConfig::ODE_PKG)
            add_library(ODE::ODE INTERFACE IMPORTED)
            target_link_libraries(ODE::ODE INTERFACE PkgConfig::ODE_PKG)
        endif()
    endif()
endif()

if(TARGET ODE::ODE)
    message(STATUS "ODE found: physics targets enabled")
    add_subdirectory(physics)
else()
    message(STATUS "ODE not found: physics targets skipped")
endif()
