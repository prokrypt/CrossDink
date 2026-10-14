add_executable(PowerLogRowTest PowerLogRowTest.cpp)
target_include_directories(PowerLogRowTest PRIVATE ${REPO_ROOT}/lib/Logging)
target_link_libraries(PowerLogRowTest PRIVATE crosspoint_test_common GTest::gtest_main)
gtest_discover_tests(PowerLogRowTest)
