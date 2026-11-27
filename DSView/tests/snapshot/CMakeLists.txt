cmake_minimum_required(VERSION 3.16)
project(DSViewSnapshotTests LANGUAGES C CXX)

if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE Release)
endif()

option(SNAPSHOT_TEST_ASAN "Build with AddressSanitizer (needs a compiler with its runtime installed)" OFF)

find_package(QT NAMES Qt6 Qt5 REQUIRED COMPONENTS Core)
find_package(Qt${QT_VERSION_MAJOR} REQUIRED COMPONENTS Core)
find_package(PkgConfig REQUIRED)
pkg_check_modules(GLIB REQUIRED glib-2.0)

set(APP_DIR ${CMAKE_CURRENT_SOURCE_DIR}/../..)
set(REPO_DIR ${APP_DIR}/..)

# The snapshot includes its neighbours by relative path, so it is compiled from
# a copy placed next to stub log and dsvdef headers
set(SNAPSHOT_TREE ${CMAKE_CURRENT_BINARY_DIR}/snapshot)
foreach(file pv/data/logicsnapshot.cpp pv/data/logicsnapshot.h
             pv/data/snapshot.cpp pv/data/snapshot.h pv/utility/array.h)
    configure_file(${APP_DIR}/${file} ${SNAPSHOT_TREE}/${file} COPYONLY)
endforeach()
foreach(file pv/log.h pv/dsvdef.h)
    configure_file(${CMAKE_CURRENT_SOURCE_DIR}/stubs/${file} ${SNAPSHOT_TREE}/${file} COPYONLY)
endforeach()

add_executable(rootnode-test rootnode.cpp
    ${SNAPSHOT_TREE}/pv/data/logicsnapshot.cpp ${SNAPSHOT_TREE}/pv/data/snapshot.cpp)
target_include_directories(rootnode-test PRIVATE
    ${SNAPSHOT_TREE} ${REPO_DIR}/libsigrok4DSL ${REPO_DIR}/common ${GLIB_INCLUDE_DIRS})
target_link_libraries(rootnode-test PRIVATE Qt${QT_VERSION_MAJOR}::Core ${GLIB_LIBRARIES})
target_compile_features(rootnode-test PRIVATE cxx_std_14)
# Asserts are live in the app. libstdc++'s checked subscripts make an access
# past the root node vector abort here, where the app reads past it silently.
# With another standard library only SNAPSHOT_TEST_ASAN catches it.
target_compile_options(rootnode-test PRIVATE -UNDEBUG -D_GLIBCXX_ASSERTIONS)
if(SNAPSHOT_TEST_ASAN)
    target_compile_options(rootnode-test PRIVATE -fsanitize=address -fno-omit-frame-pointer)
    target_link_options(rootnode-test PRIVATE -fsanitize=address)
endif()

add_executable(crop-test crop.cpp
    ${SNAPSHOT_TREE}/pv/data/logicsnapshot.cpp ${SNAPSHOT_TREE}/pv/data/snapshot.cpp)
target_include_directories(crop-test PRIVATE
    ${SNAPSHOT_TREE} ${REPO_DIR}/libsigrok4DSL ${REPO_DIR}/common ${GLIB_INCLUDE_DIRS})
target_link_libraries(crop-test PRIVATE Qt${QT_VERSION_MAJOR}::Core ${GLIB_LIBRARIES})
target_compile_features(crop-test PRIVATE cxx_std_14)
target_compile_options(crop-test PRIVATE -UNDEBUG -D_GLIBCXX_ASSERTIONS)
if(SNAPSHOT_TEST_ASAN)
    target_compile_options(crop-test PRIVATE -fsanitize=address -fno-omit-frame-pointer)
    target_compile_definitions(crop-test PRIVATE SNAPSHOT_TEST_ASAN)
    target_link_options(crop-test PRIVATE -fsanitize=address)
endif()

# One process per case: an overrun aborts, and the other cases should still run
enable_testing()
foreach(name one-node two-nodes mid-row short-of-node reuse loop)
    add_test(NAME rootnode-${name} COMMAND rootnode-test ${name})
endforeach()
foreach(name middle head tail block node loop invalid no-memory)
    add_test(NAME crop-${name} COMMAND crop-test ${name})
endforeach()
