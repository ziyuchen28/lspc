#pragma once

#include "clspc/lsp_types.h"
#include "clspc/session.h"
#include "clspc/source_window.h"

#include <chrono>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace clspc {


struct ExpandOptions 
{
    std::filesystem::path scope_root;
    int max_depth{3};

    std::chrono::milliseconds ready_timeout{20000};
    std::chrono::milliseconds retry_interval{250};

    std::size_t snippet_padding_before{1};
    std::size_t snippet_padding_after{1};
};


struct ExpandedNode 
{
    CallHierarchyItem item;
    std::vector<Range> from_ranges;
    std::string stop_reason;
    std::optional<SourceWindow> snippet;
    std::vector<ExpandedNode> children;
};


struct ExpansionResult 
{
    std::filesystem::path anchor_file;
    std::string anchor_method;
    DocumentSymbol anchor_symbol;
    CallHierarchyItem anchor_item;
    ExpandedNode root;
    std::size_t attempts{0};
    // debug
    std::size_t initial_edge_probe_attempts{0};
    std::size_t initial_edge_count{0};
};


struct ExpandedSnippet 
{
    CallHierarchyItem item;
    std::string stop_reason;
    SourceWindow window;
};


struct AnchorResolution 
{
    DocumentSymbol symbol;
    CallHierarchyItem item;
    std::size_t attempts{0};
};


std::optional<DocumentSymbol> find_method_symbol(const std::vector<DocumentSymbol> &symbols,
                                                 std::string_view method_name);


ExpansionResult expand_outgoing_from_method(Session &session,
                                            const std::filesystem::path &file,
                                            std::string_view method_name,
                                            const ExpandOptions &options);

ExpansionResult expand_incoming_to_method(Session &session,
                                          const std::filesystem::path &file,
                                          std::string_view method_name,
                                          const ExpandOptions &options);

std::vector<ExpandedSnippet> collect_unique_snippets(const ExpandedNode &root);

}  // namespace clspc
