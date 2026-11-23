cmake_minimum_required(VERSION 3.16)
project(DSViewSearchTests LANGUAGES CXX)

find_package(QT NAMES Qt6 Qt5 REQUIRED COMPONENTS Core)
find_package(Qt${QT_VERSION_MAJOR} REQUIRED COMPONENTS Core)

if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE RelWithDebInfo)
endif()

add_executable(search-test
    searchtest.cpp
    ../../pv/search/searchquery.cpp
    ../../pv/search/searchengine.cpp)
target_link_libraries(search-test PRIVATE Qt${QT_VERSION_MAJOR}::Core)
target_compile_features(search-test PRIVATE cxx_std_14)
# The checks use assert-free comparisons, but keep asserts live like the app.
target_compile_options(search-test PRIVATE -UNDEBUG)

enable_testing()
add_test(NAME search COMMAND search-test)
