#include "clspc/inspect.h"
#include "clspc/jdtls.h"
#include "clspc/session.h"
#include "clspc/source_window.h"

#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <optional>
#include <string>
#include <string_view>
#include <thread>
#include <unordered_set>
#include <vector>

namespace fs = std::filesystem;
using namespace clspc;

namespace {

struct Args {
    fs::path jdtls_home;
    fs::path root;
    fs::path workspace;
    fs::path file;
    std::string method;
    std::string java_bin{"java"};
    int max_depth{3};
    bool show_help{false};
};

struct GraphNode {
    CallHierarchyItem item;
    std::vector<Range> from_ranges;
    std::string stop_reason;
    std::vector<GraphNode> children;
};

[[noreturn]] void fail(const std::string &msg) {
    throw std::runtime_error(msg);
}

void require(bool condition, const std::string &message) {
    if (!condition) {
        fail(message);
    }
}

std::string next_arg(int &i, int argc, char **argv, const std::string &flag) {
    if (i + 1 >= argc) {
        fail("missing value after " + flag);
    }
    ++i;
    return argv[i];
}

Args parse_args(int argc, char **argv) {
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

void print_help() {
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
        << "  --max-depth N   (default: 3)\n\n"
        << "Example:\n"
        << "  dep_expand_demo \\\n"
        << "    --jdtls-home /home/user/jdtls \\\n"
        << "    --root /repo/mini-java-playground \\\n"
        << "    --workspace /tmp/clspc-demo-workspace \\\n"
        << "    --file /repo/mini-java-playground/src/main/java/com/acme/playground/CheckoutService.java \\\n"
        << "    --method finalizeCheckout \\\n"
        << "    --max-depth 3\n";
}

std::string shell_quote_single(std::string_view s) {
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

void write_executable_script(const fs::path &path, const std::string &contents) {
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


bool is_under_root(const fs::path &path, const fs::path &root) 
{
    // fs::canonical crash if file doesn't exists
    const fs::path abs_path = fs::weakly_canonical(path);
    const fs::path abs_root = fs::weakly_canonical(root);

    auto it_root = abs_root.begin();
    auto it_path = abs_path.begin();

    for (; it_root != abs_root.end() && it_path != abs_path.end(); ++it_root, ++it_path) {
        if (*it_root != *it_path) {
            return false;
        }
    }

    return it_root == abs_root.end();
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
                                        std::string_view method_name) {
    for (const auto &item : items) {
        if (logical_name(item.name) == method_name) {
            return &item;
        }
    }
    return nullptr;
}

std::string node_key(const CallHierarchyItem &item) {
    return item.uri + "|" +
           item.name + "|" +
           std::to_string(item.selection_range.start.line) + ":" +
           std::to_string(item.selection_range.start.character);
}

GraphNode expand_outgoing(Session &session,
                          const CallHierarchyItem &item,
                          const fs::path &repo_root,
                          int depth,
                          int max_depth,
                          std::unordered_set<std::string> &visited) {
    GraphNode node;
    node.item = item;

    const std::string key = node_key(item);
    if (!visited.insert(key).second) {
        node.stop_reason = "already-visited";
        return node;
    }

    if (depth >= max_depth) {
        node.stop_reason = "max-depth";
        return node;
    }

    if (item.path.empty()) {
        node.stop_reason = "no-path";
        return node;
    }

    if (!is_under_root(item.path, repo_root)) {
        node.stop_reason = "external-or-library";
        return node;
    }

    const std::vector<OutgoingCall> outgoing = session.outgoing_calls(item);
    if (outgoing.empty()) {
        node.stop_reason = "leaf";
        return node;
    }

    for (const auto &call : outgoing) {
        GraphNode child = expand_outgoing(session,
                                          call.to,
                                          repo_root,
                                          depth + 1,
                                          max_depth,
                                          visited);
        child.from_ranges = call.from_ranges;
        node.children.push_back(std::move(child));
    }

    return node;
}


void print_graph_node(const GraphNode &node, int depth = 0) 
{
    const std::string indent(static_cast<std::size_t>(depth * 2), ' ');
    std::cout << indent
              << "- "
              << "depth="
              << depth
              << "  "
              << "path="
              << node.item.path
              << std::endl;
    std::cout << indent
              << "- " << node.item.name
              << "  logical=" << logical_name(node.item.name)
              << "  kind=" << symbol_kind_name(node.item.kind)
              << "  file=" << (node.item.path.empty() ? "<none>" : node.item.path.filename().string())
              << "  range=" << format_range(node.item.range);

    if (!node.stop_reason.empty()) {
        std::cout << "  stop=" << node.stop_reason;
    }

    std::cout << "\n";

    if (!node.from_ranges.empty()) {
        for (const auto &range : node.from_ranges) {
            std::cout << indent << "  from=" << format_range(range) << "\n";
        }
    }

    for (const auto &child : node.children) {
        print_graph_node(child, depth + 1);
    }
}


std::string snippet_key(const GraphNode &node) 
{
    return node.item.path.generic_string() + "|" +
           std::to_string(node.item.range.start.line) + ":" +
           std::to_string(node.item.range.start.character) + "|" +
           std::to_string(node.item.range.end.line) + ":" +
           std::to_string(node.item.range.end.character);
}


void print_graph_snippets(const GraphNode &node,
                          const fs::path &repo_root,
                          std::unordered_set<std::string> &seen,
                          std::size_t context_before,
                          std::size_t context_after) 
{
    if (!node.item.path.empty() && is_under_root(node.item.path, repo_root)) {
        const std::string key = snippet_key(node);
        if (seen.insert(key).second) {
            const SourceWindow window =
                extract_source_window(node.item.path,
                                      node.item.range,
                                      context_before,
                                      context_after);

            std::cout << "---- "
                      << node.item.path.filename().string()
                      << " :: "
                      << node.item.name
                      << "  stop="
                      << (node.stop_reason.empty() ? "<none>" : node.stop_reason)
                      << "  [" << window.start_line << "-" << window.end_line << "]\n";
            std::cout << window.text << "\n\n";
        }
    }

    for (const auto &child : node.children) {
        print_graph_snippets(child,
                             repo_root,
                             seen,
                             context_before,
                             context_after);
    }
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


}  // namespace


int main(int argc, char **argv) 
{
    using namespace std::chrono_literals;

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

        print_section("launch mode");
        std::cout << "java_bin=" << args.java_bin << "\n";
        if (effective_java_bin == args.java_bin) {
            std::cout << "timeout_wrapper=disabled\n";
        } else {
            std::cout << "timeout_wrapper=enabled\n";
            std::cout << "effective_java_bin=" << effective_java_bin << "\n";
        }

        std::vector<DocumentSymbol> symbols;
        std::optional<DocumentSymbol> method;
        std::vector<CallHierarchyItem> items;
        const CallHierarchyItem *anchor = nullptr;

        std::size_t attempts = 0;
        const auto deadline = std::chrono::steady_clock::now() + 20s;
        while (std::chrono::steady_clock::now() < deadline) {
            ++attempts;

            try {
                symbols = session.document_symbols(args.file);
                method = find_method_recursive(symbols, args.method);

                if (method.has_value()) {
                    items = session.prepare_call_hierarchy(
                        args.file,
                        method->selection_range.start);

                    anchor = find_call_item(items, args.method);
                    if (anchor != nullptr) {
                        break;
                    }
                }
            } catch (...) {
                // best effort during startup/import window
            }

            std::this_thread::sleep_for(250ms);
        }

        print_section("retry");
        std::cout << "attempts=" << attempts << "\n";

        print_section("document symbols");
        print_document_symbols(std::cout, symbols);

        require(method.has_value(), "failed to find anchor method via documentSymbol");
        require(anchor != nullptr, "failed to find anchor call-hierarchy item");

        print_section("anchor method");
        std::cout << "name=" << method->name
                  << " logical=" << logical_name(method->name)
                  << " range=" << format_range(method->range)
                  << " selection=" << format_range(method->selection_range)
                  << "\n";

        print_section("prepareCallHierarchy result");
        print_call_hierarchy_items(std::cout, items);

        std::unordered_set<std::string> visited;
        GraphNode graph = expand_outgoing(session,
                                          *anchor,
                                          args.root,
                                          0,
                                          args.max_depth,
                                          visited);

        print_section("expanded dependency tree");
        print_graph_node(graph);

        print_section("fetched code snippets");
        std::unordered_set<std::string> seen_snippets;
        print_graph_snippets(graph,
                             args.root,
                             seen_snippets,
                             1,   // context_before
                             1);  // context_after

        session.shutdown_and_exit();
        session.wait();

        return 0;
    } catch (const std::exception &ex) {
        std::cerr << "dep_expand_demo error: " << ex.what() << "\n\n";
        print_help();
        return 1;
    }
}
