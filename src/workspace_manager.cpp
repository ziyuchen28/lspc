#include "clspc/workspace_manager.h"

#include "clspc/jdtls.h"
#include "clspc/session.h"

#include <stdexcept>
#include <utility>

namespace clspc {

namespace {

SessionOptions make_session_options(const WorkspaceConfig &cfg) 
{
    SessionOptions options;
    options.root_dir = cfg.workspace_root;
    options.client_name = "clspc-manager";
    options.client_version = "0.1";
    options.trace_lsp_messages = cfg.trace_lsp_messages;
    options.trace_request_timing = cfg.trace_request_timing;
    return options;
}

jdtls::LaunchOptions make_launch_options(const WorkspaceConfig &cfg) 
{
    jdtls::LaunchOptions launch;
    launch.jdtls_home = cfg.jdtls_home;
    launch.workspace_dir = cfg.workspace_data_dir;
    launch.root_dir = cfg.workspace_root;
    launch.java_bin = cfg.java_bin;
    launch.xms_mb = cfg.xms_mb;
    launch.xmx_mb = cfg.xmx_mb;
    launch.log_protocol = false;
    launch.log_level = "INFO";
    return launch;
}

ResolveAnchorOptions make_resolve_options(const WorkspaceConfig &cfg) 
{
    ResolveAnchorOptions options;
    options.scope_root = cfg.scope_root.empty() ? cfg.workspace_root : cfg.scope_root;
    options.ready_timeout = cfg.ready_timeout;
    options.retry_interval = cfg.retry_interval;
    return options;
}

ExpandOptions make_expand_options(const WorkspaceConfig &cfg,
                                  const ExpandRequest &request) {
    ExpandOptions options;
    options.scope_root = cfg.scope_root.empty() ? cfg.workspace_root : cfg.scope_root;
    options.max_depth = request.max_depth;
    options.ready_timeout = cfg.ready_timeout;
    options.retry_interval = cfg.retry_interval;
    options.snippet_padding_before = request.snippet_padding_before;
    options.snippet_padding_after = request.snippet_padding_after;
    return options;
}


ResolvedAnchor make_file_method_anchor(const std::filesystem::path &file,
                                       std::string method_name,
                                       const ExpansionResult &expansion) {
    ResolvedAnchor anchor;
    anchor.class_name = {};
    anchor.method_name = std::move(method_name);
    anchor.class_symbol = WorkspaceSymbol{};
    anchor.file = std::filesystem::absolute(file).lexically_normal();
    anchor.method_symbol = expansion.anchor_symbol;
    anchor.call_item = expansion.anchor_item;
    anchor.attempts = expansion.attempts;
    anchor.candidate_count = 0;
    return anchor;
}

DirectionResult make_direction_result(const ExpansionResult &expansion) {
    DirectionResult result;
    result.expansion = expansion;
    result.snippets = collect_unique_snippets(expansion.root);
    return result;
}

struct ExpandWork {
    std::optional<ResolvedAnchor> anchor;
    std::optional<DirectionResult> outgoing;
    std::optional<DirectionResult> incoming;
};

ExpandWork run_expand(Session &session,
                      const WorkspaceConfig &cfg,
                      const ExpandRequest &request) {
    ExpandWork out;

    const ExpandOptions expand_options = make_expand_options(cfg, request);

    std::filesystem::path effective_file;
    std::string effective_method = request.anchor.method_name;

    if (request.anchor.kind == AnchorKind::ClassMethod) {
        const ResolveAnchorOptions resolve_options = make_resolve_options(cfg);
        const ResolvedAnchor resolved =
            resolve_anchor(session,
                           request.anchor.class_name,
                           request.anchor.method_name,
                           resolve_options);

        effective_file = resolved.file;
        effective_method = resolved.method_name;
        out.anchor = resolved;
    } else {
        effective_file = std::filesystem::absolute(request.anchor.file).lexically_normal();
    }

    if (request.direction == ExpandDirection::Outgoing ||
        request.direction == ExpandDirection::Both) {
        const ExpansionResult expansion =
            expand_outgoing_from_method(session,
                                        effective_file,
                                        effective_method,
                                        expand_options);
        out.outgoing = make_direction_result(expansion);

        if (!out.anchor.has_value()) {
            out.anchor = make_file_method_anchor(effective_file,
                                                effective_method,
                                                expansion);
        }
    }

    if (request.direction == ExpandDirection::Incoming ||
        request.direction == ExpandDirection::Both) {
        const ExpansionResult expansion =
            expand_incoming_to_method(session,
                                      effective_file,
                                      effective_method,
                                      expand_options);
        out.incoming = make_direction_result(expansion);

        if (!out.anchor.has_value()) {
            out.anchor = make_file_method_anchor(effective_file,
                                                effective_method,
                                                expansion);
        }
    }

    return out;
}

}  // namespace


struct WorkspaceManager::Impl 
{
    explicit Impl(WorkspaceConfig cfg)
        : config(std::move(cfg)) {
        status.warmup_enabled = config.warmup_probe.has_value();
        if (config.scope_root.empty()) {
            config.scope_root = config.workspace_root;
        }
    }

