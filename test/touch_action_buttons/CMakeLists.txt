add_executable(TouchActionButtonsTest TouchActionButtonsTest.cpp)
target_include_directories(TouchActionButtonsTest PRIVATE
  ${REPO_ROOT}/src/components
  ${REPO_ROOT}/src
)
target_link_libraries(TouchActionButtonsTest PRIVATE crosspoint_test_common GTest::gtest_main)
gtest_discover_tests(TouchActionButtonsTest)
