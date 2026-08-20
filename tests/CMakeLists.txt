add_executable(activity_log_tests activity_log_tests.cpp)
target_link_libraries(activity_log_tests PRIVATE fidget_core doctest::doctest)

add_test(NAME activity_log_tests COMMAND activity_log_tests)

add_executable(bridge_protocol_tests bridge_protocol_tests.cpp)
target_link_libraries(bridge_protocol_tests PRIVATE
    fidget_core
    doctest::doctest
)

add_test(NAME bridge_protocol_tests COMMAND bridge_protocol_tests)

add_executable(bridge_relay_tests bridge_relay_tests.cpp)
target_link_libraries(bridge_relay_tests PRIVATE
    fidget_hardware
    doctest::doctest
)

add_test(NAME bridge_relay_tests COMMAND bridge_relay_tests)

add_executable(bridge_transport_tests bridge_transport_tests.cpp)
target_link_libraries(bridge_transport_tests PRIVATE
    fidget_hardware
    doctest::doctest
)

add_test(NAME bridge_transport_tests COMMAND bridge_transport_tests)

add_executable(mvme_export_tests mvme_export_tests.cpp)
target_link_libraries(mvme_export_tests PRIVATE fidget_core doctest::doctest)
target_compile_definitions(mvme_export_tests PRIVATE
    FIDGET_TEST_FIXTURE_DIR="${CMAKE_CURRENT_SOURCE_DIR}/fixtures"
)

add_test(NAME mvme_export_tests COMMAND mvme_export_tests)

add_executable(stream_decoder_tests stream_decoder_tests.cpp)
target_link_libraries(stream_decoder_tests PRIVATE fidget_core doctest::doctest)

add_test(NAME stream_decoder_tests COMMAND stream_decoder_tests)

add_executable(readout_protocol_tests readout_protocol_tests.cpp)
target_link_libraries(readout_protocol_tests PRIVATE fidget_core doctest::doctest)

add_test(NAME readout_protocol_tests COMMAND readout_protocol_tests)

add_executable(vme_protocol_tests vme_protocol_tests.cpp)
target_link_libraries(vme_protocol_tests PRIVATE fidget_core doctest::doctest)

add_test(NAME vme_protocol_tests COMMAND vme_protocol_tests)

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

add_executable(recovery_journal_tests recovery_journal_tests.cpp)
target_link_libraries(recovery_journal_tests PRIVATE
    fidget_core
    doctest::doctest
)

add_test(NAME recovery_journal_tests COMMAND recovery_journal_tests)

add_executable(recovery_verification_tests recovery_verification_tests.cpp)
target_link_libraries(recovery_verification_tests PRIVATE
    fidget_core
    doctest::doctest
)

add_test(NAME recovery_verification_tests COMMAND recovery_verification_tests)

add_executable(transport_interface_tests transport_interface_tests.cpp)
target_link_libraries(transport_interface_tests PRIVATE
    fidget_hardware
    doctest::doctest
)

add_test(NAME transport_interface_tests COMMAND transport_interface_tests)

add_executable(vme_transaction_tests vme_transaction_tests.cpp)
target_link_libraries(vme_transaction_tests PRIVATE
    fidget_hardware
    doctest::doctest
)

add_test(NAME vme_transaction_tests COMMAND vme_transaction_tests)

add_executable(mvlc_command_transport_tests mvlc_command_transport_tests.cpp)
target_link_libraries(mvlc_command_transport_tests PRIVATE
    fidget_hardware
    doctest::doctest
)

add_test(
    NAME mvlc_command_transport_tests
    COMMAND mvlc_command_transport_tests
)

add_executable(mvlc_data_receiver_tests mvlc_data_receiver_tests.cpp)
target_link_libraries(mvlc_data_receiver_tests PRIVATE
    fidget_hardware
    doctest::doctest
)

add_test(NAME mvlc_data_receiver_tests COMMAND mvlc_data_receiver_tests)

add_executable(acquisition_receiver_tests acquisition_receiver_tests.cpp)
target_link_libraries(acquisition_receiver_tests PRIVATE
    fidget_hardware
    doctest::doctest
)

add_test(NAME acquisition_receiver_tests COMMAND acquisition_receiver_tests)

add_executable(command_worker_tests command_worker_tests.cpp)
target_link_libraries(command_worker_tests PRIVATE
    fidget_hardware
    doctest::doctest
)

add_test(NAME command_worker_tests COMMAND command_worker_tests)

add_executable(tuner_snapshot_tests tuner_snapshot_tests.cpp)
target_link_libraries(tuner_snapshot_tests PRIVATE
    fidget_core
    doctest::doctest
)

