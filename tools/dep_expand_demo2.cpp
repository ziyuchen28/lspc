#include "clspc/inspect.h"
#include "clspc/jdtls.h"
#include "clspc/semantic.h"
#include "clspc/session.h"

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace fs = std::filesystem;
using namespace clspc;

namespace {


enum class Direction 
{
    Outgoing,
    Incoming,
    Both,
};


struct Args 
{
    fs::path jdtls_home;
    fs::path root;
    fs::path workspace;
    fs::path file;
    std::string method;
    std::string java_bin{"java"};
    int max_depth{3};
    Direction direction{Direction::Outgoing};
    bool show_help{false};
};


[[noreturn]] void fail(const std::string &msg) 
{
    throw std::runtime_error(msg);
}

void require(bool condition, const std::string &message) 
{
    if (!condition) {
        fail(message);
    }
}

std::string next_arg(int &i, int argc, char **argv, const std::string &flag) 
{
    if (i + 1 >= argc) {
        fail("missing value after " + flag);
    }
    ++i;
    return argv[i];
}

Direction parse_direction(const std::string &value) 
{
    if (value == "outgoing") {
        return Direction::Outgoing;
    }
    if (value == "incoming") {
        return Direction::Incoming;
    }
    if (value == "both") {
        return Direction::Both;
    }
    fail("unknown direction: " + value);
}

Args parse_args(int argc, char **argv) 
{
    Args args;

    for (int i = 1; i < argc; ++i) {
        const std::string arg = argv[i];

        if (arg == "--help" || arg == "-h") {
            args.show_help = true;
            return args;
        } else if (arg == "--jdtls-home") {
            args.jdtls_home = fs::path(next_arg(i, argc, argv, arg));
        } else if (arg == "--root") {
            args.root = fs::path(next_arg(i, argc, argv, arg));
        } else if (arg == "--workspace") {
            args.workspace = fs::path(next_arg(i, argc, argv, arg));
        } else if (arg == "--file") {
            args.file = fs::path(next_arg(i, argc, argv, arg));
        } else if (arg == "--method") {
            args.method = next_arg(i, argc, argv, arg);
        } else if (arg == "--java") {
            args.java_bin = next_arg(i, argc, argv, arg);
        } else if (arg == "--max-depth") {
            args.max_depth = std::stoi(next_arg(i, argc, argv, arg));
        } else if (arg == "--direction") {
            args.direction = parse_direction(next_arg(i, argc, argv, arg));
        } else {
            fail("unknown argument: " + arg);
        }
    }

    if (args.jdtls_home.empty() ||
        args.root.empty() ||
        args.workspace.empty() ||
        args.file.empty() ||
        args.method.empty()) {
        fail("missing required args: --jdtls-home --root --workspace --file --method");
    }

    return args;
}

void print_help() 
{
    std::cout
        << "dep_expand_demo\n\n"
        << "Required:\n"
        << "  --jdtls-home PATH\n"
        << "  --root PATH\n"
        << "  --workspace PATH\n"
        << "  --file PATH\n"
        << "  --method NAME\n\n"
        << "Optional:\n"
        << "  --java PATH\n"
        << "  --max-depth N      (default: 3)\n"
        << "  --direction MODE   outgoing|incoming|both (default: outgoing)\n";
}

std::optional<std::string> timeout_bin_from_env() 
{
    if (const char *env = std::getenv("CLSPC_TIMEOUT_BIN")) {
        if (*env != '\0') {
            return std::string(env);
        }
    }
    return std::nullopt;
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


}  // namespace

int main(int argc, char **argv) 
{
    try {
        const Args args = parse_args(argc, argv);
        if (args.show_help) {
            print_help();
            return 0;
        }

        fs::create_directories(args.workspace);

        std::string effective_java_bin = args.java_bin;

        if (auto timeout_bin = timeout_bin_from_env(); timeout_bin.has_value()) {
            const fs::path wrapper = args.workspace / "java-timebox.sh";
            {
                std::ofstream out(wrapper);
                require(static_cast<bool>(out), "failed to create wrapper script");
                out
                    << "#!/usr/bin/env bash\n"
                    << "set -euo pipefail\n"
                    << "exec "
                    << shell_quote_single(*timeout_bin)
                    << " 20s "
                    << shell_quote_single(args.java_bin)
                    << " \"$@\"\n";
            }

            fs::permissions(wrapper,
                            fs::perms::owner_exec | fs::perms::owner_read | fs::perms::owner_write |
                            fs::perms::group_exec | fs::perms::group_read |
                            fs::perms::others_exec | fs::perms::others_read,
                            fs::perm_options::replace);

            effective_java_bin = wrapper.string();
        }

        jdtls::LaunchOptions launch;
        launch.jdtls_home = args.jdtls_home;
        launch.workspace_dir = args.workspace;
        launch.root_dir = args.root;
        launch.java_bin = effective_java_bin;
        launch.log_protocol = false;
        launch.log_level = "INFO";

        auto child = jdtls::spawn(launch, jdtls::current_platform());

        SessionOptions options;
        options.root_dir = args.root;
        options.client_name = "clspc-demo";
        options.client_version = "0.1";

        Session session(std::move(child), options);

        print_section("initialize");
        const InitializeResult init = session.initialize();
        print_initialize_result(std::cout, init);
        session.initialized();

        print_section("document symbols");
        const std::vector<DocumentSymbol> symbols =
            session.document_symbols(args.file);
        print_document_symbols(std::cout, symbols);

        ExpandOptions expand_options;
        expand_options.scope_root = args.root;
        expand_options.max_depth = args.max_depth;
        expand_options.snippet_padding_before = 1;
        expand_options.snippet_padding_after = 1;

        if (args.direction == Direction::Outgoing ||
            args.direction == Direction::Both) {
            const ExpansionResult outgoing =
                expand_outgoing_from_method(session,
                                            args.file,
                                            args.method,
                                            expand_options);
            print_expansion_result("outgoing", outgoing);
        }

        if (args.direction == Direction::Incoming ||
            args.direction == Direction::Both) {
            const ExpansionResult incoming =
                expand_incoming_to_method(session,
                                          args.file,
                                          args.method,
                                          expand_options);
            print_expansion_result("incoming", incoming);
        }

        session.shutdown_and_exit();
        session.wait();

        return 0;
    } catch (const std::exception &ex) {
        std::cerr << "dep_expand_demo error: " << ex.what() << "\n\n";
        print_help();
        return 1;
    }
}
