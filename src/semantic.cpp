#include "clspc/semantic.h"
#include "clspc/inspect.h"

#include <chrono>
#include <stdexcept>
#include <thread>
#include <unordered_set>

namespace clspc {
 

static char tolower_ascii(char c)
{
    return (c >= 'A' && c <= 'Z') ? (c |= 32) : c;
}


// case insensitive
static bool iequals_ascii(std::string_view a, std::string_view b)
{
    if (a.size() != b.size()) return false;
    for (std::size_t i = 0; i < a.size(); i++) {
        if (tolower_ascii(a[i]) != tolower_ascii(b[i])) return false;
    }
    return true;
}



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


static std::optional<SourceWindow> make_snippet_for_item(const CallHierarchyItem &item,
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


static bool is_type_like_kind(SymbolKind kind) 
{
    switch (kind) {
        case SymbolKind::Class:
        case SymbolKind::Interface:
        case SymbolKind::Enum:
        case SymbolKind::Struct:
            return true;
        default:
            return false;
    }
}


static std::optional<ResolvedAnchor> try_resolve_method_anchor_in_file_once(
        Session &session,
        const std::filesystem::path &file,
        std::string_view method_name) 
{
    const auto anchor_file = std::filesystem::absolute(file).lexically_normal();

    const std::vector<DocumentSymbol> symbols =
        session.document_symbols(anchor_file);

    const std::optional<DocumentSymbol> method =
        find_method_symbol(symbols, method_name);

    if (!method.has_value()) {
        return std::nullopt;
    }

    const std::vector<CallHierarchyItem> items =
        session.prepare_call_hierarchy(anchor_file,
                                       method->selection_range.start);

    for (const auto &item : items) {
        if (iequals_ascii(logical_name(item.name), method_name)) {
            return ResolvedAnchor{
                .file = anchor_file,
                .class_name = {},
                .method_name = item.name,
                .class_symbol = WorkspaceSymbol{},                
                .method_symbol = *method,
                .call_item = item,
                .attempts = 1,
            };
        }
    }

    return std::nullopt;
}


static ResolvedAnchor resolve_method_anchor_in_file(Session &session,
                                                    const std::filesystem::path &file,
                                                    std::string_view method_name,
                                                    const ExpandOptions &options) 
{
    ResolvedAnchor result;
    result.file = std::filesystem::absolute(file).lexically_normal();
    result.method_name = std::string(method_name);

    const auto deadline = std::chrono::steady_clock::now() + options.ready_timeout;

    while (std::chrono::steady_clock::now() < deadline) {
        ++result.attempts;

        emit_trace(options, ExpandTraceEvent{
            .kind = ExpandTraceKind::AnchorResolveAttempt,
            .attempt = result.attempts,
            .message = "resolving method anchor in file",
        });

        try {
            const auto anchor =
                try_resolve_method_anchor_in_file_once(session,
                                                       result.file,
                                                       method_name);

            if (anchor.has_value()) {
                result.method_symbol = anchor->method_symbol;
                result.call_item = anchor->call_item;

                emit_trace(options, ExpandTraceEvent{
                    .kind = ExpandTraceKind::AnchorSymbolFound,
                    .attempt = result.attempts,
                    .message = "method anchor symbol found via documentSymbol",
                });

                emit_trace(options, ExpandTraceEvent{
                    .kind = ExpandTraceKind::AnchorCallHierarchyReady,
                    .attempt = result.attempts,
                    .item = result.call_item,
                    .edge_count = 1,
                    .message = "prepareCallHierarchy returned anchor item",
                });

                return result;
            }
        } catch (...) {
            // best effort during server/project warmup
        }

        std::this_thread::sleep_for(options.retry_interval);
    }

    throw std::runtime_error("failed to resolve anchor method via documentSymbol: " +
                             std::string(method_name));
}


static std::vector<WorkspaceSymbol> select_anchor_candidates(
        const std::vector<WorkspaceSymbol> &symbols,
        std::string_view class_name,
        const std::filesystem::path &scope_root) 
{
    std::vector<WorkspaceSymbol> out;
    for (const auto &sym : symbols) {
        if (!is_type_like_kind(sym.kind)) {
            continue;
        }
        // if (logical_name(sym.name) != class_name) {
        //     c
        //     ontinue;
        // }
        if (!iequals_ascii(logical_name(sym.name), class_name)) {
            continue;
        }
        if (sym.path.empty()) {
            continue;
        }
        if (!scope_root.empty() && !is_under_root(sym.path, scope_root)) {
            continue;
        }
        out.push_back(sym);
    }
    return out;
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


std::optional<DocumentSymbol> find_method_symbol(const std::vector<DocumentSymbol> &symbols,
                                                 std::string_view method_name) 
{
    for (const auto &sym : symbols) {
        if (sym.kind == SymbolKind::Method &&
            iequals_ascii(logical_name(sym.name), method_name)) {
            return sym;
        }

        if (auto child = find_method_symbol(sym.children, method_name)) {
            return child;
        }
    }

    return std::nullopt;
}


ExpansionResult expand_outgoing_from_method(Session &session,
                                            const std::filesystem::path &file,
                                            std::string_view method_name,
                                            const ExpandOptions &options) {
    ExpansionResult result;
    result.anchor_file = std::filesystem::absolute(file).lexically_normal();
    result.anchor_method = std::string(method_name);

    const ResolvedAnchor anchor =
        resolve_method_anchor_in_file(session, result.anchor_file, method_name, options);
    result.anchor_symbol = anchor.method_symbol;
    result.anchor_item = anchor.call_item;
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

    const ResolvedAnchor anchor =
        resolve_method_anchor_in_file(session, result.anchor_file, method_name, options);
    result.anchor_symbol = anchor.method_symbol;
    result.anchor_item = anchor.call_item;
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



ResolvedAnchor resolve_anchor(Session &session,
                              std::string_view class_name,
                              std::string_view method_name,
                              const ResolveAnchorOptions &options) {
    ResolvedAnchor result;
    result.class_name = std::string(class_name);
    result.method_name = std::string(method_name);

    const auto deadline = std::chrono::steady_clock::now() + options.ready_timeout;

    while (std::chrono::steady_clock::now() < deadline) {
        ++result.attempts;
        try {
            // jdtls workspace/symbols returns multiple matches
            const std::vector<WorkspaceSymbol> symbols =
                session.workspace_symbols(std::string(class_name));

            const std::vector<WorkspaceSymbol> candidates =
                select_anchor_candidates(symbols, class_name, options.scope_root);

            result.candidate_count = std::max(result.candidate_count, candidates.size());

            for (const auto &candidate : candidates) {
                const auto anchor =
                    try_resolve_method_anchor_in_file_once(session,
                                                           candidate.path,
                                                           method_name);
                if (anchor.has_value()) {
                    result.class_symbol = candidate;
                    result.file = anchor->file;
                    result.method_symbol = anchor->method_symbol;
                    result.call_item = anchor->call_item;
                    return result;
                }
            }
        } catch (...) {
            // best effort during server/project warmup
        }

        std::this_thread::sleep_for(options.retry_interval);
    }

    throw std::runtime_error("failed to resolve anchor class+method: " +
                             std::string(class_name) + "." +
                             std::string(method_name));
}


}


