FetchContent_Declare(
  arduinojson
  GIT_REPOSITORY https://github.com/bblanchon/ArduinoJson.git
  GIT_TAG v7.4.2
)
FetchContent_MakeAvailable(arduinojson)

add_executable(ThemePackFieldsTest
  ThemePackFieldsTest.cpp
  ${REPO_ROOT}/src/components/ThemePackFields.cpp
)
target_include_directories(ThemePackFieldsTest PRIVATE
  ${REPO_ROOT}/src
  ${REPO_ROOT}/freeink-sdk/libs/assets/Icons/include
  ${arduinojson_SOURCE_DIR}/src
)
target_link_libraries(ThemePackFieldsTest PRIVATE crosspoint_test_common GTest::gtest_main)
gtest_discover_tests(ThemePackFieldsTest)
