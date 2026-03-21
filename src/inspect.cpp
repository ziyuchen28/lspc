#include "clspc/inspect.h"
#include "clspc/semantic.h"

#include <ostream>
#include <sstream>
#include <iostream>

namespace clspc {


std::string_view logical_name(std::string_view s) 
{
    const auto p = s.find('(');
    return (p == std::string_view::npos) ? s : s.substr(0, p);
}


const char *symbol_kind_name(SymbolKind kind) 
{
    switch (kind) {
        case SymbolKind::File: return "File";
        case SymbolKind::Module: return "Module";
        case SymbolKind::Namespace: return "Namespace";
        case SymbolKind::Package: return "Package";
        case SymbolKind::Class: return "Class";
        case SymbolKind::Method: return "Method";
        case SymbolKind::Property: return "Property";
        case SymbolKind::Field: return "Field";
        case SymbolKind::Constructor: return "Constructor";
        case SymbolKind::Enum: return "Enum";
        case SymbolKind::Interface: return "Interface";
        case SymbolKind::Function: return "Function";
        case SymbolKind::Variable: return "Variable";
        case SymbolKind::Constant: return "Constant";
        case SymbolKind::String: return "String";
        case SymbolKind::Number: return "Number";
        case SymbolKind::Boolean: return "Boolean";
        case SymbolKind::Array: return "Array";
        case SymbolKind::Object: return "Object";
        case SymbolKind::Key: return "Key";
        case SymbolKind::Null: return "Null";
        case SymbolKind::EnumMember: return "EnumMember";
        case SymbolKind::Struct: return "Struct";
        case SymbolKind::Event: return "Event";
        case SymbolKind::Operator: return "Operator";
        case SymbolKind::TypeParameter: return "TypeParameter";
    }
    return "Unknown";
}


std::string format_range(const Range &range) 
{
    std::ostringstream out;
    out << "[" << (range.start.line + 1) << ":" << (range.start.character + 1)
        << " - " << (range.end.line + 1) << ":" << (range.end.character + 1)
        << "]";
    return out.str();
}


void print_initialize_result(std::ostream &os, const InitializeResult &init) 
{
    os << "server_name=" << init.server_name << "\n";
    os << "server_version=" << init.server_version << "\n";
    os << "definitionProvider=" << (init.has_definition_provider ? "true" : "false") << "\n";
    os << "implementationProvider=" << (init.has_implementation_provider ? "true" : "false") << "\n";
    os << "referencesProvider=" << (init.has_references_provider ? "true" : "false") << "\n";
    os << "hoverProvider=" << (init.has_hover_provider ? "true" : "false") << "\n";
    os << "documentSymbolProvider=" << (init.has_document_symbol_provider ? "true" : "false") << "\n";
    os << "workspaceSymbolProvider=" << (init.has_workspace_symbol_provider ? "true" : "false") << "\n";
    os << "callHierarchyProvider=" << (init.has_call_hierarchy_provider ? "true" : "false") << "\n";
}


void print_document_symbols(std::ostream &os,
                            const std::vector<DocumentSymbol> &symbols,
                            int depth) 
{
    for (const auto &sym : symbols) {
        os << std::string(static_cast<std::size_t>(depth * 2), ' ')
           << "- name=" << sym.name
           << " logical=" << logical_name(sym.name)
           << " kind=" << symbol_kind_name(sym.kind)
           << " range=" << format_range(sym.range)
           << " selection=" << format_range(sym.selection_range)
           << "\n";
        print_document_symbols(os, sym.children, depth + 1);
    }
}


void print_workspace_symbols(std::ostream &os,
                             const std::vector<WorkspaceSymbol> &symbols) 
{
    if (symbols.empty()) {
        os << "(no workspace symbols)\n";
        return;
    }

    for (const auto &sym : symbols) {
        os << "- name=" << sym.name
           << " logical=" << logical_name(sym.name)
           << " kind=" << symbol_kind_name(sym.kind)
           << " file=" << (sym.path.empty() ? "<none>" : sym.path.filename().string());

        if (sym.range.has_value()) {
            os << " range=" << format_range(*sym.range);
        }

        if (!sym.detail.empty()) {
            os << " detail=" << sym.detail;
        }

        if (!sym.container_name.empty()) {
            os << " container=" << sym.container_name;
        }

        if (sym.data_json.has_value()) {
            os << " data=" << *sym.data_json;
        }

        os << "\n";
    }
}


void print_locations(std::ostream &os,
                     const std::vector<Location> &locations) 
{
    if (locations.empty()) {
        os << "(no locations)\n";
        return;
    }

    for (const auto &loc : locations) {
        os << "- file=" << (loc.path.empty() ? "<none>" : loc.path.filename().string())
           << " range=" << format_range(loc.range)
           << "\n";
    }
}


void print_call_hierarchy_items(std::ostream &os,
                                const std::vector<CallHierarchyItem> &items) 
{
    if (items.empty()) {
        os << "(no call hierarchy items)\n";
        return;
    }
    for (const auto &item : items) {
        os << "- name=" << item.name
           << " logical=" << logical_name(item.name)
           << " kind=" << symbol_kind_name(item.kind)
           << " file=" << (item.path.empty() ? "<none>" : item.path.filename().string())
           << " range=" << format_range(item.range)
           << " selection=" << format_range(item.selection_range);
        if (item.data_json.has_value()) {
            os << " data=" << *item.data_json;
        }
        os << "\n";
    }
}


void print_outgoing_calls(std::ostream &os,
                          const std::vector<OutgoingCall> &calls) 
{
    if (calls.empty()) {
        os << "(no outgoing calls)\n";
        return;
    }
    for (const auto &call : calls) {
        os << "- to=" << call.to.name
           << " logical=" << logical_name(call.to.name)
           << " file=" << (call.to.path.empty() ? "<none>" : call.to.path.filename().string())
           << " range=" << format_range(call.to.range)
           << "\n";
        if (!call.from_ranges.empty()) {
            os << "  fromRanges:\n";
            for (const auto &range : call.from_ranges) {
                os << "    " << format_range(range) << "\n";
            }
        }
    }
}


void print_incoming_calls(std::ostream &os,
                          const std::vector<IncomingCall> &calls) 
{
    if (calls.empty()) {
        os << "(no incoming calls)\n";
        return;
    }

    for (const auto &call : calls) {
        os << "- from=" << call.from.name
           << " logical=" << logical_name(call.from.name)
           << " file=" << (call.from.path.empty() ? "<none>" : call.from.path.filename().string())
           << " range=" << format_range(call.from.range)
           << "\n";

        if (!call.from_ranges.empty()) {
            os << "  fromRanges:\n";
            for (const auto &range : call.from_ranges) {
                os << "    " << format_range(range) << "\n";
            }
        }
    }
}


void print_section(const std::string &title) 
{
    std::cout << "\n=== " << title << " ===\n";
}


void print_expanded_node(const ExpandedNode &node, int depth) 
{
    const std::string indent(static_cast<std::size_t>(depth * 2), ' ');

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
        print_expanded_node(child, depth + 1);
    }
}


