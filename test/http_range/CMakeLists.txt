add_executable(HttpRangeTest HttpRangeTest.cpp)

target_compile_definitions(HttpRangeTest PRIVATE HTTP_RANGE_PARSE_ONLY)
target_include_directories(HttpRangeTest PRIVATE ${REPO_ROOT}/src/network)

target_link_libraries(HttpRangeTest PRIVATE
  crosspoint_test_common
  GTest::gtest_main
)

gtest_discover_tests(HttpRangeTest)
