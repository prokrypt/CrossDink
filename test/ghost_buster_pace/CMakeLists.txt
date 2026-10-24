add_executable(GhostBusterPaceTest
  GhostBusterPaceTest.cpp
)

target_include_directories(GhostBusterPaceTest PRIVATE
  ${REPO_ROOT}/src/activities
)

target_link_libraries(GhostBusterPaceTest PRIVATE
  crosspoint_test_common
  GTest::gtest_main
)

gtest_discover_tests(GhostBusterPaceTest)
