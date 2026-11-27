cmake_minimum_required(VERSION 3.20)
project(open-tick VERSION 0.3.1 LANGUAGES C)

# open-tick builds on its own (tests, examples, CI) and as a subproject of an
# application, through add_subdirectory() or FetchContent. As a subproject it
# builds only its libraries:
#
#   add_subdirectory(path/to/open-tick)        # or FetchContent_MakeAvailable
#   target_link_libraries(my_sim PRIVATE opentick::core opentick::host)
if(CMAKE_SOURCE_DIR STREQUAL CMAKE_CURRENT_SOURCE_DIR)
    set(OTK_TOP_LEVEL ON)
else()
    set(OTK_TOP_LEVEL OFF)
endif()

option(OTK_WERROR "Treat warnings as errors in open-tick's own code" OFF)
option(OTK_BUILD_TESTS "Build open-tick's tests" ${OTK_TOP_LEVEL})
option(OTK_BUILD_EXAMPLES "Build open-tick's examples" ${OTK_TOP_LEVEL})

if(OTK_TOP_LEVEL)
    set(CMAKE_EXPORT_COMPILE_COMMANDS ON)
endif()

# Plain ISO C11 without GNU extensions, as in open-crank: among other things
# this keeps GCC from fusing a*b+c into FMA instructions, so results differ
# between machines only for real reasons. Set per target, so that a parent
# project's settings are left alone.
set(OTK_WARNINGS -Wall -Wextra -Wpedantic -Wshadow -Wconversion $<$<BOOL:${OTK_WERROR}>:-Werror>)

function(otk_c11 target)
    set_target_properties(${target} PROPERTIES C_STANDARD 11 C_STANDARD_REQUIRED ON C_EXTENSIONS OFF)
endfunction()

# Zone 1: core. ISO C11 and its standard library only: no OS, no clock, no
# sockets. Builds on any host with a C11 compiler.
add_library(opentick_core STATIC
    core/src/signals.c
    core/src/scenario.c
    core/src/player.c
    core/src/can.c
    core/src/trace.c
    core/src/oracle.c
    core/src/link.c
    core/src/framelog.c
    core/src/bridge.c
)
add_library(opentick::core ALIAS opentick_core)
target_include_directories(opentick_core PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/core/include)
target_compile_options(opentick_core PRIVATE ${OTK_WARNINGS})
otk_c11(opentick_core)

# Zone 2: host. Drivers and bridges that need an operating system. Linux for
# now: the pacer uses clock_nanosleep and the CAN bridge uses SocketCAN.
if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
    add_library(opentick_host STATIC
        host/src/pacer.c
        host/src/driver.c
        host/src/socketcan.c
        host/src/hard.c
        host/src/hard_loop.c
    )
    add_library(opentick::host ALIAS opentick_host)
    target_include_directories(opentick_host PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/host/include)
    target_link_libraries(opentick_host PUBLIC opentick_core m)
    target_compile_options(opentick_host PRIVATE ${OTK_WARNINGS})
    otk_c11(opentick_host)
    # The hard driver's measuring tool (host/probe/hard_probe.c).
    add_executable(otk-hard-probe host/probe/hard_probe.c)
    target_link_libraries(otk-hard-probe PRIVATE opentick_host)
    target_compile_options(otk-hard-probe PRIVATE ${OTK_WARNINGS})
    otk_c11(otk-hard-probe)
    set(OTK_HAS_HOST ON)
else()
    set(OTK_HAS_HOST OFF)
    message(STATUS "open-tick: host zone (pacer, drivers, SocketCAN) is Linux-only; building core only")
endif()

if(OTK_BUILD_TESTS)
    enable_testing()
    add_subdirectory(tests)
endif()

if(OTK_BUILD_EXAMPLES AND OTK_HAS_HOST)
    add_subdirectory(examples)
endif()
