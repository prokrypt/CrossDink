cmake_minimum_required(VERSION 3.16)
if(CMAKE_SOURCE_DIR STREQUAL CMAKE_CURRENT_SOURCE_DIR)
  project(date_formatting_tests LANGUAGES CXX)
  set(REPO_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/../..")
  enable_testing()
endif()
find_package(Python3 COMPONENTS Interpreter REQUIRED)
set(DATE_GENERATED "${CMAKE_CURRENT_BINARY_DIR}/generated")
file(MAKE_DIRECTORY "${DATE_GENERATED}")
add_custom_command(OUTPUT ${DATE_GENERATED}/I18nKeys.h ${DATE_GENERATED}/I18nStrings.h ${DATE_GENERATED}/I18nStrings.cpp
  COMMAND ${Python3_EXECUTABLE} ${REPO_ROOT}/scripts/gen_i18n.py ${REPO_ROOT}/lib/I18n/translations ${DATE_GENERATED}
  DEPENDS ${REPO_ROOT}/scripts/gen_i18n.py ${REPO_ROOT}/lib/I18n/translations/english.yaml
  WORKING_DIRECTORY ${REPO_ROOT} VERBATIM)
# Same host setup as language_cache's I18nSdTest: copies beside the generated headers, HalStorage stubs.
configure_file(${REPO_ROOT}/lib/I18n/I18n.h ${DATE_GENERATED}/I18n.h COPYONLY)
configure_file(${REPO_ROOT}/lib/I18n/I18n.cpp ${DATE_GENERATED}/I18n.cpp COPYONLY)
configure_file(${REPO_ROOT}/lib/I18n/DateFormatting.cpp ${DATE_GENERATED}/DateFormatting.cpp COPYONLY)
add_executable(DateFormattingTest DateFormattingTest.cpp ${DATE_GENERATED}/I18n.cpp ${DATE_GENERATED}/I18nStrings.cpp
  ${DATE_GENERATED}/DateFormatting.cpp ${REPO_ROOT}/lib/LanguageCache/LanguageCache.cpp)
target_include_directories(DateFormattingTest PRIVATE ${REPO_ROOT}/test/language_cache/stubs ${DATE_GENERATED}
  ${REPO_ROOT}/include ${REPO_ROOT}/lib/LanguageCache ${REPO_ROOT}/lib/Memory)
target_compile_definitions(DateFormattingTest PRIVATE SIMULATOR)
target_compile_features(DateFormattingTest PRIVATE cxx_std_20)
target_compile_options(DateFormattingTest PRIVATE -Wall -Wextra -Werror)
add_test(NAME DateFormattingTest COMMAND DateFormattingTest)
