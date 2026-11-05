add_executable(StoredZipWriterTest
  StoredZipWriterTest.cpp
  ${REPO_ROOT}/src/util/StoredZip.cpp
)

target_include_directories(StoredZipWriterTest PRIVATE
  ${REPO_ROOT}/src/util
)

target_link_libraries(StoredZipWriterTest PRIVATE
  crosspoint_test_common
  GTest::gtest_main
)

gtest_discover_tests(StoredZipWriterTest)
