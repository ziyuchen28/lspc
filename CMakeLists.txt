cmake_minimum_required(VERSION 3.20)
project(clspc LANGUAGES CXX)


set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

add_subdirectory(external/pcr)


option(CLSPC_BUILD_INTEGRATION_TESTS "Build integration tests requiring a real JDTLS install" OFF)
if(PROJECT_IS_TOP_LEVEL)
    option(CLSPC_BUILD_TESTS "Build clspc tests" ON)
else()
    option(CLSPC_BUILD_TESTS "Build clspc tests" OFF)
endif()


add_library(clspc
    src/uri.cpp
    src/jdtls_paths.cpp
    src/jdtls_command.cpp
    src/jdtls_spawn.cpp
)

target_compile_features(clspc PUBLIC cxx_std_20)

target_include_directories(clspc
    PUBLIC
        ${CMAKE_CURRENT_SOURCE_DIR}/include
)

target_link_libraries(clspc
    PUBLIC
        pcr::proc
)


if (CLSPC_BUILD_TESTS)
    enable_testing()

    add_executable(test_uri
        tests/test_uri.cpp
    )
    target_link_libraries(test_uri PRIVATE clspc)
    add_test(NAME test_uri COMMAND test_uri)
    
    add_executable(test_jdtls_paths
        tests/test_jdtls_paths.cpp
    )
    target_link_libraries(test_jdtls_paths PRIVATE clspc)
    add_test(NAME test_jdtls_paths COMMAND test_jdtls_paths)


    add_executable(test_jdtls_command
        tests/test_jdtls_command.cpp
    )
    target_link_libraries(test_jdtls_command PRIVATE clspc)
    add_test(NAME test_jdtls_command COMMAND test_jdtls_command)

    add_executable(test_jdtls_spawn
        tests/test_jdtls_spawn.cpp
    )
    target_link_libraries(test_jdtls_spawn PRIVATE clspc)
    add_test(NAME test_jdtls_spawn COMMAND test_jdtls_spawn)

endif()

if (CLSPC_BUILD_INTEGRATION_TESTS)
    add_executable(test_jdtls_real_spawn
        tests/test_jdtls_real_spawn.cpp
    )
    target_link_libraries(test_jdtls_real_spawn PRIVATE clspc)
    add_test(NAME test_jdtls_real_spawn COMMAND test_jdtls_real_spawn)
endif()
