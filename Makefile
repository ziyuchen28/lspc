BUILD_DIR := build

-include .env

# test 
T ?= .*

.PHONY: all config config-integ build integ test_all test_all_verbose test_one clean

all: config build

integ: config-integ build

config:
	cmake -S . -B $(BUILD_DIR) -DCLSPC_BUILD_TESTS=ON -DCLSPC_BUILD_INTEGRATION_TESTS=OFF

config-integ:
	cmake -S . -B $(BUILD_DIR) -DCLSPC_BUILD_TESTS=ON -DCLSPC_BUILD_INTEGRATION_TESTS=ON

build: config
	cmake --build $(BUILD_DIR) -j -- --no-print-directory

build-integ: config-integ
	cmake --build $(BUILD_DIR) -j -- --no-print-directory

clean:
	rm -rf $(BUILD_DIR)



test_all: build
	cd $(BUILD_DIR) && ctest --output-on-failure

test_all_verbose: build 
	cd $(BUILD_DIR) && ctest --output-on-failure -V

test_one: build
	cd $(BUILD_DIR) && ctest -R $(T) --output-on-failure -V

test_integ: build-integ
	cd $(BUILD_DIR) && CLSPC_TEST_JDTLS_HOME="$(CLSPC_TEST_JDTLS_HOME)" ctest -R $(T) --output-on-failure -V





