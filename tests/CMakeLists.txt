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

add_test(NAME test_signals COMMAND test_signals)
add_test(NAME test_scenario COMMAND test_scenario ${CMAKE_CURRENT_SOURCE_DIR}/data ${CMAKE_CURRENT_BINARY_DIR})
add_test(NAME test_player COMMAND test_player)
add_test(NAME test_can COMMAND test_can)
add_test(NAME test_trace COMMAND test_trace)
add_test(NAME test_version COMMAND test_version)
add_test(NAME test_oracle COMMAND test_oracle)
add_test(NAME test_link COMMAND test_link)

if(OTK_HAS_HOST)
    otk_test(test_driver opentick_host)
    otk_test(test_socketcan opentick_host)
    add_test(NAME test_driver COMMAND test_driver)
    add_test(NAME test_socketcan COMMAND test_socketcan)
    set_tests_properties(test_socketcan PROPERTIES SKIP_RETURN_CODE 77)
endif()
