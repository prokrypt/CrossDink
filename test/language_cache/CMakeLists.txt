cmake_minimum_required(VERSION 3.16)
if(CMAKE_SOURCE_DIR STREQUAL CMAKE_CURRENT_SOURCE_DIR)
  project(language_cache_tests LANGUAGES CXX)
  set(REPO_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/../..")
  enable_testing()
endif()
add_executable(LanguageCacheTest LanguageCacheTest.cpp ${REPO_ROOT}/lib/LanguageCache/LanguageCache.cpp)
target_include_directories(LanguageCacheTest PRIVATE ${REPO_ROOT}/lib/LanguageCache ${REPO_ROOT}/lib/Memory)
target_compile_features(LanguageCacheTest PRIVATE cxx_std_20)
target_compile_options(LanguageCacheTest PRIVATE -Wall -Wextra -Werror)
add_test(NAME LanguageCacheTest COMMAND LanguageCacheTest)

find_package(Python3 COMPONENTS Interpreter REQUIRED)
set(LANGUAGE_GENERATED "${CMAKE_CURRENT_BINARY_DIR}/generated")
file(MAKE_DIRECTORY "${LANGUAGE_GENERATED}")
file(GLOB_RECURSE LANGUAGE_USAGE CONFIGURE_DEPENDS ${REPO_ROOT}/src/*.cpp ${REPO_ROOT}/src/*.h
  ${REPO_ROOT}/lib/*.cpp ${REPO_ROOT}/lib/*.h)
list(FILTER LANGUAGE_USAGE EXCLUDE REGEX "I18n(Keys|Strings)\\.(cpp|h)$")
add_custom_command(OUTPUT ${LANGUAGE_GENERATED}/I18nKeys.h ${LANGUAGE_GENERATED}/I18nStrings.h
  ${LANGUAGE_GENERATED}/I18nStrings.cpp
  COMMAND ${Python3_EXECUTABLE} ${REPO_ROOT}/scripts/gen_i18n.py
  ${REPO_ROOT}/lib/I18n/translations ${LANGUAGE_GENERATED} --strip-unused
  --src-dirs ${REPO_ROOT}/src ${REPO_ROOT}/lib
  DEPENDS ${REPO_ROOT}/scripts/gen_i18n.py ${REPO_ROOT}/lib/I18n/translations/english.yaml ${LANGUAGE_USAGE}
  WORKING_DIRECTORY ${REPO_ROOT} VERBATIM)
# I18n.cpp on the host: real LanguageCache and generated strings, filesystem stubs for HalStorage.
add_executable(I18nSdTest I18nSdTest.cpp ${REPO_ROOT}/lib/I18n/I18n.cpp ${LANGUAGE_GENERATED}/I18nStrings.cpp
  ${REPO_ROOT}/lib/LanguageCache/LanguageCache.cpp)
target_include_directories(I18nSdTest PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/stubs ${LANGUAGE_GENERATED}
  ${REPO_ROOT}/lib/I18n ${REPO_ROOT}/lib/LanguageCache ${REPO_ROOT}/lib/Memory)
target_compile_definitions(I18nSdTest PRIVATE SIMULATOR)
target_compile_features(I18nSdTest PRIVATE cxx_std_20)
target_compile_options(I18nSdTest PRIVATE -Wall -Wextra -Werror)
add_test(NAME I18nSdTest COMMAND I18nSdTest)
