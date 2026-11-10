FetchContent_Declare(
  arduinojson
  GIT_REPOSITORY https://github.com/bblanchon/ArduinoJson.git
  GIT_TAG v7.4.2
)
FetchContent_MakeAvailable(arduinojson)

add_executable(ThemePackHomeTest
  ThemePackHomeTest.cpp
  ${REPO_ROOT}/src/components/ThemePackHome.cpp
)
target_include_directories(ThemePackHomeTest PRIVATE
  ${CMAKE_CURRENT_SOURCE_DIR}/stubs
  ${REPO_ROOT}/src
  ${arduinojson_SOURCE_DIR}/src
)
target_link_libraries(ThemePackHomeTest PRIVATE crosspoint_test_common GTest::gtest_main)
target_compile_definitions(ThemePackHomeTest PRIVATE REPO_ROOT_DIR="${REPO_ROOT}")
gtest_discover_tests(ThemePackHomeTest)
