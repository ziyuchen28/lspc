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
    src/session.cpp
    src/inspect.cpp
    src/source_window.cpp
)

target_compile_features(clspc PUBLIC cxx_std_20)

target_include_directories(clspc
    PUBLIC
        ${CMAKE_CURRENT_SOURCE_DIR}/include
)

target_link_libraries(clspc
    PUBLIC
        pcr::proc
    PRIVATE
        pcr::channel
        pcr::framing
        pcr::rpc_nlohmann
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


    add_executable(test_session_initialize
        tests/test_session_initialize.cpp
    )
    target_link_libraries(test_session_initialize PRIVATE clspc)
    add_test(NAME test_session_initialize COMMAND test_session_initialize)


    add_executable(test_session_document_sync
        tests/test_session_document_sync.cpp
    )
    target_link_libraries(test_session_document_sync PRIVATE clspc)
    add_test(NAME test_session_document_sync COMMAND test_session_document_sync)


    add_executable(test_session_document_symbols
        tests/test_session_document_symbols.cpp
    )
    target_link_libraries(test_session_document_symbols PRIVATE clspc)
    add_test(NAME test_session_document_symbols COMMAND test_session_document_symbols)

    add_executable(test_session_definition
        tests/test_session_definition.cpp
    )
    target_link_libraries(test_session_definition PRIVATE clspc)
    add_test(NAME test_session_definition COMMAND test_session_definition)

    add_executable(test_session_call_hierarchy
        tests/test_session_call_hierarchy.cpp
    )
    target_link_libraries(test_session_call_hierarchy PRIVATE clspc)
    add_test(NAME test_session_call_hierarchy COMMAND test_session_call_hierarchy)

    add_executable(test_source_window
        tests/test_source_window.cpp
    )
    target_link_libraries(test_source_window PRIVATE clspc)
    add_test(NAME test_source_window COMMAND test_source_window)


endif()



if (CLSPC_BUILD_INTEGRATION_TESTS)

    add_executable(test_jdtls_real_spawn
        tests/test_jdtls_real_spawn.cpp
    )
    target_link_libraries(test_jdtls_real_spawn PRIVATE clspc)
    add_test(NAME test_jdtls_real_spawn COMMAND test_jdtls_real_spawn)


    add_executable(test_session_initialize_real
        tests/test_session_initialize_real.cpp
    )
    target_link_libraries(test_session_initialize_real PRIVATE clspc)
    add_test(NAME test_session_initialize_real COMMAND test_session_initialize_real)

    add_executable(test_session_document_symbols_real
        tests/test_session_document_symbols_real.cpp
    )
    target_link_libraries(test_session_document_symbols_real PRIVATE clspc)
    add_test(NAME test_session_document_symbols_real COMMAND test_session_document_symbols_real)

    add_executable(test_session_call_hierarchy_real
        tests/test_session_call_hierarchy_real.cpp
    )
    target_link_libraries(test_session_call_hierarchy_real PRIVATE clspc)
    add_test(NAME test_session_call_hierarchy_real COMMAND test_session_call_hierarchy_real)

    add_executable(dep_expand_demo
        tools/dep_expand_demo.cpp
    )
    target_link_libraries(dep_expand_demo PRIVATE clspc)


endif()



