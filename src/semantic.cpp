#include "clspc/semantic.h"
#include "clspc/inspect.h"

#include <chrono>
#include <stdexcept>
#include <thread>
#include <unordered_set>

namespace clspc {


static void emit_trace(const ExpandOptions &options, ExpandTraceEvent event) 
{
    if (options.trace) {
        options.trace(event);
    }
}


static bool is_under_root(const std::filesystem::path &path,
                          const std::filesystem::path &root) 
{
    if (root.empty()) {
        return true;
    }

    std::error_code ec1;
    std::error_code ec2;
    const std::filesystem::path abs_path = std::filesystem::weakly_canonical(path, ec1);
    const std::filesystem::path abs_root = std::filesystem::weakly_canonical(root, ec2);

    const auto &lhs = ec1 ? path : abs_path;
    const auto &rhs = ec2 ? root : abs_root;

    auto it_root = rhs.begin();
    auto it_path = lhs.begin();

    for (; it_root != rhs.end() && it_path != lhs.end(); ++it_root, ++it_path) {
        if (*it_root != *it_path) {
            return false;
        }
    }

    return it_root == rhs.end();
}


static std::string node_key(const CallHierarchyItem &item) 
{
    return item.uri + "|" +
           item.name + "|" +
           std::to_string(item.selection_range.start.line) + ":" +
           std::to_string(item.selection_range.start.character);
}


static std::string snippet_key(const ExpandedNode &node) 
{
    return node.item.path.generic_string() + "|" +
           std::to_string(node.item.range.start.line) + ":" +
           std::to_string(node.item.range.start.character) + "|" +
           std::to_string(node.item.range.end.line) + ":" +
           std::to_string(node.item.range.end.character);
}


std::optional<SourceWindow> make_snippet_for_item(const CallHierarchyItem &item,
                                                  const ExpandOptions &options) 
{
    if (item.path.empty()) {
        return std::nullopt;
    }
    if (!is_under_root(item.path, options.scope_root)) {
        return std::nullopt;
    }
    return extract_source_window(item.path,
                                 item.range,
                                 options.snippet_padding_before,
                                 options.snippet_padding_after);
}


std::optional<DocumentSymbol> find_method_symbol(const std::vector<DocumentSymbol> &symbols,
                                                 std::string_view method_name) 
{
    for (const auto &sym : symbols) {
        if (sym.kind == SymbolKind::Method &&
            logical_name(sym.name) == method_name) {
            return sym;
        }

        if (auto child = find_method_symbol(sym.children, method_name)) {
            return child;
        }
    }

    return std::nullopt;
}


static AnchorResolution resolve_anchor(Session &session,
                                       const std::filesystem::path &file,
                                       std::string_view method_name,
                                       const ExpandOptions &options) 
{
    AnchorResolution result;

    std::vector<DocumentSymbol> symbols;
    std::optional<DocumentSymbol> method;
    std::vector<CallHierarchyItem> items;
    const CallHierarchyItem *anchor = nullptr;

    const auto anchor_file = std::filesystem::absolute(file).lexically_normal();
    const auto deadline = std::chrono::steady_clock::now() + options.ready_timeout;

    while (std::chrono::steady_clock::now() < deadline) {
        ++result.attempts;
        emit_trace(options, ExpandTraceEvent{
            .kind = ExpandTraceKind::AnchorResolveAttempt,
            .attempt = result.attempts,
            .message = "resolving anchor",
        });
        try {
            symbols = session.document_symbols(anchor_file);
            // find the range
            method = find_method_symbol(symbols, method_name);
            if (method.has_value()) {
                emit_trace(options, ExpandTraceEvent{
                    .kind = ExpandTraceKind::AnchorSymbolFound,
                    .attempt = result.attempts,
                    .message = "anchor symbol found via documentSymbol",
                });
                items = session.prepare_call_hierarchy(anchor_file,
                                                       method->selection_range.start);
                for (const auto &item : items) {
                    if (logical_name(item.name) == method_name) {
                        anchor = &item;
                        break;
                    }
                }
                if (anchor != nullptr) {
                    break;
                }
            }
        } catch (...) {
            // best effort during server/project warmup
        }
        std::this_thread::sleep_for(options.retry_interval);
    }
    if (!method.has_value()) {
        throw std::runtime_error("failed to resolve anchor method via documentSymbol: " +
                                 std::string(method_name));
    }
    if (anchor == nullptr) {
        throw std::runtime_error("failed to resolve anchor call-hierarchy item: " +
                                 std::string(method_name));
    }
    result.symbol = *method;
    result.item = *anchor;
    return result;
}


static ExpandedNode expand_outgoing_node(Session &session,
                                         const CallHierarchyItem &item,
                                         const ExpandOptions &options,
                                         int depth,
                                         std::unordered_set<std::string> &visited) 
{
    ExpandedNode node;
    node.item = item;
    emit_trace(options, ExpandTraceEvent{
        .kind = ExpandTraceKind::EnterNode,
        .depth = depth,
        .item = item,
        .message = "enter outgoing node",
    });
    node.snippet = make_snippet_for_item(item, options);

    const std::string key = node_key(item);
    if (!visited.insert(key).second) {
        node.stop_reason = "already-visited";
        emit_trace(options, ExpandTraceEvent{
            .kind = ExpandTraceKind::StopNode,
            .depth = depth,
            .item = item,
            .message = "stop outgoing node",
            .stop_reason = node.stop_reason,
        });
        return node;
    }
    if (depth >= options.max_depth) {
        node.stop_reason = "max-depth";
        return node;
    }
    if (item.path.empty()) {
        node.stop_reason = "no-path";
        return node;
    }
    if (!is_under_root(item.path, options.scope_root)) {
        node.stop_reason = "external-or-library";
        return node;
    }
    const std::vector<OutgoingCall> outgoing = session.outgoing_calls(item);
    emit_trace(options, ExpandTraceEvent{
        .kind = ExpandTraceKind::ExpandOutgoing,
        .depth = depth,
        .item = item,
        .edge_count = outgoing.size(),
        .message = "fetched outgoing edges",
    });
    if (outgoing.empty()) {
        node.stop_reason = "leaf";
        return node;
    }
    for (const auto &call : outgoing) {
        ExpandedNode child = expand_outgoing_node(session,
                                                  call.to,
                                                  options,
                                                  depth + 1,
                                                  visited);
        child.from_ranges = call.from_ranges;
        node.children.push_back(std::move(child));
    }
    return node;
}


static ExpandedNode expand_incoming_node(
        Session &session,
        const CallHierarchyItem &item,
        const ExpandOptions &options,
        int depth,
        std::unordered_set<std::string> &visited,
        const std::optional<std::vector<IncomingCall>> &prefetched_incoming = std::nullopt) 
{
    ExpandedNode node;
    node.item = item;
    node.snippet = make_snippet_for_item(item, options);

    const std::string key = node_key(item);
    if (!visited.insert(key).second) {
        node.stop_reason = "already-visited";
        return node;
    }

    if (depth >= options.max_depth) {
        node.stop_reason = "max-depth";
        return node;
    }

    if (item.path.empty()) {
        node.stop_reason = "no-path";
        return node;
    }

    if (!is_under_root(item.path, options.scope_root)) {
        node.stop_reason = "external-or-library";
        return node;
    }

    const std::vector<IncomingCall> incoming =
        prefetched_incoming.has_value() ? *prefetched_incoming : session.incoming_calls(item);

    if (incoming.empty()) {
        node.stop_reason = "leaf";
        return node;
    }

    for (const auto &call : incoming) {
        ExpandedNode child = expand_incoming_node(session,
                                                  call.from,
                                                  options,
                                                  depth + 1,
                                                  visited);
        child.from_ranges = call.from_ranges;
        node.children.push_back(std::move(child));
    }

    return node;
}


// dedupe: call hierarchy could have overlapped nodes
static void collect_unique_snippets_recursive(const ExpandedNode &node,
                                              std::unordered_set<std::string> &seen,
                                              std::vector<ExpandedSnippet> &out) 
{
    if (node.snippet.has_value()) {
        const std::string key = snippet_key(node);
        if (seen.insert(key).second) {
            out.push_back(ExpandedSnippet{
                .item = node.item,
                .stop_reason = node.stop_reason,
                .window = *node.snippet,
            });
        }
    }
    for (const auto &child : node.children) {
        collect_unique_snippets_recursive(child, seen, out);
    }
}


ExpansionResult expand_outgoing_from_method(Session &session,
                                            const std::filesystem::path &file,
                                            std::string_view method_name,
                                            const ExpandOptions &options) {
    ExpansionResult result;
    result.anchor_file = std::filesystem::absolute(file).lexically_normal();
    result.anchor_method = std::string(method_name);

    const AnchorResolution anchor =
        resolve_anchor(session, result.anchor_file, method_name, options);
    result.anchor_symbol = anchor.symbol;
    result.anchor_item = anchor.item;
    result.attempts = anchor.attempts;
    std::unordered_set<std::string> visited;
    result.root = expand_outgoing_node(session,
                                       result.anchor_item,
                                       options,
                                       0,
                                       visited);
    return result;
}


static std::vector<IncomingCall> wait_for_initial_incoming(Session &session,
                                                    const CallHierarchyItem &item,
                                                    const ExpandOptions &options,
                                                    std::size_t &attempts_out) 
{
    const auto deadline = std::chrono::steady_clock::now() + options.ready_timeout;
    while (std::chrono::steady_clock::now() < deadline) {
        emit_trace(options, ExpandTraceEvent{
            .kind = ExpandTraceKind::RootEdgeRetryAttempt,
            .attempt = attempts_out,
            .message = "probing initial incoming edges",
        });
        ++attempts_out;
        try {
            const std::vector<IncomingCall> incoming = session.incoming_calls(item);
            emit_trace(options, ExpandTraceEvent{
                .kind = ExpandTraceKind::RootEdgeRetryResult,
                .attempt = attempts_out,
                .edge_count = incoming.size(),
                .message = "initial incoming edge probe result",
            });
            if (!incoming.empty()) {
                return incoming;
            }
        } catch (...) {
        }
        std::this_thread::sleep_for(options.retry_interval);
    }
    return {};
}


ExpansionResult expand_incoming_to_method(Session &session,
                                          const std::filesystem::path &file,
                                          std::string_view method_name,
                                          const ExpandOptions &options) 
{
    ExpansionResult result;
    result.anchor_file = std::filesystem::absolute(file).lexically_normal();
    result.anchor_method = std::string(method_name);
    const AnchorResolution anchor =
        resolve_anchor(session, result.anchor_file, method_name, options);
    result.anchor_symbol = anchor.symbol;
    result.anchor_item = anchor.item;
    result.attempts = anchor.attempts;

    // only retry the inital edges to ensure lsp readiness
    // lower layers are traversed normally
    const std::vector<IncomingCall> initial_incoming =
        wait_for_initial_incoming(session,
                                  result.anchor_item,
                                  options,
                                  result.initial_edge_probe_attempts);

    result.initial_edge_count = initial_incoming.size();

    std::unordered_set<std::string> visited;
    result.root = expand_incoming_node(session,
                                       result.anchor_item,
                                       options,
                                       0,
                                       visited);
    return result;
}


std::vector<ExpandedSnippet> collect_unique_snippets(const ExpandedNode &root) 
{
    std::vector<ExpandedSnippet> out;
    std::unordered_set<std::string> seen;
    collect_unique_snippets_recursive(root, seen, out);
    return out;
}

}


