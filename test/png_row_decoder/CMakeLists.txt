add_executable(PngRowDecoderTest PngRowDecoderTest.cpp
  ${REPO_ROOT}/lib/PngToBmpConverter/PngRowDecoder.cpp
  ${REPO_ROOT}/lib/miniz/src/InflateStream.cpp ${REPO_ROOT}/lib/miniz/src/miniz_impl.c
  ${REPO_ROOT}/lib/Memory/BuildScratch.cpp)
target_include_directories(PngRowDecoderTest PRIVATE ${REPO_ROOT}/test/memory_policy/stubs
  ${REPO_ROOT}/lib/PngToBmpConverter ${REPO_ROOT}/lib/Memory ${REPO_ROOT}/lib/MemoryBudget ${REPO_ROOT}/lib/miniz/src)
target_compile_definitions(PngRowDecoderTest PRIVATE ARDUINO_ARCH_ESP32=1 CROSSDINK_MEMORY_TEST=1)
target_link_libraries(PngRowDecoderTest PRIVATE crosspoint_test_common GTest::gtest_main)
gtest_discover_tests(PngRowDecoderTest PROPERTIES ENVIRONMENT "UBSAN_OPTIONS=halt_on_error=1")
