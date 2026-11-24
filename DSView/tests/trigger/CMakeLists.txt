cmake_minimum_required(VERSION 3.16)
project(DSViewBusTriggerTests LANGUAGES CXX)

find_package(QT NAMES Qt6 Qt5 REQUIRED COMPONENTS Core)
find_package(Qt${QT_VERSION_MAJOR} REQUIRED COMPONENTS Core)

add_executable(buspattern-test buspattern.cpp ../../pv/trigger/buspattern.cpp)
target_link_libraries(buspattern-test PRIVATE Qt${QT_VERSION_MAJOR}::Core)
target_compile_features(buspattern-test PRIVATE cxx_std_11)

enable_testing()
add_test(NAME buspattern COMMAND buspattern-test)
