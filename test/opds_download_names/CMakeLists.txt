add_executable(OpdsDownloadNamesTest
  OpdsDownloadNamesTest.cpp
)

target_include_directories(OpdsDownloadNamesTest PRIVATE
  ${REPO_ROOT}/src/network
  ${REPO_ROOT}/src/util
)

target_link_libraries(OpdsDownloadNamesTest PRIVATE
  crosspoint_test_common
  GTest::gtest_main
)

gtest_discover_tests(OpdsDownloadNamesTest)