void print_expanded_snippets(const std::vector<ExpandedSnippet> &snippets) 
{
    for (const auto &snippet : snippets) {
        std::cout << "---- "
                  << snippet.item.path.filename().string()
                  << " :: "
                  << snippet.item.name
                  << "  stop="
                  << (snippet.stop_reason.empty() ? "<none>" : snippet.stop_reason)
                  << "  ["
                  << snippet.window.start_line
                  << "-"
                  << snippet.window.end_line
                  << "]\n";
        std::cout << snippet.window.text << "\n\n";
    }
}


void print_expansion_result(const std::string &label,
                            const ExpansionResult &result) 
{
    // print_section(label + " retry");
    // std::cout << "attempts=" << result.attempts << "\n";

    if (result.initial_edge_probe_attempts > 0 || result.initial_edge_count > 0) {
        print_section(label + " root edge probe");
        std::cout << "probe_attempts=" << result.initial_edge_probe_attempts << "\n";
        std::cout << "edge_count=" << result.initial_edge_count << "\n";
    }

    print_section(label + " anchor method");
    std::cout << "name=" << result.anchor_symbol.name
              << " logical=" << logical_name(result.anchor_symbol.name)
              << " range=" << format_range(result.anchor_symbol.range)
              << " selection=" << format_range(result.anchor_symbol.selection_range)
              << "\n";

    print_section(label + " call hierarchy anchor");
    print_call_hierarchy_items(std::cout,
                               std::vector<CallHierarchyItem>{result.anchor_item});

    print_section(label + " expanded dependency tree");
    print_expanded_node(result.root);

    print_section(label + " fetched code snippets");
    const std::vector<ExpandedSnippet> snippets =
        collect_unique_snippets(result.root);
    print_expanded_snippets(snippets);
}



}  // namespace clspc


