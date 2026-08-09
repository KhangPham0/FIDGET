add_executable(sanity_tests sanity_tests.cpp)
target_link_libraries(sanity_tests PRIVATE doctest::doctest)

add_test(NAME sanity_tests COMMAND sanity_tests)

add_executable(stream_decoder_tests stream_decoder_tests.cpp)
target_link_libraries(stream_decoder_tests PRIVATE fidget_core doctest::doctest)

add_test(NAME stream_decoder_tests COMMAND stream_decoder_tests)

add_executable(readout_protocol_tests readout_protocol_tests.cpp)
target_link_libraries(readout_protocol_tests PRIVATE fidget_core doctest::doctest)

add_test(NAME readout_protocol_tests COMMAND readout_protocol_tests)
