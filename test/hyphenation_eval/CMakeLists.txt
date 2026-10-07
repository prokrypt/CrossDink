add_executable(HyphenationEvaluationTest
  HyphenationEvaluationTest.cpp
  ${REPO_ROOT}/lib/Epub/Epub/hyphenation/Hyphenator.cpp
  ${REPO_ROOT}/lib/Epub/Epub/hyphenation/LanguageRegistry.cpp
  ${REPO_ROOT}/lib/Epub/Epub/hyphenation/LiangHyphenation.cpp
  ${REPO_ROOT}/lib/Epub/Epub/hyphenation/HyphenationCommon.cpp
  ${REPO_ROOT}/lib/miniz/src/PackedAsset.cpp
  ${REPO_ROOT}/lib/miniz/src/InflateStream.cpp
  ${REPO_ROOT}/lib/miniz/src/miniz_impl.c
  ${REPO_ROOT}/lib/Memory/BuildScratch.cpp
  ${REPO_ROOT}/lib/Utf8/Utf8.cpp
)

target_include_directories(HyphenationEvaluationTest PRIVATE
  ${REPO_ROOT}/test/memory_policy/stubs
  ${REPO_ROOT}/lib/Epub
  ${REPO_ROOT}/lib/Utf8
  ${REPO_ROOT}/lib/Memory
  ${REPO_ROOT}/lib/MemoryBudget
  ${REPO_ROOT}/lib/miniz/src
)

target_compile_definitions(HyphenationEvaluationTest PRIVATE
  HYPHENATION_RESOURCES_DIR="${CMAKE_CURRENT_SOURCE_DIR}/resources"
)

target_link_libraries(HyphenationEvaluationTest PRIVATE
  crosspoint_test_common
  GTest::gtest_main
)

gtest_discover_tests(HyphenationEvaluationTest)
