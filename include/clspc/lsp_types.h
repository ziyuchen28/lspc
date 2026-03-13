#pragma once

#include <filesystem>
#include <optional>
#include <string>
#include <vector>

namespace clspc {

struct Position 
{
    int line{0};       // 0-based
    int character{0};  // 0-based
};

struct Range 
{
    Position start{};
    Position end{};
};

struct Location 
{
    std::filesystem::path path;
    std::string uri;
    Range range{};
};


enum class SymbolKind : int 
{
    File = 1,
    Module = 2,
    Namespace = 3,
    Package = 4,
    Class = 5,
    Method = 6,
    Property = 7,
    Field = 8,
    Constructor = 9,
    Enum = 10,
    Interface = 11,
    Function = 12,
    Variable = 13,
    Constant = 14,
    String = 15,
    Number = 16,
    Boolean = 17,
    Array = 18,
    Object = 19,
    Key = 20,
    Null = 21,
    EnumMember = 22,
    Struct = 23,
    Event = 24,
    Operator = 25,
    TypeParameter = 26,
};


struct DocumentSymbol 
{
    std::string name;
    std::string detail;
    SymbolKind kind{SymbolKind::Variable};
    Range range{};
    Range selection_range{};
    std::vector<DocumentSymbol> children;
};


enum class DiagnosticSeverity : int 
{
    Error = 1,
    Warning = 2,
    Information = 3,
    Hint = 4,
};


struct Diagnostic 
{
    std::filesystem::path path;
    Range range{};
    DiagnosticSeverity severity{DiagnosticSeverity::Information};
    std::string code;
    std::string message;
};


struct HoverInfo 
{
    std::string markdown;
    std::optional<Range> range;
};


struct CallHierarchyItem 
{
    std::string name;
    std::string detail;
    SymbolKind kind{SymbolKind::Method};
    std::filesystem::path path;
    std::string uri;
    Range range{};
    Range selection_range{};
};


struct OutgoingCall 
{
    CallHierarchyItem to;
    std::vector<Range> from_ranges;
};


struct IncomingCall 
{
    CallHierarchyItem from;
    std::vector<Range> from_ranges;
};


struct InitializeResult 
{
    std::string server_name;
    std::string server_version;
    bool has_definition_provider{false};
    bool has_references_provider{false};
    bool has_hover_provider{false};
    bool has_document_symbol_provider{false};
    bool has_call_hierarchy_provider{false};
};

}  // namespace clspc



