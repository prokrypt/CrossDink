add_executable(ProgressRecordTest
  ProgressRecordTest.cpp
)

target_include_directories(ProgressRecordTest PRIVATE
  ${REPO_ROOT}/src/activities/reader
)

target_link_libraries(ProgressRecordTest PRIVATE
  crosspoint_test_common
  GTest::gtest_main
)

gtest_discover_tests(ProgressRecordTest)
