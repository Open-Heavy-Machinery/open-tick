# Unit tests. Headless; only test_driver touches the wall clock, and only
# test_socketcan needs a CAN interface (it is skipped without one). They use
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
target_compile_definitions(test_version PRIVATE OTK_PROJECT_VERSION="${PROJECT_VERSION}")
otk_test(test_oracle opentick_core)
otk_test(test_link opentick_core)
otk_test(test_framelog opentick_core)
otk_test(test_bridge opentick_core)

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

if(OTK_HAS_HOST)
    otk_test(test_driver opentick_host)
    otk_test(test_socketcan opentick_host)
    find_package(Threads REQUIRED)
    otk_test(test_hard opentick_host Threads::Threads util)
    add_test(NAME test_driver COMMAND test_driver)
    add_test(NAME test_hard COMMAND test_hard)
    add_test(NAME test_socketcan COMMAND test_socketcan)
    set_tests_properties(test_socketcan PROPERTIES SKIP_RETURN_CODE 77)
endif()

# The gateware's system testbench takes its script from the codec and is
# judged by it (gateware/tang_nano_20k/sim/run.sh, which needs iverilog).
# Here only the generator runs, so the scenario stays buildable and valid C.
add_executable(otk-bridge-vectors ${PROJECT_SOURCE_DIR}/gateware/tang_nano_20k/sim/bridge_vectors.c)
target_compile_options(otk-bridge-vectors PRIVATE ${OTK_WARNINGS})
otk_c11(otk-bridge-vectors)
target_link_libraries(otk-bridge-vectors PRIVATE opentick_core)
add_test(NAME bridge_vectors_gen COMMAND otk-bridge-vectors gen ${CMAKE_CURRENT_BINARY_DIR}/bridge_stimulus.txt)
