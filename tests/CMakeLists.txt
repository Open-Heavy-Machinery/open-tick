# Unit tests. Headless; test_driver and the hard driver's tests (against a
# fake bridge on a pseudo-terminal) touch the wall clock, and test_socketcan
# and test_hard_can need a CAN interface (they are skipped without one). They use
# CHECK (tests/check.h), never assert(), so Release builds still test.

function(otk_test name)
    add_executable(${name} ${name}.c)
    target_compile_options(${name} PRIVATE ${OTK_WARNINGS})
    otk_c11(${name})
    target_link_libraries(${name} PRIVATE ${ARGN})
endfunction()

otk_test(test_signals opentick_core)
otk_test(test_scenario opentick_core)
otk_test(test_player opentick_core)
otk_test(test_can opentick_core)
otk_test(test_trace opentick_core)
otk_test(test_version opentick_core)
if(OTK_VERSION_PRERELEASE)
    set(OTK_FULL_VERSION "${PROJECT_VERSION}-${OTK_VERSION_PRERELEASE}")
else()
    set(OTK_FULL_VERSION "${PROJECT_VERSION}")
endif()
target_compile_definitions(test_version PRIVATE OTK_PROJECT_VERSION="${OTK_FULL_VERSION}"
                                                OTK_PROJECT_PRERELEASE="${OTK_VERSION_PRERELEASE}")
otk_test(test_oracle opentick_core)
otk_test(test_link opentick_core)
otk_test(test_framelog opentick_core)
otk_test(test_bridge opentick_core)
otk_test(test_fidelity opentick_core)

add_test(NAME test_signals COMMAND test_signals)
add_test(NAME test_scenario COMMAND test_scenario ${CMAKE_CURRENT_SOURCE_DIR}/data ${CMAKE_CURRENT_BINARY_DIR})
add_test(NAME test_player COMMAND test_player)
add_test(NAME test_can COMMAND test_can)
add_test(NAME test_trace COMMAND test_trace)
add_test(NAME test_version COMMAND test_version)
add_test(NAME test_oracle COMMAND test_oracle)
add_test(NAME test_link COMMAND test_link)
add_test(NAME test_framelog COMMAND test_framelog)
add_test(NAME test_bridge COMMAND test_bridge)
add_test(NAME test_fidelity COMMAND test_fidelity)

if(OTK_HAS_HOST)
    otk_test(test_driver opentick_host)
    otk_test(test_socketcan opentick_host)
    find_package(Threads REQUIRED)
    otk_test(test_hard opentick_host Threads::Threads util)
    otk_test(test_hard_loop opentick_host Threads::Threads util m)
    target_sources(test_hard_loop PRIVATE fake_bridge.c)
    otk_test(test_hard_can opentick_host Threads::Threads util m)
    target_sources(test_hard_can PRIVATE fake_bridge.c)
    add_test(NAME test_driver COMMAND test_driver)
    add_test(NAME test_hard COMMAND test_hard)
    add_test(NAME test_hard_loop COMMAND test_hard_loop)
    add_test(NAME test_hard_can COMMAND test_hard_can)
    add_test(NAME test_socketcan COMMAND test_socketcan)
    set_tests_properties(test_socketcan test_hard_can PROPERTIES SKIP_RETURN_CODE 77)
    # The fake bridge as a program, and otk-flywheel --driver hard against it.
    add_executable(otk-fake-bridge fake_bridge_main.c fake_bridge.c)
    target_compile_options(otk-fake-bridge PRIVATE ${OTK_WARNINGS})
    otk_c11(otk-fake-bridge)
    target_link_libraries(otk-fake-bridge PRIVATE opentick_core Threads::Threads util m)
    if(OTK_BUILD_EXAMPLES)
        add_test(NAME example_flywheel_hard
                 COMMAND ${CMAKE_CURRENT_SOURCE_DIR}/hard_flywheel.sh ${CMAKE_BINARY_DIR}
                         ${CMAKE_CURRENT_SOURCE_DIR}/data/hard_short.csv ${CMAKE_CURRENT_BINARY_DIR}/hard_flywheel)
        set_tests_properties(example_flywheel_hard PROPERTIES RUN_SERIAL TRUE RESOURCE_LOCK can_interface)
    endif()
    # The hard driver's tests run in real time against the fake bridge: alone,
    # so that a parallel ctest does not make them miss steps. The tests that
    # use the CAN interface take it one at a time (each sees the others' frames).
    set_tests_properties(test_hard_loop test_hard_can PROPERTIES RUN_SERIAL TRUE)
    set_tests_properties(test_socketcan test_hard_can PROPERTIES RESOURCE_LOCK can_interface)
endif()

# The gateware's system testbench takes its script from the codec and is
# judged by it (gateware/tang_nano_20k/sim/run.sh, which needs iverilog).
# Here only the generator runs, so the scenario stays buildable and valid C.
add_executable(otk-bridge-vectors ${PROJECT_SOURCE_DIR}/gateware/tang_nano_20k/sim/bridge_vectors.c)
target_compile_options(otk-bridge-vectors PRIVATE ${OTK_WARNINGS})
otk_c11(otk-bridge-vectors)
target_link_libraries(otk-bridge-vectors PRIVATE opentick_core)
add_test(NAME bridge_vectors_gen COMMAND otk-bridge-vectors gen ${CMAKE_CURRENT_BINARY_DIR}/bridge_stimulus.txt)
