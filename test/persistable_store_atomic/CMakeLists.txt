# ArduinoJson is a PlatformIO dependency, so fetch the same pinned version here
# instead of relying on a prior simulator build populating .pio/libdeps.
FetchContent_Declare(
  ArduinoJson
  GIT_REPOSITORY https://github.com/bblanchon/ArduinoJson.git
  GIT_TAG v7.4.2
)
FetchContent_MakeAvailable(ArduinoJson)

add_executable(PersistableStoreAtomicTest
  PersistableStoreAtomicTest.cpp
  ${REPO_ROOT}/lib/Serialization/PersistableStore.cpp
)

target_include_directories(PersistableStoreAtomicTest PRIVATE
  ${CMAKE_CURRENT_SOURCE_DIR}/stubs
  ${REPO_ROOT}/lib/Serialization
)

target_link_libraries(PersistableStoreAtomicTest PRIVATE
  crosspoint_test_common
  ArduinoJson
  GTest::gtest_main
  ArduinoJson
)

gtest_discover_tests(PersistableStoreAtomicTest)

add_executable(CrossPointStateTest
  CrossPointStateTest.cpp
  ${REPO_ROOT}/src/CrossPointState.cpp
  ${REPO_ROOT}/lib/Serialization/PersistableStore.cpp
  ${REPO_ROOT}/lib/uzlib/src/uzlib_checksums.c
)
target_include_directories(CrossPointStateTest PRIVATE
  ${CMAKE_CURRENT_SOURCE_DIR}/stubs
  ${REPO_ROOT}/src
  ${REPO_ROOT}/lib/Serialization
  ${REPO_ROOT}/lib/uzlib/src
)
target_link_libraries(CrossPointStateTest PRIVATE crosspoint_test_common GTest::gtest_main ArduinoJson)
gtest_discover_tests(CrossPointStateTest)
