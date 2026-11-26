cmake_minimum_required(VERSION 3.16)
project(DSViewConditionTests LANGUAGES C)

if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE Release)
endif()

option(CONDITION_TEST_ASAN "Build with AddressSanitizer (needs a compiler with its runtime installed)" OFF)

find_package(PkgConfig REQUIRED)
pkg_check_modules(GLIB REQUIRED glib-2.0)
# Headers only: libsigrokdecode-internal.h includes Python.h, the matcher makes no Python calls
find_package(Python3 REQUIRED COMPONENTS Development)

set(REPO_DIR ${CMAKE_CURRENT_SOURCE_DIR}/../../..)

# match.c is compiled from the source tree, next to the old matcher
# (reference_match.c) and a stub for the log calls
add_executable(condition-test conditiontest.c reference_match.c stub_log.c
    ${REPO_DIR}/libsigrokdecode4DSL/match.c)
target_include_directories(condition-test PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR} ${REPO_DIR}/libsigrokdecode4DSL ${REPO_DIR}/common
    ${GLIB_INCLUDE_DIRS} ${Python3_INCLUDE_DIRS})
target_link_libraries(condition-test PRIVATE ${GLIB_LIBRARIES})
# Release gives -O3 like the app; keep the test's own checks
target_compile_options(condition-test PRIVATE -UNDEBUG)
if(CONDITION_TEST_ASAN)
    target_compile_options(condition-test PRIVATE -fsanitize=address -fno-omit-frame-pointer)
    target_link_options(condition-test PRIVATE -fsanitize=address)
endif()

enable_testing()
add_test(NAME conditions COMMAND condition-test)
