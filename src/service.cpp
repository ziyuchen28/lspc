#include "clspc/service.h"
#include "clspc/session.h"

#include <filesystem>
#include <stdexcept>
#include <utility>
#include <iostream>
#include <string_view>

#include <thread>
#include <unistd.h>
#include <fcntl.h>
#include <cerrno>
#include <cstring>

namespace clspc::service {


namespace {

struct StderrDrainer
{
    int fd{-1};
    int out_fd{-1};
    std::thread th;

    explicit StderrDrainer(int stderr_fd, const std::string &path)
        : fd(stderr_fd)
    {
        out_fd = ::open(path.c_str(), O_CREAT | O_WRONLY | O_APPEND, 0644);
        if (out_fd < 0) {
            throw std::runtime_error(
                "failed to open stderr log file: " + std::string(std::strerror(errno)));
        }

        th = std::thread([this]() {
            char buf[4096];

            for (;;) {
                const ssize_t n = ::read(fd, buf, sizeof(buf));
                if (n == 0) {
                    break;
                }
                if (n < 0) {
                    if (errno == EINTR) {
                        continue;
                    }
                    break;
                }

                ssize_t written = 0;
                while (written < n) {
                    const ssize_t m = ::write(out_fd, buf + written, static_cast<std::size_t>(n - written));
                    if (m < 0) {
                        if (errno == EINTR) {
                            continue;
                        }
                        return;
                    }
                    written += m;
                }
            }
        });
    }

    ~StderrDrainer()
    {
        if (th.joinable()) {
            th.join();
        }
        if (out_fd >= 0) {
            ::close(out_fd);
        }
    }

    StderrDrainer(const StderrDrainer &) = delete;
    StderrDrainer &operator=(const StderrDrainer &) = delete;
    StderrDrainer(StderrDrainer &&) = delete;
    StderrDrainer &operator=(StderrDrainer &&) = delete;
};


void service_trace_line(bool enabled, std::string_view line)
{
    if (!enabled) {
        return;
    }

    std::cerr << "[service] " << line << "\n";
    std::cerr.flush();
}

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
    const bool trace = trace_lsp_messages || trace_request_timing;
    service_trace_line(trace, "spawn begin");
    auto child = clspc::jdtls::spawn(launch, clspc::jdtls::current_platform());
    service_trace_line(trace, "spawn done");


    std::optional<StderrDrainer> stderr_drainer;
    if (const char *path = std::getenv("CLSPC_CHILD_STDERR_LOG")) {
        // handle empty path
        if (*path != '\0') {
            service_trace_line(trace, "stderr drainer begin");
            stderr_drainer.emplace(child.stderr_read_fd(), path);
            service_trace_line(trace, "stderr drainer ready");
        }
    }

    clspc::SessionOptions session_options;
    session_options.root_dir = launch.root_dir;
    session_options.trace_lsp_messages = trace_lsp_messages;
    session_options.trace_request_timing = trace_request_timing;

    clspc::Session session(std::move(child), session_options);

    try {
        service_trace_line(trace, "initialize begin");
        const clspc::InitializeResult initialize = session.initialize();
        service_trace_line(trace, "initialize done");
        session.initialized();
        service_trace_line(trace, "initialized notification sent");
        service_trace_line(trace, "handler begin");
        auto out = std::forward<Fn>(fn)(session, initialize);
        service_trace_line(trace, "handler done");
        service_trace_line(trace, "shutdown_and_exit begin");
        session.shutdown_and_exit();
        service_trace_line(trace, "shutdown_and_exit done");
        service_trace_line(trace, "wait begin");
        session.wait();
        service_trace_line(trace, "wait done");

        return out;
    } catch (...) {
        service_trace_line(trace, "exception path entered");

        try {
            service_trace_line(trace, "shutdown_and_exit begin (exception path)");
            session.shutdown_and_exit();
            service_trace_line(trace, "shutdown_and_exit done (exception path)");
        } catch (...) {
            service_trace_line(trace, "shutdown_and_exit failed (exception path)");
        }

        try {
            service_trace_line(trace, "wait begin (exception path)");
            session.wait();
            service_trace_line(trace, "wait done (exception path)");
        } catch (...) {
            service_trace_line(trace, "wait failed (exception path)");
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


ResolveAnchorResponse run_resolve_anchor(const ResolveAnchorRequest &req)
{
    if (req.class_name.empty()) {
        throw std::runtime_error("ResolveAnchorRequest.class_name must not be empty");
    }

    if (req.method_name.empty()) {
        throw std::runtime_error("ResolveAnchorRequest.method_name must not be empty");
    }

    const clspc::jdtls::LaunchOptions launch = prepare_launch(req.launch);

    return with_started_session(
        launch,
        req.trace_lsp_messages,
        req.trace_request_timing,
        [&](clspc::Session &session, const clspc::InitializeResult &initialize) {
            (void)initialize;

            clspc::ResolveAnchorOptions options;
            options.scope_root = launch.root_dir;
            options.ready_timeout = req.ready_timeout;
            options.retry_interval = req.retry_interval;

            ResolveAnchorResponse out;
            out.anchor = clspc::resolve_anchor(session,
                                               req.class_name,
                                               req.method_name,
                                               options);
            return out;
        });
}


}  // namespace clspc::service


