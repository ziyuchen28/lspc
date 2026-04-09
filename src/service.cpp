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

}  // namespace


InitializeProbeResponse run_initialize_probe(const InitializeProbeRequest &req)
{
    clspc::jdtls::LaunchOptions launch = req.launch;
    validate_launch(launch);

    launch.root_dir = normalize_abs(launch.root_dir);
    launch.workspace_dir = normalize_abs(launch.workspace_dir);
    launch.jdtls_home = normalize_abs(launch.jdtls_home);

    std::filesystem::create_directories(launch.workspace_dir);

    auto child = clspc::jdtls::spawn(launch, clspc::jdtls::current_platform());

    clspc::SessionOptions session_options;
    session_options.root_dir = launch.root_dir;
    session_options.trace_lsp_messages = req.trace_lsp_messages;
    session_options.trace_request_timing = req.trace_request_timing;

    clspc::Session session(std::move(child), session_options);

    try {
        InitializeProbeResponse out;
        out.initialize = session.initialize();
        session.initialized();

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

}  // namespace clspc::service


