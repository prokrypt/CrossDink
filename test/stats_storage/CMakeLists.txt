add_executable(StatsStorageFolderTest
 StatsStorageFolderTest.cpp
 ${REPO_ROOT}/src/util/StatsStorage.cpp
 ${REPO_ROOT}/src/activities/reader/DailyReadingStats.cpp
 ${REPO_ROOT}/src/activities/reader/ReadingSessionJournal.cpp
 ${REPO_ROOT}/src/activities/reader/ReadingStatsUtils.cpp
 ${REPO_ROOT}/src/sync/SyncFiles.cpp
 ${REPO_ROOT}/lib/uzlib/src/uzlib_checksums.c
)
# Not SIMULATOR: the twin (/.crosspoint) paths are under test.
target_include_directories(StatsStorageFolderTest PRIVATE
 ${CMAKE_CURRENT_SOURCE_DIR}/stubs ${REPO_ROOT}/test/reading_session_journal/stubs
 ${REPO_ROOT}/test/daily_reading_stats/stubs ${REPO_ROOT}/src ${REPO_ROOT}/src/activities/reader
 ${REPO_ROOT}/lib/uzlib/src)
target_compile_definitions(StatsStorageFolderTest PRIVATE DAILY_READING_STATS_TEST)
target_link_libraries(StatsStorageFolderTest PRIVATE crosspoint_test_common GTest::gtest_main)
gtest_discover_tests(StatsStorageFolderTest)
