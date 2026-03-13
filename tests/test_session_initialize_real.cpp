#include "clspc/jdtls.h"
#include "clspc/session.h"

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>
#include <string_view>

namespace fs = std::filesystem;
using namespace clspc;



namespace {

void require(bool condition, const std::string &message) 
{
    if (!condition) {
        std::cerr << "FAIL: " << message << "\n";
        std::exit(1);
    }
}

std::string real_jdtls_home() 
{
    if (const char* env = std::getenv("CLSPC_TEST_JDTLS_HOME")) {
        return env;
    }
    require(false, "set CLSPC_TEST_JDTLS_HOME to the extracted JDTLS directory");
    return {};
}

std::string real_java_bin() 
{
    if (const char *env = std::getenv("CLSPC_TEST_JAVA_BIN")) {
        return env;
    }
    return "java";
}

std::string shell_quote_single(std::string_view s) 
{
    std::string out;
    out.push_back('\'');
    for (char ch : s) {
        if (ch == '\'') {
            out += "'\\''";
        } else {
            out.push_back(ch);
        }
    }
    out.push_back('\'');
    return out;
}

void write_executable_script(const fs::path &path, const std::string &contents) 
{
    std::ofstream out(path);
    require(static_cast<bool>(out), "failed to create script: " + path.string());
    out << contents;
    out.close();

    fs::permissions(path,
                    fs::perms::owner_exec | fs::perms::owner_read | fs::perms::owner_write |
                    fs::perms::group_exec | fs::perms::group_read |
                    fs::perms::others_exec | fs::perms::others_read,
                    fs::perm_options::replace);
}

bool contains(std::string_view haystack, std::string_view needle) 
{
    return haystack.find(needle) != std::string_view::npos;
}

}  // namespace


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

    // We intentionally wrap the real java binary in `timeout` so this test
    // cannot hang forever even if the server lingers during shutdown.
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

    jdtls::LaunchOptions launch;
    launch.jdtls_home = fs::path(real_jdtls_home());
    launch.workspace_dir = workspace;
    launch.root_dir = repo;
    launch.java_bin = wrapper.string();
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