add_test(NAME tuner_snapshot_tests COMMAND tuner_snapshot_tests)

add_executable(startup_audit_tests startup_audit_tests.cpp)
target_link_libraries(startup_audit_tests PRIVATE
    fidget_core
    doctest::doctest
)

add_test(NAME startup_audit_tests COMMAND startup_audit_tests)

add_executable(startup_preparation_tests startup_preparation_tests.cpp)
target_link_libraries(startup_preparation_tests PRIVATE
    fidget_core
    doctest::doctest
)

add_test(NAME startup_preparation_tests COMMAND startup_preparation_tests)

add_executable(
    startup_preparation_operation_tests
    startup_preparation_operation_tests.cpp
)
target_link_libraries(startup_preparation_operation_tests PRIVATE
    fidget_hardware
    doctest::doctest
)

add_test(
    NAME startup_preparation_operation_tests
    COMMAND startup_preparation_operation_tests
)

add_executable(
    deterministic_startup_operation_tests
    deterministic_startup_operation_tests.cpp
)
target_link_libraries(deterministic_startup_operation_tests PRIVATE
    fidget_hardware
    doctest::doctest
)

add_test(
    NAME deterministic_startup_operation_tests
    COMMAND deterministic_startup_operation_tests
)

add_executable(
    diagnostic_acquisition_operation_tests
    diagnostic_acquisition_operation_tests.cpp
)
target_link_libraries(diagnostic_acquisition_operation_tests PRIVATE
    fidget_hardware
    doctest::doctest
)

add_test(
    NAME diagnostic_acquisition_operation_tests
    COMMAND diagnostic_acquisition_operation_tests
)

add_executable(
    diagnostic_recovery_operation_tests
    diagnostic_recovery_operation_tests.cpp
)
target_link_libraries(diagnostic_recovery_operation_tests PRIVATE
    fidget_hardware
    doctest::doctest
)

add_test(
    NAME diagnostic_recovery_operation_tests
    COMMAND diagnostic_recovery_operation_tests
)

add_executable(
    diagnostic_acquisition_lifecycle_tests
    diagnostic_acquisition_lifecycle_tests.cpp
)
target_link_libraries(diagnostic_acquisition_lifecycle_tests PRIVATE
    fidget_hardware
    doctest::doctest
)

add_test(
    NAME diagnostic_acquisition_lifecycle_tests
    COMMAND diagnostic_acquisition_lifecycle_tests
)

add_executable(
    diagnostic_source_operation_tests
    diagnostic_source_operation_tests.cpp
)
target_link_libraries(diagnostic_source_operation_tests PRIVATE
    fidget_hardware
    doctest::doctest
)

add_test(
    NAME diagnostic_source_operation_tests
    COMMAND diagnostic_source_operation_tests
)

add_executable(
    diagnostic_preview_operation_tests
    diagnostic_preview_operation_tests.cpp
)
target_link_libraries(diagnostic_preview_operation_tests PRIVATE
    fidget_hardware
    doctest::doctest
)

add_test(
    NAME diagnostic_preview_operation_tests
    COMMAND diagnostic_preview_operation_tests
)

add_executable(ownership_service_tests ownership_service_tests.cpp)
target_link_libraries(ownership_service_tests PRIVATE
    fidget_hardware
    doctest::doctest
)

add_test(NAME ownership_service_tests COMMAND ownership_service_tests)

add_executable(scp_capture_operation_tests scp_capture_operation_tests.cpp)
target_link_libraries(scp_capture_operation_tests PRIVATE
    fidget_hardware
    doctest::doctest
)

add_test(NAME scp_capture_operation_tests COMMAND scp_capture_operation_tests)

add_executable(
    scp_single_repair_operation_tests
    scp_single_repair_operation_tests.cpp
)
target_link_libraries(scp_single_repair_operation_tests PRIVATE
    fidget_hardware
    doctest::doctest
)

add_test(
    NAME scp_single_repair_operation_tests
    COMMAND scp_single_repair_operation_tests
)

add_executable(
    scp_bulk_apply_operation_tests
    scp_bulk_apply_operation_tests.cpp
)
target_link_libraries(scp_bulk_apply_operation_tests PRIVATE
    fidget_hardware
    doctest::doctest
)

add_test(
    NAME scp_bulk_apply_operation_tests
    COMMAND scp_bulk_apply_operation_tests
)

add_executable(cli_tests cli_tests.cpp)
target_link_libraries(cli_tests PRIVATE
    fidget_cli_support
    fidget_core
    doctest::doctest
)

add_test(NAME cli_tests COMMAND cli_tests)
