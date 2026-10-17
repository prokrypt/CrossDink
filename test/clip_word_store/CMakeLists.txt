add_executable(ClipWordStoreTest
  ClipWordStoreTest.cpp
  ${REPO_ROOT}/src/clippings/ClipTextBuilder.cpp
  ${REPO_ROOT}/lib/Utf8/Utf8.cpp
)

target_link_libraries(ClipWordStoreTest PRIVATE
  crosspoint_test_common
  GTest::gtest_main
)

target_include_directories(ClipWordStoreTest PRIVATE
  ${REPO_ROOT}/test/minibidi_arabic/stubs  # no-op Logging.h for host tests
  ${REPO_ROOT}/src
  ${REPO_ROOT}/src/clippings
  ${REPO_ROOT}/lib/EpdFont
  ${REPO_ROOT}/lib/Utf8
)

gtest_discover_tests(ClipWordStoreTest)
