# mod-custom-items — per-module .cmake hook. Runs inline during the
# modules/CMakeLists.txt configure pass (see the OPTIONAL include at the
# bottom of that file). Registers the EvaluateRewriteEntry pure-function
# unit tests into AC's `unit_tests` target via the two global properties
# read by src/test/CMakeLists.txt.
#
# Tests only compile when -DBUILD_TESTING=ON is set, same as the rest of
# AC's test suite. The module-source build is unaffected either way.

get_property(_ACORE_MODULE_TEST_SOURCES GLOBAL PROPERTY ACORE_MODULE_TEST_SOURCES)
list(APPEND _ACORE_MODULE_TEST_SOURCES
    "${CMAKE_CURRENT_LIST_DIR}/tests/CustomItemsTests.cpp")
set_property(GLOBAL PROPERTY ACORE_MODULE_TEST_SOURCES
    "${_ACORE_MODULE_TEST_SOURCES}")

get_property(_ACORE_MODULE_TEST_INCLUDES GLOBAL PROPERTY ACORE_MODULE_TEST_INCLUDES)
list(APPEND _ACORE_MODULE_TEST_INCLUDES
    "${CMAKE_CURRENT_LIST_DIR}/src")
set_property(GLOBAL PROPERTY ACORE_MODULE_TEST_INCLUDES
    "${_ACORE_MODULE_TEST_INCLUDES}")