    WorkspaceConfig config;
    WorkspaceStatus status;
    std::unique_ptr<Session> session;
    std::optional<InitializeResult> initialize_result;
};


WorkspaceManager::WorkspaceManager(WorkspaceConfig config)
    : impl_(std::make_unique<Impl>(std::move(config))) {}

WorkspaceManager::WorkspaceManager(WorkspaceManager &&) noexcept = default;
WorkspaceManager &WorkspaceManager::operator=(WorkspaceManager &&) noexcept = default;
WorkspaceManager::~WorkspaceManager() = default;

void WorkspaceManager::start() 
{
    if (impl_->status.state == ManagerState::Ready) {
        return;
    }

    if (impl_->status.state != ManagerState::Stopped &&
        impl_->status.state != ManagerState::Error) {
        throw std::runtime_error("workspace manager is already running or starting");
    }

    if (impl_->status.state == ManagerState::Error) {
        stop();
    }

    if (impl_->config.workspace_root.empty()) {
        throw std::runtime_error("WorkspaceConfig.workspace_root must not be empty");
    }
    if (impl_->config.jdtls_home.empty()) {
        throw std::runtime_error("WorkspaceConfig.jdtls_home must not be empty");
    }
    if (impl_->config.workspace_data_dir.empty()) {
        throw std::runtime_error("WorkspaceConfig.workspace_data_dir must not be empty");
    }

    impl_->status = WorkspaceStatus{};
    impl_->status.state = ManagerState::Starting;
    impl_->status.warmup_enabled = impl_->config.warmup_probe.has_value();

    try {
        const jdtls::LaunchOptions launch =
            make_launch_options(impl_->config);

        auto child = jdtls::spawn(launch, jdtls::current_platform());

        impl_->status.state = ManagerState::Initializing;

        impl_->session = std::make_unique<Session>(std::move(child),
                                                   make_session_options(impl_->config));

        impl_->initialize_result = impl_->session->initialize();
        impl_->session->initialized();
        impl_->status.initialized = true;

        if (impl_->config.warmup_probe.has_value()) {
            impl_->status.state = ManagerState::WarmingUp;

            ExpandRequest probe_request;
            probe_request.anchor.kind = AnchorKind::ClassMethod;
            probe_request.anchor.class_name = impl_->config.warmup_probe->class_name;
            probe_request.anchor.method_name = impl_->config.warmup_probe->method_name;
            probe_request.direction = impl_->config.warmup_probe->direction;
            probe_request.max_depth = impl_->config.warmup_probe->max_depth;
            probe_request.snippet_padding_before = 0;
            probe_request.snippet_padding_after = 0;

            const ExpandWork warmup =
                run_expand(*impl_->session, impl_->config, probe_request);

            (void)warmup;
            impl_->status.warmup_complete = true;
        }

        impl_->status.state = ManagerState::Ready;
    } catch (const std::exception &ex) {
        impl_->status.state = ManagerState::Error;
        impl_->status.last_error = ex.what();
        throw;
    }
}

void WorkspaceManager::stop() {
    if (!impl_->session) {
        impl_->status = WorkspaceStatus{};
        impl_->status.state = ManagerState::Stopped;
        return;
    }

    try {
        impl_->session->shutdown_and_exit();
        impl_->session->wait();
    } catch (...) {
        // best effort shutdown
    }

    impl_->session.reset();
    impl_->initialize_result.reset();
    impl_->status = WorkspaceStatus{};
    impl_->status.state = ManagerState::Stopped;
}

WorkspaceStatus WorkspaceManager::status() const {
    return impl_->status;
}

ExpandResponse WorkspaceManager::expand(const ExpandRequest &request) {
    ExpandResponse response;

    try {
        if (impl_->status.state == ManagerState::Stopped) {
            start();
        }

        if (impl_->status.state != ManagerState::Ready || !impl_->session) {
            response.status = QueryStatus::NotReady;
            response.message = "workspace manager is not ready";
            return response;
        }

        if (request.anchor.method_name.empty()) {
            response.status = QueryStatus::InvalidArgument;
            response.message = "anchor.method_name must not be empty";
            return response;
        }

        if (request.anchor.kind == AnchorKind::ClassMethod &&
            request.anchor.class_name.empty()) {
            response.status = QueryStatus::InvalidArgument;
            response.message = "anchor.class_name must not be empty for ClassMethod";
            return response;
        }

        if (request.anchor.kind == AnchorKind::FileMethod &&
            request.anchor.file.empty()) {
            response.status = QueryStatus::InvalidArgument;
            response.message = "anchor.file must not be empty for FileMethod";
            return response;
        }

        const ExpandWork work =
            run_expand(*impl_->session, impl_->config, request);

        response.status = QueryStatus::Ok;
        response.message.clear();
        response.anchor = work.anchor;
        response.outgoing = work.outgoing;
        response.incoming = work.incoming;
        return response;
    } catch (const std::exception &ex) {
        impl_->status.state = ManagerState::Error;
        impl_->status.last_error = ex.what();

        response.status = QueryStatus::Error;
        response.message = ex.what();
        return response;
    }
}

}  // namespace clspc
