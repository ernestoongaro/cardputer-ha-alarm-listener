cmake_minimum_required(VERSION 3.16)
project(cardputer_alarm_detector_tests LANGUAGES CXX)

enable_testing()
add_executable(detector_tests
    test/test_alarm_detector.cpp
    src/alarm_detector.cpp
)
target_include_directories(detector_tests PRIVATE src)
target_compile_features(detector_tests PRIVATE cxx_std_17)
target_compile_options(detector_tests PRIVATE -Wall -Wextra -Wpedantic)
add_test(NAME detector_tests COMMAND detector_tests)

