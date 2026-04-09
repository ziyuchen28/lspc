#include "clspc/service.h"
#include "clspc/session.h"

#include <filesystem>
#include <stdexcept>
#include <utility>

namespace clspc::service {


namespace {

std::filesystem::path normalize_abs(const std::filesystem::path &path)
{
    return std::filesystem::absolute(path).lexically_normal();
}

void validate_launch(const clspc::jdtls::LaunchOptions &launch)
{
    if (launch.root_dir.empty()) {
        throw std::runtime_error("LaunchOptions.root_dir must not be empty");
    }
    if (launch.workspace_dir.empty()) {
        throw std::runtime_error("LaunchOptions.workspace_dir must not be empty");
    }
    if (launch.jdtls_home.empty()) {
        throw std::runtime_error("LaunchOptions.jdtls_home must not be empty");
    }
    if (launch.java_bin.empty()) {
        throw std::runtime_error("LaunchOptions.java_bin must not be empty");
    }
}


clspc::jdtls::LaunchOptions prepare_launch(const clspc::jdtls::LaunchOptions &input)
{
    clspc::jdtls::LaunchOptions launch = input;
    validate_launch(launch);

    launch.root_dir = normalize_abs(launch.root_dir);
    launch.workspace_dir = normalize_abs(launch.workspace_dir);
    launch.jdtls_home = normalize_abs(launch.jdtls_home);

    std::filesystem::create_directories(launch.workspace_dir);
    return launch;
}


template <typename Fn>
auto with_started_session(const clspc::jdtls::LaunchOptions &launch,
                          bool trace_lsp_messages,
                          bool trace_request_timing,
                          Fn &&fn)
{
    auto child = clspc::jdtls::spawn(launch, clspc::jdtls::current_platform());

    clspc::SessionOptions session_options;
    session_options.root_dir = launch.root_dir;
    session_options.trace_lsp_messages = trace_lsp_messages;
    session_options.trace_request_timing = trace_request_timing;

    clspc::Session session(std::move(child), session_options);

    try {
        const clspc::InitializeResult initialize = session.initialize();
        session.initialized();

        auto out = std::forward<Fn>(fn)(session, initialize);

        session.shutdown_and_exit();
        session.wait();
        return out;
    } catch (...) {
        try {
            session.shutdown_and_exit();
        } catch (...) {
        }

        try {
            session.wait();
        } catch (...) {
        }

        throw;
    }
}


}  // namespace


InitializeProbeResponse run_initialize_probe(const InitializeProbeRequest &req)
{
    const clspc::jdtls::LaunchOptions launch = prepare_launch(req.launch);

    return with_started_session(
        launch,
        req.trace_lsp_messages,
        req.trace_request_timing,
        [](clspc::Session &session, const clspc::InitializeResult &initialize) {
            (void)session;
            InitializeProbeResponse out;
            out.initialize = initialize;
            return out;
        });
}


DocumentSymbolsResponse run_document_symbols(const DocumentSymbolsRequest &req)
{
    if (req.file.empty()) {
        throw std::runtime_error("DocumentSymbolsRequest.file must not be empty");
    }

    const clspc::jdtls::LaunchOptions launch = prepare_launch(req.launch);
    const std::filesystem::path file = normalize_abs(req.file);

    return with_started_session(
        launch,
        req.trace_lsp_messages,
        req.trace_request_timing,
        [&](clspc::Session &session, const clspc::InitializeResult &initialize) {
            (void)initialize;

            DocumentSymbolsResponse out;
            out.file = file;
            out.symbols = session.document_symbols(file);
            return out;
        });
}


}  // namespace clspc::service


