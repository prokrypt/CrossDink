add_executable(OpdsParserTest
  OpdsParserTest.cpp
  OpdsJsonParserTest.cpp
  OpdsUrlTest.cpp
  ${REPO_ROOT}/src/util/UrlUtils.cpp
  ${REPO_ROOT}/lib/OpdsParser/OpdsParser.cpp
  ${REPO_ROOT}/lib/OpdsParser/OpdsJsonParser.cpp
  ${REPO_ROOT}/lib/OpdsParser/OpdsSearchTemplate.cpp
  ${REPO_ROOT}/lib/JsonParser/StreamingJsonParser.cpp
  ${REPO_ROOT}/lib/expat/xmlparse.c
  ${REPO_ROOT}/lib/expat/xmlrole.c
  ${REPO_ROOT}/lib/expat/xmltok.c
)

target_include_directories(OpdsParserTest PRIVATE
  stubs
  ${REPO_ROOT}/src
  ${REPO_ROOT}/lib/JsonParser
  ${REPO_ROOT}/lib/OpdsParser
  ${REPO_ROOT}/lib/XmlParserUtils
  ${REPO_ROOT}/lib/Memory
  ${REPO_ROOT}/lib/expat
)

target_compile_definitions(OpdsParserTest PRIVATE XML_CONTEXT_BYTES=0 XML_GE=0)

target_link_libraries(OpdsParserTest PRIVATE
  crosspoint_test_common
  GTest::gtest_main
)

gtest_discover_tests(OpdsParserTest)
