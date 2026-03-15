#include "clspc/jdtls.h"      
#include "clspc/session.h"    
#include "clspc/inspect.h"    
#include "integ_test_helper.h"

#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <optional>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

namespace fs = std::filesystem;
using namespace clspc;

namespace {


std::optional<DocumentSymbol> find_method_recursive(const std::vector<DocumentSymbol> &symbols,
                                                    std::string_view method_name) 
{
    for (const auto &sym : symbols) {
        if (sym.kind == SymbolKind::Method &&
            logical_name(sym.name) == method_name) {
            return sym;
        }

        if (auto child = find_method_recursive(sym.children, method_name)) {
            return child;
        }
    }

    return std::nullopt;
}


std::optional<DocumentSymbol> find_class_recursive(const std::vector<DocumentSymbol> &symbols,
                                                   std::string_view class_name) 
{
    for (const auto &sym : symbols) {
        if (sym.kind == SymbolKind::Class &&
            logical_name(sym.name) == class_name) {
            return sym;
        }

        if (auto child = find_class_recursive(sym.children, class_name)) {
            return child;
        }
    }

    return std::nullopt;
}

}  // namespace


int main() 
{
    using namespace std::chrono_literals;

    const fs::path root =
        fs::temp_directory_path() / "clspc-test-session-document-symbols-real";

    std::error_code ec;
    fs::remove_all(root, ec);

    const fs::path workspace = root / "workspace";
    const fs::path repo = root / "repo";
    const fs::path src_dir = repo / "src/main/java/com/acme/playground";
    fs::create_directories(workspace, ec);
    require(!ec, "failed to create workspace dir");
    fs::create_directories(src_dir, ec);
    require(!ec, "failed to create source dir");

    // Minimal Maven project so JDTLS imports it cleanly.
    write_file(repo / "pom.xml", R"(<?xml version="1.0" encoding="UTF-8"?>
<project xmlns="http://maven.apache.org/POM/4.0.0"
         xmlns:xsi="http://www.w3.org/2001/XMLSchema-instance"
         xsi:schemaLocation="http://maven.apache.org/POM/4.0.0
                             https://maven.apache.org/xsd/maven-4.0.0.xsd">
  <modelVersion>4.0.0</modelVersion>
  <groupId>com.acme</groupId>
  <artifactId>mini-playground</artifactId>
  <version>1.0-SNAPSHOT</version>
  <properties>
    <maven.compiler.source>17</maven.compiler.source>
    <maven.compiler.target>17</maven.compiler.target>
  </properties>
</project>
)");

    const fs::path java_file = src_dir / "CheckoutService.java";
    write_file(java_file, R"(package com.acme.playground;

public final class CheckoutService {
    private final int count;

    public CheckoutService(int count) {
        this.count = count;
    }

    public int finalizeCheckout() {
        return count;
    }
}
)");

    // w/a to add timeout before java, which doesn't work with exec sys call
    std::string effective_java_bin = real_java_bin();
    if (auto timeout_bin = timeout_bin_from_env(); timeout_bin.has_value()) {
        const fs::path wrapper = root / "java-timebox.sh";
        {
            std::ofstream out(wrapper);
            require(static_cast<bool>(out), "failed to create wrapper script");

           out
                << "#!/usr/bin/env bash\n"
                << "set -euo pipefail\n"
                << "exec timeout 4s "
                << shell_quote_single(real_java_bin())
                << " \"$@\"\n";
            out.close();

            fs::permissions(wrapper,
                            fs::perms::owner_exec | fs::perms::owner_read | fs::perms::owner_write |
                            fs::perms::group_exec | fs::perms::group_read |
                            fs::perms::others_exec | fs::perms::others_read,
                            fs::perm_options::replace);
        }
        effective_java_bin = wrapper.string();
    }


    jdtls::LaunchOptions launch;
    launch.jdtls_home = fs::path(real_jdtls_home());
    launch.workspace_dir = workspace;
    launch.root_dir = repo;
    launch.java_bin = effective_java_bin;
    launch.log_protocol = false;
    launch.log_level = "INFO";

    auto child = jdtls::spawn(launch, jdtls::Platform::Linux);

    SessionOptions options;
    options.root_dir = repo;
    options.client_name = "clspc-test";
    options.client_version = "0.1";

    Session session(std::move(child), options);

    const InitializeResult init = session.initialize();
    require(!init.server_name.empty(), "server_name should not be empty");
    require(init.has_document_symbol_provider,
            "expected real JDTLS to advertise documentSymbolProvider");

    session.initialized();

    // JDTLS may need a little time after initialized/project import.
    std::vector<DocumentSymbol> symbols;
    bool found_class = false;
    bool found_method = false;

    const auto deadline = std::chrono::steady_clock::now() + 20s;
    while (std::chrono::steady_clock::now() < deadline) {
        try {
            symbols = session.document_symbols(java_file);

            found_class = find_class_recursive(symbols, "CheckoutService").has_value();
            found_method = find_method_recursive(symbols, "finalizeCheckout").has_value();

            if (found_class && found_method) {
                break;
            }
        } catch (...) {
            // best effort during startup/import window
        }

        std::this_thread::sleep_for(250ms);
    }

    require(!symbols.empty(), "expected non-empty document symbols from real JDTLS");
    require(found_class, "expected class symbol CheckoutService");
    require(found_method, "expected method symbol finalizeCheckout");

    session.shutdown_and_exit();
    session.wait();

    fs::remove_all(root, ec);

    std::cout << "test_session_document_symbols_real passed\n";
    return 0;
}
