#pragma once

#include "clspc/semantic.h"

#include <chrono>
#include <filesystem>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace clspc {

enum class ExpandDirection 
{
    Outgoing,
    Incoming,
    Both,
};

enum class ManagerState 
{
    Stopped,
    Starting,
    Initializing,
    WarmingUp,
    Ready,
    Error,
};

enum class QueryStatus 
{
    Ok,
    WarmingUp,
    TimedOut,
    InvalidArgument,
    Error,
};

// FileMethod: user provides abs file path and anchor method
// ClassMethod: user provides only the class name and anchor method
// meaning extra anchoring and search is needed to locate the file path
enum class AnchorKind 
{
    FileMethod,
    ClassMethod,
};


struct WarmupProbeConfig 
{
    std::string class_name;
    std::string method_name;
    ExpandDirection direction{ExpandDirection::Both};
    int max_depth{5};
};


struct WorkspaceConfig 
{
    std::filesystem::path workspace_root;
    std::filesystem::path scope_root;
    std::filesystem::path jdtls_home;
    std::filesystem::path workspace_data_dir;

    std::string java_bin{"java"};
    int xms_mb{512};
    int xmx_mb{8192};

    std::chrono::milliseconds ready_timeout{std::chrono::milliseconds(20000)};
    std::chrono::milliseconds retry_interval{std::chrono::milliseconds(250)};
    std::chrono::milliseconds request_timeout{std::chrono::milliseconds(5000)};

    std::optional<WarmupProbeConfig> warmup_probe;

    bool trace_lsp_messages{false};
    bool trace_request_timing{false};
};


struct AnchorSpec {
    AnchorKind kind{AnchorKind::ClassMethod};

    std::filesystem::path file;
    std::string class_name;
    std::string method_name;
};

struct ExpandRequest {
    AnchorSpec anchor;

    ExpandDirection direction{ExpandDirection::Outgoing};
    int max_depth{3};

    std::size_t snippet_padding_before{1};
    std::size_t snippet_padding_after{1};
};

struct DirectionResult {
    ExpansionResult expansion;
    std::vector<ExpandedSnippet> snippets;
};

struct ExpandResponse {
    QueryStatus status{QueryStatus::Error};
    std::string message;

    // Always set on success. For FileMethod requests, class_symbol/class_name
    // inside ResolvedAnchor may be empty/default.
    std::optional<ResolvedAnchor> anchor;

    // Present depending on requested direction.
    std::optional<DirectionResult> outgoing;
    std::optional<DirectionResult> incoming;
};


struct WorkspaceStatus 
{
    ManagerState state{ManagerState::Stopped};

    bool initialized{false};
    bool warmup_enabled{false};
    bool warmup_complete{false};

    std::string last_error;
    std::size_t active_progress_tokens{0};
};


class WorkspaceManager 
{
public:
    explicit WorkspaceManager(WorkspaceConfig config);

    WorkspaceManager(WorkspaceManager &&) noexcept;
    WorkspaceManager &operator=(WorkspaceManager &&) noexcept;
    ~WorkspaceManager();

    WorkspaceManager(const WorkspaceManager &) = delete;
    WorkspaceManager &operator=(const WorkspaceManager &) = delete;

    void start();
    void stop();

    WorkspaceStatus status() const;

    // One single e2e endpoint:
    // - if anchor.kind == ClassMethod, resolve class -> file first
    // - if anchor.kind == FileMethod, use file directly
    // - then run semantic expansion
    ExpandResponse expand(const ExpandRequest &request);

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

}  // namespace clspc
