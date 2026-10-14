add_executable(BugReportSanitizeTest
  BugReportSanitizeTest.cpp
  ${REPO_ROOT}/src/util/BugReportSanitize.cpp
)

target_include_directories(BugReportSanitizeTest PRIVATE
  ${REPO_ROOT}/src/util
)

target_link_libraries(BugReportSanitizeTest PRIVATE
  crosspoint_test_common
  GTest::gtest_main
)

gtest_discover_tests(BugReportSanitizeTest)
