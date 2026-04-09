#pragma once

#include "clspc/jdtls.h"
#include "clspc/lsp_types.h"

#include <string>

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

InitializeProbeResponse run_initialize_probe(const InitializeProbeRequest &req);

}  // namespace clspc::service
