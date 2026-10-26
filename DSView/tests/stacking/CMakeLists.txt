cmake_minimum_required(VERSION 3.16)
project(DSViewStackingTests LANGUAGES C CXX)

if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE Release)
endif()

find_package(QT NAMES Qt6 Qt5 REQUIRED COMPONENTS Core)
find_package(Qt${QT_VERSION_MAJOR} REQUIRED COMPONENTS Core)
find_package(PkgConfig REQUIRED)
pkg_check_modules(GLIB REQUIRED glib-2.0)

set(APP_DIR ${CMAKE_CURRENT_SOURCE_DIR}/../..)
set(REPO_DIR ${APP_DIR}/..)

# The merger includes its neighbours by relative path, so it is compiled from a
# copy placed next to stub snapshot, log and language headers
set(MERGER_TREE ${CMAKE_CURRENT_BINARY_DIR}/merger)
foreach(file pv/data/logicstackingmerger.cpp pv/data/logicstackingmerger.h pv/logicstackingconfig.h)
    configure_file(${APP_DIR}/${file} ${MERGER_TREE}/${file} COPYONLY)
endforeach()
foreach(file pv/data/logicsnapshot.h pv/log.h pv/ui/langresource.h)
    configure_file(${CMAKE_CURRENT_SOURCE_DIR}/stubs/${file} ${MERGER_TREE}/${file} COPYONLY)
endforeach()

add_executable(stackingmerger-test stackingmerger.cpp ${MERGER_TREE}/pv/data/logicstackingmerger.cpp)
target_include_directories(stackingmerger-test PRIVATE
    ${MERGER_TREE} ${REPO_DIR}/libsigrok4DSL ${REPO_DIR}/common ${GLIB_INCLUDE_DIRS})
target_link_libraries(stackingmerger-test PRIVATE Qt${QT_VERSION_MAJOR}::Core ${GLIB_LIBRARIES})
target_compile_features(stackingmerger-test PRIVATE cxx_std_14)

enable_testing()
add_test(NAME stackingmerger COMMAND stackingmerger-test)
