cmake_minimum_required(VERSION 3.20)
project(clspc LANGUAGES CXX)


set(CMAKE_EXPORT_COMPILE_COMMANDS ON)


if(PROJECT_IS_TOP_LEVEL)
    option(CLSPC_BUILD_TESTS "Build clspc tests" ON)
else()
    option(CLSPC_BUILD_TESTS "Build clspc tests" OFF)
endif()


add_library(clspc
    src/uri.cpp
)

target_compile_features(clspc PUBLIC cxx_std_20)

target_include_directories(clspc
    PUBLIC
        ${CMAKE_CURRENT_SOURCE_DIR}/include
)

if (CLSPC_BUILD_TESTS)
    enable_testing()
    add_executable(test_uri
        tests/test_uri.cpp
    )
    target_link_libraries(test_uri PRIVATE clspc)
    add_test(NAME test_uri COMMAND test_uri)
endif()
