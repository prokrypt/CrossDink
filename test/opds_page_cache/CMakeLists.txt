add_executable(OpdsPageCacheTest
  OpdsPageCacheTest.cpp
  ${REPO_ROOT}/src/activities/browser/OpdsPageCache.cpp
)

target_include_directories(OpdsPageCacheTest PRIVATE
  ${REPO_ROOT}/src/activities/browser
  ${REPO_ROOT}/lib/Memory
)

target_link_libraries(OpdsPageCacheTest PRIVATE
  crosspoint_test_common
  GTest::gtest_main
)

gtest_discover_tests(OpdsPageCacheTest)
