#include "clspc/jdtls.h"
#include "clspc/session.h"
#include "integ_test_helper.h"

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>
#include <string_view>

namespace fs = std::filesystem;
using namespace clspc;


int main() 
{
    const fs::path root =
        fs::temp_directory_path() / "clspc-test-session-initialize-real";

    std::error_code ec;
    fs::remove_all(root, ec);

    const fs::path workspace = root / "workspace";
    const fs::path repo = root / "repo";
    fs::create_directories(workspace, ec);
    require(!ec, "failed to create workspace dir");
    fs::create_directories(repo, ec);
    require(!ec, "failed to create repo dir");

    // wrap in a time box to avoid lsp jdtls hanging
    std::string effective_java_bin = real_java_bin();
    if (auto timeout_bin = timeout_bin_from_env(); timeout_bin.has_value()) {
        const fs::path wrapper = root / "java-timebox.sh";
        {
            std::ofstream out(wrapper);
            require(static_cast<bool>(out), "failed to create wrapper script");

            out
                << "#!/usr/bin/env bash\n"
                << "set -euo pipefail\n"
                << "exec timeout 10s "
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


    std::cout << "launch mode: " << "\n";
    std::cout << "java_bin=" << real_java_bin() << "\n";
    if (effective_java_bin == real_java_bin()) {
        std::cout << "timeout_wrapper=disabled\n";
    } else {
        std::cout << "timeout_wrapper=enabled\n";
        std::cout << "effective_java_bin=" << effective_java_bin << "\n";
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
    require(contains(init.server_name, "JDT Language Server"),
            "unexpected server_name: " + init.server_name);

    require(init.has_definition_provider,
            "expected real JDTLS to advertise definitionProvider");
    require(init.has_references_provider,
            "expected real JDTLS to advertise referencesProvider");
    require(init.has_hover_provider,
            "expected real JDTLS to advertise hoverProvider");
    require(init.has_document_symbol_provider,
            "expected real JDTLS to advertise documentSymbolProvider");
    require(init.has_call_hierarchy_provider,
            "expected real JDTLS to advertise callHierarchyProvider");

    session.initialized();
    session.shutdown_and_exit();
    session.wait();

    fs::remove_all(root, ec);

    std::cout << "test_session_initialize_real passed\n";
    return 0;
}



