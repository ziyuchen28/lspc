#pragma once

#include "clspc/lsp_types.h"

#include <filesystem>
#include <memory>
#include <string>

#include <pcr/proc/piped_child.h>


namespace clspc {

struct SessionOptions 
{
    std::filesystem::path root_dir;
    std::string client_name{"clspc"};
    std::string client_version{"0.1"};
    bool trace_lsp_messages{false};
    bool trace_request_timing{false};
};

class Session 
{
public:
    Session(pcr::proc::PipedChild child, SessionOptions options);

    Session(Session&&) noexcept;
    Session& operator=(Session&&) noexcept;
    ~Session();

    Session(const Session&) = delete;
    Session &operator=(const Session&) = delete;

    InitializeResult initialize();
    void initialized();
    void shutdown_and_exit();

    void wait();

    // document sync
    int sync_disk_file(const std::filesystem::path &path);
    int sync_text(const std::filesystem::path &path,
                  std::string text,
                  std::string language_id = "plaintext");
    void close_file(const std::filesystem::path &path);

    std::vector<DocumentSymbol> document_symbols(const std::filesystem::path &path);
    std::vector<WorkspaceSymbol> workspace_symbols(std::string query);
    std::vector<Location> definition(const std::filesystem::path &path, Position pos);

    std::vector<CallHierarchyItem> prepare_call_hierarchy(const std::filesystem::path &path,
                                                          Position pos);

    std::vector<OutgoingCall> outgoing_calls(const CallHierarchyItem &item);
    std::vector<IncomingCall> incoming_calls(const CallHierarchyItem &item);
    std::vector<Location> implementation(const std::filesystem::path &path, Position pos);
    std::vector<Location> references(const std::filesystem::path &path,
                                 Position pos,
                                 bool include_declaration = true);


private:

    // PIMPL idiom - avoid including a lot of pcr libs headers
    struct Impl;
    std::unique_ptr<Impl> impl_;
    void ensure_query_document_available(const std::filesystem::path &path);
    std::string request_json_raw(std::string_view method,
                                 std::string params_json,
                                 const char *error_prefix);
};

}  // namespace clspc
