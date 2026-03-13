export CLSPC_TEST_JDTLS_HOME=/absolute/path/to/extracted/jdtls

ctest --test-dir build -V -R test_jdtls_real_spawn --output-on-failure
