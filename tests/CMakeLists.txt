add_executable(sanity_tests sanity_tests.cpp)
target_link_libraries(sanity_tests PRIVATE doctest::doctest)

add_test(NAME sanity_tests COMMAND sanity_tests)
