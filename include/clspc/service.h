#pragma once

#include "clspc/jdtls.h"
#include "clspc/lsp_types.h"

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

}  // namespace clspc::service
