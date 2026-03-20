BUILD_DIR := build

-include .env

export CLSPC_JAVA_BIN
export CLSPC_JDTLS_HOME
export CLSPC_TIMEOUT_BIN

# test 
T ?= .*

.PHONY: all config config-integ build integ test_all test_all_verbose test_one clean demo

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



test-all: build
	cd $(BUILD_DIR) && ctest --output-on-failure

test-all-verbose: build 
	cd $(BUILD_DIR) && ctest --output-on-failure -V

test-one: build
	cd $(BUILD_DIR) && ctest -R $(T) --output-on-failure -V

# test_integ: build-integ
# 	cd $(BUILD_DIR) && CLSPC_TEST_JDTLS_HOME="$(CLSPC_TEST_JDTLS_HOME)" ctest -R $(T) --output-on-failure -V

test-integ: build-integ
	cd $(BUILD_DIR) && ctest -R $(T) --output-on-failure -V

demo:
	cd $(BUILD_DIR) && ./dep_expand_demo \
		--java "$(CLSPC_JAVA_BIN)" \
		--jdtls-home "$(CLSPC_JDTLS_HOME)" \
		--root "$(CLSPC_DEMO_ROOT)" \
		--workspace "$(CLSPC_DEMO_WORKSPACE)" \
		--file "$(CLSPC_DEMO_FILE)" \
		--method "$(CLSPC_DEMO_METHOD)" \
		--max-depth "$(CLSPC_DEMO_MAX_DEPTH)"

demo2:
	cd $(BUILD_DIR) && ./dep_expand_demo2 \
		--java "$(CLSPC_JAVA_BIN)" \
		--jdtls-home "$(CLSPC_JDTLS_HOME)" \
		--root "$(CLSPC_DEMO_ROOT)" \
		--workspace "$(CLSPC_DEMO_WORKSPACE)" \
		--file "$(CLSPC_DEMO_FILE)" \
		--method "$(CLSPC_DEMO_METHOD)" \
		--max-depth 3 \
		--direction both \

demo2-trace:
	cd $(BUILD_DIR) && \
		CLSPC_TRACE_RPC=1 \
		CLSPC_TRACE_LSP=1 \
		./dep_expand_demo2 \
			--java "$(CLSPC_JAVA_BIN)" \
			--jdtls-home "$(CLSPC_JDTLS_HOME)" \
			--root "$(CLSPC_DEMO_ROOT)" \
			--workspace "$(CLSPC_DEMO_WORKSPACE)" \
			--file "$(CLSPC_DEMO_FILE)" \
			--method "$(CLSPC_DEMO_METHOD)" \
			--max-depth 3 \
			--direction both
