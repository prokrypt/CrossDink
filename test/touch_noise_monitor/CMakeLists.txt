add_executable(TouchNoiseMonitorTest
  TouchNoiseMonitorTest.cpp
)

target_include_directories(TouchNoiseMonitorTest PRIVATE
  ${REPO_ROOT}/src
)

target_link_libraries(TouchNoiseMonitorTest PRIVATE
  crosspoint_test_common
  GTest::gtest_main
)

gtest_discover_tests(TouchNoiseMonitorTest)
