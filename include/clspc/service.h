#pragma once

#include "clspc/jdtls.h"
#include "clspc/lsp_types.h"
#include "clspc/semantic.h"

namespace clspc::service {

struct InitializeProbeRequest
{
    clspc::jdtls::LaunchOptions launch;
    bool trace_lsp_messages{false};
    bool trace_request_timing{false};
};

struct InitializeProbeResponse
{
    clspc::InitializeResult initialize;
};


struct DocumentSymbolsRequest
{
    clspc::jdtls::LaunchOptions launch;
    std::filesystem::path file;
    bool trace_lsp_messages{false};
    bool trace_request_timing{false};
};

struct DocumentSymbolsResponse
{
    std::filesystem::path file;
    std::vector<clspc::DocumentSymbol> symbols;
};

InitializeProbeResponse run_initialize_probe(const InitializeProbeRequest &req);

DocumentSymbolsResponse run_document_symbols(const DocumentSymbolsRequest &req);

struct ResolveAnchorRequest
{
    clspc::jdtls::LaunchOptions launch;
    std::string class_name;
    std::string method_name;

    std::chrono::milliseconds ready_timeout{std::chrono::milliseconds{20000}};
    std::chrono::milliseconds retry_interval{std::chrono::milliseconds{250}};

    bool trace_lsp_messages{false};
    bool trace_request_timing{false};
};

struct ResolveAnchorResponse
{
    clspc::ResolvedAnchor anchor;
};

ResolveAnchorResponse run_resolve_anchor(const ResolveAnchorRequest &req);

}  // namespace clspc::service
