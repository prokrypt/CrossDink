add_executable(TransferLightPulseTest
  TransferLightPulseTest.cpp
)

target_include_directories(TransferLightPulseTest PRIVATE
  ${REPO_ROOT}/src/util
)

target_link_libraries(TransferLightPulseTest PRIVATE
  crosspoint_test_common
  GTest::gtest_main
)

gtest_discover_tests(TransferLightPulseTest)
