add_executable(ReadingSessionDatesTest
 ReadingSessionDatesTest.cpp
 ${REPO_ROOT}/src/activities/reader/ReadingSessionStats.cpp
 ${REPO_ROOT}/src/activities/reader/ReadingStatsUtils.cpp
)
target_include_directories(ReadingSessionDatesTest PRIVATE ${REPO_ROOT}/test/daily_reading_stats/stubs
 ${REPO_ROOT}/src/activities/reader)
target_link_libraries(ReadingSessionDatesTest PRIVATE crosspoint_test_common GTest::gtest_main)
gtest_discover_tests(ReadingSessionDatesTest)
