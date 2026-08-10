add_executable(stream_decoder_tests stream_decoder_tests.cpp)
target_link_libraries(stream_decoder_tests PRIVATE fidget_core doctest::doctest)

add_test(NAME stream_decoder_tests COMMAND stream_decoder_tests)

add_executable(readout_protocol_tests readout_protocol_tests.cpp)
target_link_libraries(readout_protocol_tests PRIVATE fidget_core doctest::doctest)

add_test(NAME readout_protocol_tests COMMAND readout_protocol_tests)

add_executable(scp_registry_tests scp_registry_tests.cpp)
target_link_libraries(scp_registry_tests PRIVATE fidget_core doctest::doctest)

add_test(NAME scp_registry_tests COMMAND scp_registry_tests)

add_executable(scp_profile_tests scp_profile_tests.cpp)
target_link_libraries(scp_profile_tests PRIVATE fidget_core doctest::doctest)
target_compile_definitions(scp_profile_tests PRIVATE
    FIDGET_TEST_FIXTURE_DIR="${CMAKE_CURRENT_SOURCE_DIR}/fixtures"
)

add_test(NAME scp_profile_tests COMMAND scp_profile_tests)

add_executable(crate_project_tests crate_project_tests.cpp)
target_link_libraries(crate_project_tests PRIVATE fidget_core doctest::doctest)
target_compile_definitions(crate_project_tests PRIVATE
    FIDGET_TEST_FIXTURE_DIR="${CMAKE_CURRENT_SOURCE_DIR}/fixtures"
)

add_test(NAME crate_project_tests COMMAND crate_project_tests)

add_executable(guided_workflow_tests guided_workflow_tests.cpp)
target_link_libraries(guided_workflow_tests PRIVATE fidget_core doctest::doctest)

add_test(NAME guided_workflow_tests COMMAND guided_workflow_tests)

add_executable(scp_transaction_plan_tests scp_transaction_plan_tests.cpp)
target_link_libraries(scp_transaction_plan_tests PRIVATE
    fidget_core
    doctest::doctest
)

add_test(NAME scp_transaction_plan_tests COMMAND scp_transaction_plan_tests)
