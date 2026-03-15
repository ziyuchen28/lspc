#include "clspc/jdtls.h"
#include "clspc/inspect.h"
#include "clspc/session.h"
#include "integ_test_helper.h"

#include <chrono>
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


bool same_path(const std::filesystem::path &lhs,
               const std::filesystem::path &rhs) 
{
    namespace fs = std::filesystem;
    std::error_code ec1;
    std::error_code ec2;

    const fs::path a = fs::weakly_canonical(lhs, ec1);
    const fs::path b = fs::weakly_canonical(rhs, ec2);

    if (!ec1 && !ec2) {
        return a == b;
    }

    return lhs.lexically_normal() == rhs.lexically_normal();
}


std::optional<DocumentSymbol> find_method_recursive(const std::vector<DocumentSymbol> &symbols,
                                                    std::string_view method_name) {
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


const CallHierarchyItem *find_call_item(const std::vector<CallHierarchyItem> &items,
                                        std::string_view method_name) 
{
    for (const auto &item : items) {
        if (logical_name(item.name) == method_name) {
            return &item;
        }
    }
    return nullptr;
}


const OutgoingCall *find_outgoing_call(const std::vector<OutgoingCall> &calls,
                                       const fs::path &expected_file,
                                       std::string_view expected_method) 
{
    for (const auto &call : calls) {
        std::cout << "to path: " << call.to.path << std::endl;
        std::cout << "expected: " << expected_file << std::endl;
        if (same_path(call.to.path, expected_file) &&
            logical_name(call.to.name) == expected_method) {
            return &call;
        }
    }
    return nullptr;
}


void print_section(const std::string &title) 
{
    std::cout << "\n=== " << title << " ===\n";
}


}  // namespace



int main() 
{
    using namespace std::chrono_literals;

    const fs::path root =
        fs::temp_directory_path() / "clspc-test-session-call-hierarchy-real";

    std::error_code ec;
    fs::remove_all(root, ec);

    const fs::path workspace = root / "workspace";
    const fs::path repo = root / "repo";
    const fs::path src_dir = repo / "src/main/java/com/acme/playground";
    fs::create_directories(workspace, ec);
    require(!ec, "failed to create workspace dir");
    fs::create_directories(src_dir, ec);
    require(!ec, "failed to create source dir");

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

    const fs::path pricing_file = src_dir / "PricingEngine.java";
    write_file(pricing_file, R"(package com.acme.playground;

public final class PricingEngine {
    public int quoteTotal() {
        return 42;
    }
}
)");

    const fs::path checkout_file = src_dir / "CheckoutService.java";
    write_file(checkout_file, R"(package com.acme.playground;

public final class CheckoutService {
    private final PricingEngine pricingEngine = new PricingEngine();

    public int finalizeCheckout() {
        return pricingEngine.quoteTotal();
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

    auto child = jdtls::spawn(launch, jdtls::current_platform());

    SessionOptions options;
    options.root_dir = repo;
    options.client_name = "clspc-test";
    options.client_version = "0.1";

    Session session(std::move(child), options);


    print_section("initialize");
    const InitializeResult init = session.initialize();
    print_initialize_result(std::cout, init);

    require(init.has_document_symbol_provider,
            "expected documentSymbolProvider");
    require(init.has_call_hierarchy_provider,
            "expected callHierarchyProvider");

    session.initialized();

    std::vector<DocumentSymbol> symbols;
    std::optional<DocumentSymbol> method;
    std::vector<CallHierarchyItem> items;
    std::vector<OutgoingCall> outgoing;

    std::size_t attempts = 0;
    const auto deadline = std::chrono::steady_clock::now() + 20s;
    while (std::chrono::steady_clock::now() < deadline) {
        ++attempts;
        try {
            symbols = session.document_symbols(checkout_file);
            method = find_method_recursive(symbols, "finalizeCheckout");

            if (method.has_value()) {
                items = session.prepare_call_hierarchy(
                    checkout_file,
                    method->selection_range.start);

                const CallHierarchyItem *item =
                    find_call_item(items, "finalizeCheckout");

                if (item != nullptr) {
                    outgoing = session.outgoing_calls(*item);

                    const OutgoingCall *quote_total =
                        find_outgoing_call(outgoing,
                                           fs::absolute(pricing_file).lexically_normal(),
                                           "quoteTotal");

                    if (quote_total != nullptr) {
                        break;
                    }
                }
            }
        } catch (...) {
            // best effort during startup/import window
        }

        std::this_thread::sleep_for(250ms);
    }


    // print symbols
    print_section("document symbols");
    print_document_symbols(std::cout, symbols);


    // print methods 
    require(method.has_value(), "expected method finalizeCheckout");
    print_section("anchor method");
    std::cout << "name=" << method->name
              << " logical=" << logical_name(method->name)
              << " range=" << format_range(method->range)
              << " selection=" << format_range(method->selection_range)
              << "\n";

    // print call hierarchy items
    require(!items.empty(), "expected non-empty prepareCallHierarchy result");
    print_section("prepareCallHierarchy result");
    print_call_hierarchy_items(std::cout, items);
    const CallHierarchyItem *anchor =
        find_call_item(items, "finalizeCheckout");
    require(anchor != nullptr,
            "expected call hierarchy item for finalizeCheckout");


    // print out going calls
    print_section("outgoing calls");
    print_outgoing_calls(std::cout, outgoing);
    const OutgoingCall *quote_total =
        find_outgoing_call(outgoing,
                           fs::absolute(pricing_file).lexically_normal(),
                           "quoteTotal");
    require(quote_total != nullptr,
            "expected outgoing call to PricingEngine.quoteTotal");

    session.shutdown_and_exit();
    session.wait();

    fs::remove_all(root, ec);

    std::cout << "test_session_call_hierarchy_real passed\n";
    return 0;
}
