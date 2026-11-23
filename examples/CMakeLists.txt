# Examples: small programs that show open-tick's pieces working together.
# They are run as tests too, so an example that stops working fails the build.

add_executable(otk-flywheel flywheel/flywheel.c)
target_compile_options(otk-flywheel PRIVATE ${OTK_WARNINGS})
otk_c11(otk-flywheel)
target_link_libraries(otk-flywheel PRIVATE opentick_core opentick_host m)
set_target_properties(otk-flywheel PROPERTIES RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR})

if(OTK_BUILD_TESTS)
    set(FLYWHEEL_SCENARIO ${CMAKE_CURRENT_SOURCE_DIR}/flywheel/step.csv)
    set(FLYWHEEL_OUT ${CMAKE_CURRENT_BINARY_DIR})

    # Free-run is deterministic: two runs give the same trace, byte for byte.
    add_test(NAME example_flywheel_run_a
             COMMAND otk-flywheel ${FLYWHEEL_SCENARIO} --quiet --trace ${FLYWHEEL_OUT}/flywheel_a.csv)
    add_test(NAME example_flywheel_run_b
             COMMAND otk-flywheel ${FLYWHEEL_SCENARIO} --quiet --trace ${FLYWHEEL_OUT}/flywheel_b.csv)
    set_tests_properties(example_flywheel_run_a example_flywheel_run_b PROPERTIES FIXTURES_SETUP flywheel_traces)
    add_test(NAME example_flywheel_deterministic
             COMMAND ${CMAKE_COMMAND} -E compare_files ${FLYWHEEL_OUT}/flywheel_a.csv ${FLYWHEEL_OUT}/flywheel_b.csv)
    set_tests_properties(example_flywheel_deterministic PROPERTIES FIXTURES_REQUIRED flywheel_traces)

    # The same program, paced by the wall clock: 5 s of scenario in about 5 s.
    add_test(NAME example_flywheel_paced
             COMMAND otk-flywheel ${FLYWHEEL_SCENARIO} --quiet --driver paced)

    # The oracle grades the toy controller by its DUTY frames alone.
    add_test(NAME example_flywheel_accept
             COMMAND otk-flywheel ${FLYWHEEL_SCENARIO} --quiet --criteria ${CMAKE_CURRENT_SOURCE_DIR}/flywheel/step.accept.csv)
endif()
