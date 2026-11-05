add_executable(RemoteTokenPathTest
  RemoteTokenPathTest.cpp
  ${REPO_ROOT}/src/network/SerialRemote.cpp
)

target_include_directories(RemoteTokenPathTest PRIVATE
  ${REPO_ROOT}/src/network
)

target_link_libraries(RemoteTokenPathTest PRIVATE
  crosspoint_test_common
  GTest::gtest_main
)

gtest_discover_tests(RemoteTokenPathTest)
