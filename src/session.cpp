#include "clspc/session.h"

#include "clspc/uri.h"

#include <stdexcept>
#include <utility>
#include <fstream>
#include <sstream>
#include <iostream>
#include <stdexcept>
#include <unistd.h>

#include <nlohmann/json.hpp>

#include <pcr/channel/any_stream.h>
#include <pcr/channel/pipe_stream.h>
#include <pcr/framing/any_framer.h>
#include <pcr/framing/content_length_framer.h>
#include <pcr/rpc/any_codec.h>
#include <pcr/rpc/codec/nlohmann.h>
#include <pcr/rpc/dispatcher.h>
#include <pcr/rpc/peer.h>

namespace clspc {


namespace {

using nlohmann::json;


void trace_line(bool enabled, const std::string &line) 
{
    if (!enabled) {
        return;
    }
    std::cerr << line << "\n";
    std::cerr.flush();
}


std::string summarize_json_for_log(const std::string &text) 
{
    constexpr std::size_t k_max = 256;
    if (text.size() <= k_max) {
        return text;
    }
    return text.substr(0, k_max) + "...";
}


bool has_provider(const json &caps, const char *key) 
{
    if (!caps.contains(key)) {
        return false;
    }
    const auto &value = caps.at(key);
    if (value.is_boolean()) {
        return value.get<bool>();
    }
    return !value.is_null();
}


std::string infer_language_id(const std::filesystem::path &path) 
{
    const std::string ext = path.extension().string();
    if (ext == ".java") return "java";
    if (ext == ".xml") return "xml";
    if (ext == ".json") return "json";
    if (ext == ".properties") return "properties";
    return "plaintext";
}


std::string read_file_text(const std::filesystem::path &path) 
{
    std::ifstream in(path);
    if (!in) {
        throw std::runtime_error("failed to open file: " + path.string());
    }
    std::ostringstream out;
    out << in.rdbuf();
    return out.str();
}


Position parse_position(const json &j) 
{
    return Position{
        .line = j.value("line", 0),
        .character = j.value("character", 0),
    };
}


Range parse_range(const json &j) 
{
    return Range{
        .start = parse_position(j.at("start")),
        .end = parse_position(j.at("end")),
    };
}


SymbolKind parse_symbol_kind(const json &j) 
{
    return static_cast<SymbolKind>(j.get<int>());
}


DocumentSymbol parse_document_symbol_object(const json &j) 
{
    DocumentSymbol symbol;
    symbol.name = j.value("name", "");
    symbol.detail = j.value("detail", "");
    symbol.kind = parse_symbol_kind(j.value("kind", 13));

    if (j.contains("range")) {
        symbol.range = parse_range(j.at("range"));
    }
    if (j.contains("selectionRange")) {
        symbol.selection_range = parse_range(j.at("selectionRange"));
    } else {
        symbol.selection_range = symbol.range;
    }

    if (j.contains("children") && j.at("children").is_array()) {
        for (const auto &child : j.at("children")) {
            symbol.children.push_back(parse_document_symbol_object(child));
        }
    }

    return symbol;
}


std::vector<DocumentSymbol> parse_document_symbols(const json &j) 
{
    std::vector<DocumentSymbol> out;
    if (j.is_null() || !j.is_array()) {
        return out;
    }

    for (const auto &item : j) {
        // SymbolInformation fallback
        if (item.contains("location")) {
            DocumentSymbol symbol;
            symbol.name = item.value("name", "");
            symbol.detail = item.value("containerName", "");
            symbol.kind = parse_symbol_kind(item.value("kind", 13));

            const auto &loc = item.at("location");
            if (loc.contains("range")) {
                symbol.range = parse_range(loc.at("range"));
                symbol.selection_range = symbol.range;
            }

            out.push_back(std::move(symbol));
            continue;
        }

        // Proper DocumentSymbol (children optional)
        if (item.contains("range") &&
            item.contains("selectionRange") &&
            item.contains("kind")) {
            out.push_back(parse_document_symbol_object(item));
            continue;
        }
    }

    return out;
}


WorkspaceSymbol parse_workspace_symbol_object(const nlohmann::json &j) 
{
    WorkspaceSymbol sym;
    sym.name = j.value("name", "");
    sym.detail = j.value("detail", "");
    sym.container_name = j.value("containerName", "");
    sym.kind = parse_symbol_kind(j.value("kind", 13));

    if (j.contains("location") && j.at("location").is_object()) {
        const auto &loc = j.at("location");
        sym.uri = loc.value("uri", "");
        if (!sym.uri.empty() && sym.uri.rfind("file://", 0) == 0) {
            sym.path = path_from_file_uri(sym.uri);
        }
        if (loc.contains("range")) {
            sym.range = parse_range(loc.at("range"));
        }
    } else {
        sym.uri = j.value("uri", "");
        if (!sym.uri.empty() && sym.uri.rfind("file://", 0) == 0) {
            sym.path = path_from_file_uri(sym.uri);
        }
        if (j.contains("range")) {
            sym.range = parse_range(j.at("range"));
        }
    }

    if (j.contains("data")) {
        sym.data_json = j.at("data").dump();
    }

    return sym;
}

std::vector<WorkspaceSymbol> parse_workspace_symbols(const nlohmann::json &j) 
{
    std::vector<WorkspaceSymbol> out;
    if (j.is_null() || !j.is_array()) {
        return out;
    }
    for (const auto &item : j) {
        out.push_back(parse_workspace_symbol_object(item));
    }
    return out;
}


json json_position(const Position &p) 
{
    return json{
        {"line", p.line},
        {"character", p.character},
    };
}


json json_range(const Range &r) {
    return json{
        {"start", json_position(r.start)},
        {"end", json_position(r.end)},
    };
}


Location parse_location_object(const json &j) 
{
    // support both Location and LocationLink shapes.
    if (j.contains("targetUri")) {
        const std::string uri = j.at("targetUri").get<std::string>();
        return Location{
            .path = path_from_file_uri(uri),
            .uri = uri,
            .range = parse_range(j.at("targetRange")),
        };
    }
    const std::string uri = j.at("uri").get<std::string>();
    return Location{
        .path = path_from_file_uri(uri),
        .uri = uri,
        .range = parse_range(j.at("range")),
    };
}


std::vector<Location> parse_locations(const json &j) 
{
    std::vector<Location> out;

    if (j.is_null()) {
        return out;
    }

    if (j.is_object()) {
        out.push_back(parse_location_object(j));
        return out;
    }

    if (j.is_array()) {
        for (const auto &item : j) {
            out.push_back(parse_location_object(item));
        }
    }

    return out;
}



CallHierarchyItem parse_call_hierarchy_item(const json &j) 
{
    const std::string uri = j.value("uri", "");

    CallHierarchyItem item;
    item.name = j.value("name", "");
    item.detail = j.value("detail", "");
    item.kind = parse_symbol_kind(j.value("kind", 6));
    item.uri = uri;
    if (!uri.empty()) {
        item.path = path_from_file_uri(uri);
    }

    if (j.contains("range")) {
        item.range = parse_range(j.at("range"));
    }
    if (j.contains("selectionRange")) {
        item.selection_range = parse_range(j.at("selectionRange"));
    } else {
        item.selection_range = item.range;
    }
    if (j.contains("data")) {
        item.data_json = j.at("data").dump();
    }

    return item;
}


std::vector<CallHierarchyItem> parse_call_hierarchy_items(const json &j) 
{
    std::vector<CallHierarchyItem> out;
    if (j.is_null() || !j.is_array()) {
        return out;
    }
    for (const auto &item : j) {
        out.push_back(parse_call_hierarchy_item(item));
    }

    return out;
}


std::vector<OutgoingCall> parse_outgoing_calls(const json &j) 
{
    std::vector<OutgoingCall> out;
    if (j.is_null() || !j.is_array()) {
        return out;
    }

    for (const auto &item : j) {
        OutgoingCall call;
        call.to = parse_call_hierarchy_item(item.at("to"));

        if (item.contains("fromRanges") && item.at("fromRanges").is_array()) {
            for (const auto &r : item.at("fromRanges")) {
                call.from_ranges.push_back(parse_range(r));
            }
        }

        out.push_back(std::move(call));
    }

    return out;
}


std::vector<IncomingCall> parse_incoming_calls(const json &j) 
{
    std::vector<IncomingCall> out;
    if (j.is_null() || !j.is_array()) {
        return out;
    }

    for (const auto &item : j) {
        IncomingCall call;
        call.from = parse_call_hierarchy_item(item.at("from"));

        if (item.contains("fromRanges") && item.at("fromRanges").is_array()) {
            for (const auto &r : item.at("fromRanges")) {
                call.from_ranges.push_back(parse_range(r));
            }
        }

        out.push_back(std::move(call));
    }

    return out;
}


json json_call_hierarchy_item(const CallHierarchyItem &item) 
{
    json j{
        {"name", item.name},
        {"kind", static_cast<int>(item.kind)},
        {"uri", item.uri},
        {"range", json_range(item.range)},
        {"selectionRange", json_range(item.selection_range)},
    };

    if (!item.detail.empty()) {
        j["detail"] = item.detail;
    }

    if (item.data_json.has_value()) {
        j["data"] = json::parse(*item.data_json);
    }

    return j;
}



}  // namespace



struct Session::Impl 
{

    struct OpenDocument 
    {
        std::filesystem::path path;
        std::string uri;
        std::string text;
        std::string language_id;
        int version{0};
    };

    Impl(const Impl &) = delete;
    Impl &operator=(const Impl &) = delete;
    Impl(Impl &&) = delete;
    Impl &operator=(Impl &&) = delete;

    SessionOptions options;
    pcr::proc::PipedChild child;
    pcr::channel::AnyStream io;
    pcr::rpc::Dispatcher rpc;
    std::unordered_map<std::string, OpenDocument> docs_by_uri;

    Impl(pcr::proc::PipedChild c, SessionOptions opt)
        : child(std::move(c)),
          options(std::move(opt)),
          io(pcr::channel::PipeDuplex(
                child.stdout_read_fd(),
                child.stdin_write_fd(),
                pcr::channel::FdOwnership::Borrowed,
                pcr::channel::FdOwnership::Borrowed)),
          rpc(pcr::rpc::Peer(
                pcr::framing::AnyFramer{pcr::framing::ContentLengthFramer(io)},
                pcr::rpc::AnyCodec{pcr::rpc::NlohmannCodec{}})) {}
};


Session::Session(pcr::proc::PipedChild child, SessionOptions options)
    : impl_(std::make_unique<Impl>(std::move(child), std::move(options))) 
{

    impl_->rpc.on_notification("window/logMessage", [this](const pcr::rpc::Notification &notif) {
        if (!impl_->options.trace_lsp_messages) {
            return;
        }
        std::string line = "[lsp notify] window/logMessage";
        if (notif.params_json) {
            try {
                const auto params = nlohmann::json::parse(*notif.params_json);
                if (params.is_object()) {
                    const int type = params.value("type", 0);
                    const std::string message = params.value("message", "");
                    line += " type=" + std::to_string(type) + " msg=" + message;
                } else {
                    line += " params=" + summarize_json_for_log(params.dump());
                }
            } catch (...) {
                line += " params=<parse-failed>";
            }
        }

        trace_line(true, line);
    });

    impl_->rpc.on_notification("window/showMessage", [this](const pcr::rpc::Notification &notif) {
        if (!impl_->options.trace_lsp_messages) {
            return;
        }

        std::string line = "[lsp notify] window/showMessage";
        if (notif.params_json) {
            try {
                const auto params = nlohmann::json::parse(*notif.params_json);
                line += " params=" + summarize_json_for_log(params.dump());
            } catch (...) {
                line += " params=<parse-failed>";
            }
        }
        trace_line(true, line);
    });

    impl_->rpc.on_notification("$/progress", [this](const pcr::rpc::Notification &notif) {
        if (!impl_->options.trace_lsp_messages) {
            return;
        }
        std::string line = "[lsp notify] $/progress";
        if (notif.params_json) {
            try {
                const auto params = nlohmann::json::parse(*notif.params_json);
                line += " params=" + summarize_json_for_log(params.dump());
            } catch (...) {
                line += " params=<parse-failed>";
            }
        }
        trace_line(true, line);
    });

    impl_->rpc.on_notification("telemetry/event", [this](const pcr::rpc::Notification &notif) {
        if (!impl_->options.trace_lsp_messages) {
            return;
        }
        std::string line = "[lsp notify] telemetry/event";
        if (notif.params_json) {
            try {
                const auto params = nlohmann::json::parse(*notif.params_json);
                line += " params=" + summarize_json_for_log(params.dump());
            } catch (...) {
                line += " params=<parse-failed>";
            }
        }
        trace_line(true, line);
    });

    impl_->rpc.on_request("window/workDoneProgress/create", [this](const pcr::rpc::Request &req) {
        if (impl_->options.trace_lsp_messages) {
            std::string line = "[lsp request] window/workDoneProgress/create";
            if (req.params_json) {
                line += " params=" + summarize_json_for_log(*req.params_json);
            }
            trace_line(true, line);
        }
        return pcr::rpc::HandlerResult::ok("null");
    });

    impl_->rpc.on_request("client/registerCapability", [this](const pcr::rpc::Request &req) {
        if (impl_->options.trace_lsp_messages) {
            std::string line = "[lsp request] client/registerCapability";
            if (req.params_json) {
                line += " params=" + summarize_json_for_log(*req.params_json);
            }
            trace_line(true, line);
        }

        return pcr::rpc::HandlerResult::ok("null");
    });

    impl_->rpc.on_request("client/unregisterCapability", [this](const pcr::rpc::Request &req) {
        if (impl_->options.trace_lsp_messages) {
            std::string line = "[lsp request] client/unregisterCapability";
            if (req.params_json) {
                line += " params=" + summarize_json_for_log(*req.params_json);
            }
            trace_line(true, line);
        }

        return pcr::rpc::HandlerResult::ok("null");
    });

    impl_->rpc.on_request("workspace/workspaceFolders", [this](const pcr::rpc::Request &req) {
        if (impl_->options.trace_lsp_messages) {
            std::string line = "[lsp request] workspace/workspaceFolders";
            if (req.params_json) {
                line += " params=" + summarize_json_for_log(*req.params_json);
            }
            trace_line(true, line);
        }

        nlohmann::json folders = nlohmann::json::array({
            {
                {"uri", file_uri_from_path(impl_->options.root_dir)},
                {"name", impl_->options.root_dir.filename().string()},
            }
        });
        return pcr::rpc::HandlerResult::ok(folders.dump());
    });

    impl_->rpc.on_request("workspace/configuration", [this](const pcr::rpc::Request &req) {
        if (impl_->options.trace_lsp_messages) {
            std::string line = "[lsp request] workspace/configuration";
            if (req.params_json) {
                line += " params=" + summarize_json_for_log(*req.params_json);
            }
            trace_line(true, line);
        }

        nlohmann::json result = nlohmann::json::array();

        try {
            if (!req.params_json) {
                return pcr::rpc::HandlerResult::ok(result.dump());
            }

            const auto params = nlohmann::json::parse(*req.params_json);
            if (params.contains("items") && params.at("items").is_array()) {
                for (const auto &item : params.at("items")) {
                    (void)item;
                    result.push_back(nullptr);
                }
            }
        } catch (...) {
            // best effort
        }

        return pcr::rpc::HandlerResult::ok(result.dump());
    });
}

Session::Session(Session&&) noexcept = default;
Session &Session::operator=(Session&&) noexcept = default;
Session::~Session() = default;


// wrap request in timing measures
// returns raw response in raw json
std::string Session::request_json_raw(std::string_view method,
                                      std::string params_json,
                                      const char *error_prefix) 
{
    const auto t0 = std::chrono::steady_clock::now();

    trace_line(impl_->options.trace_request_timing,
               "[session] -> " + std::string(method) +
               " params=" + summarize_json_for_log(params_json));

    const pcr::rpc::Id id =
        impl_->rpc.send_request(std::string(method), params_json);

    for (;;) {
        if (auto response = impl_->rpc.take_response(id); response.has_value()) {
            const auto elapsed_ms =
                std::chrono::duration_cast<std::chrono::milliseconds>(
                    std::chrono::steady_clock::now() - t0).count();

            if (response->error) {
                trace_line(impl_->options.trace_request_timing,
                           "[session] <- " + std::string(method) +
                           " error after " + std::to_string(elapsed_ms) +
                           "ms msg=" + response->error->message);

                throw std::runtime_error(std::string(error_prefix) + ": " +
                                         response->error->message);
            }

            const std::string result_text =
                response->result_json ? *response->result_json : "null";

            trace_line(impl_->options.trace_request_timing,
                       "[session] <- " + std::string(method) +
                       " ok after " + std::to_string(elapsed_ms) +
                       "ms result=" + summarize_json_for_log(result_text));

            return result_text;
        }

        if (!impl_->rpc.pump_once()) {
            const auto elapsed_ms =
                std::chrono::duration_cast<std::chrono::milliseconds>(
                    std::chrono::steady_clock::now() - t0).count();

            trace_line(impl_->options.trace_request_timing,
                       "[session] <- " + std::string(method) +
                       " EOF after " + std::to_string(elapsed_ms) + "ms");

            throw std::runtime_error(std::string(error_prefix) +
                                     ": LSP EOF while waiting for " +
                                     std::string(method) + " response");
        }
    }
}



InitializeResult Session::initialize() 
{
    using nlohmann::json;

    json caps = json::object();
    caps["workspace"]["workspaceFolders"] = true;
    caps["textDocument"]["definition"]["linkSupport"] = true;
    caps["textDocument"]["documentSymbol"]["hierarchicalDocumentSymbolSupport"] = true;
    caps["textDocument"]["callHierarchy"]["dynamicRegistration"] = false;

    json params;
    params["processId"] = static_cast<int>(::getpid());
    params["clientInfo"] = {
        {"name", impl_->options.client_name},
        {"version", impl_->options.client_version},
    };
    params["rootUri"] = file_uri_from_path(impl_->options.root_dir);
    params["capabilities"] = caps;
    params["workspaceFolders"] = json::array({
        {
            {"uri", file_uri_from_path(impl_->options.root_dir)},
            {"name", impl_->options.root_dir.filename().string()},
        }
    });
    params["trace"] = "off";

    const std::string result_text =
        request_json_raw("initialize", params.dump(), "initialize failed");

    const auto result_json = nlohmann::json::parse(result_text);

    InitializeResult result;

    if (result_json.contains("serverInfo") && result_json.at("serverInfo").is_object()) {
        result.server_name = result_json.at("serverInfo").value("name", "");
        result.server_version = result_json.at("serverInfo").value("version", "");
    }

    const nlohmann::json capabilities = result_json.value("capabilities", nlohmann::json::object());
    result.has_definition_provider = has_provider(capabilities, "definitionProvider");
    result.has_implementation_provider = has_provider(capabilities, "implementationProvider");
    result.has_references_provider = has_provider(capabilities, "referencesProvider");
    result.has_hover_provider = has_provider(capabilities, "hoverProvider");
    result.has_document_symbol_provider = has_provider(capabilities, "documentSymbolProvider");
    result.has_call_hierarchy_provider = has_provider(capabilities, "callHierarchyProvider");

    return result;
}


// three-way handshake
void Session::initialized() 
{
    impl_->rpc.send_notification("initialized", "{}");
}


void Session::shutdown_and_exit() 
{

    // best effort during teardown
    try {
        (void)request_json_raw("shutdown", "null", "shutdown failed");
    } catch (const std::exception &ex) {
        trace_line(impl_->options.trace_request_timing,
                   std::string("[session] shutdown best-effort ignore: ") + ex.what());
    } catch (...) {
        trace_line(impl_->options.trace_request_timing,
                   "[session] shutdown best-effort ignore: unknown exception");
    }
    impl_->rpc.send_notification("exit", "null");
    impl_->child.close_stdin_write();
}


void Session::wait() 
{
    impl_->child.wait();
}


void Session::ensure_query_document_available(const std::filesystem::path &path) 
{
    const auto abs = std::filesystem::absolute(path).lexically_normal();
    const std::string uri = file_uri_from_path(abs);

    if (impl_->docs_by_uri.find(uri) == impl_->docs_by_uri.end()) {
        sync_disk_file(abs);
    }
}


int Session::sync_disk_file(const std::filesystem::path &path) 
{
    return sync_text(path, read_file_text(path), infer_language_id(path));
}


int Session::sync_text(const std::filesystem::path &path,
                       std::string text,
                       std::string language_id) 
{
    const auto abs = std::filesystem::absolute(path).lexically_normal();
    const std::string uri = file_uri_from_path(abs);

    auto it = impl_->docs_by_uri.find(uri);
    if (it == impl_->docs_by_uri.end()) {
        Impl::OpenDocument doc{
            .path = abs,
            .uri = uri,
            .text = std::move(text), 
            .language_id = std::move(language_id),
            .version = 1,
        };

        json params{
            {"textDocument", {
                {"uri", doc.uri},
                {"languageId", doc.language_id},
                {"version", doc.version},
                {"text", doc.text},
            }}
        };

        impl_->rpc.send_notification("textDocument/didOpen", params.dump());
        impl_->docs_by_uri.emplace(uri, std::move(doc));
        return 1;
    }

    if (it->second.text == text) {
        return it->second.version;
    }

    it->second.text = std::move(text);
    ++it->second.version;

    json params{
        {"textDocument", {
            {"uri", it->second.uri},
            {"version", it->second.version},
        }},
        {"contentChanges", json::array({
            {
                {"text", it->second.text},
            }
        })}
    };

    impl_->rpc.send_notification("textDocument/didChange", params.dump());
    return it->second.version;
}


void Session::close_file(const std::filesystem::path &path) 
{
    const auto abs = std::filesystem::absolute(path).lexically_normal();
    const std::string uri = file_uri_from_path(abs);

    auto it = impl_->docs_by_uri.find(uri);
    if (it == impl_->docs_by_uri.end()) {
        return;
    }

    json params{
        {"textDocument", {
            {"uri", uri},
        }}
    };

    impl_->rpc.send_notification("textDocument/didClose", params.dump());
    impl_->docs_by_uri.erase(it);
}


std::vector<DocumentSymbol> Session::document_symbols(const std::filesystem::path &path) 
{
    const auto abs = std::filesystem::absolute(path).lexically_normal();
    ensure_query_document_available(abs);

    json params{
        {"textDocument", {
            {"uri", file_uri_from_path(abs)},
        }}
    };

    const std::string result_text =
        request_json_raw("textDocument/documentSymbol",
                         params.dump(),
                         "documentSymbol failed");

    const auto result_json = nlohmann::json::parse(result_text);
    return parse_document_symbols(result_json);
}


std::vector<WorkspaceSymbol> Session::workspace_symbols(std::string query) 
{
    nlohmann::json params{
        {"query", query},
    };

    const std::string result_text =
        request_json_raw("workspace/symbol",
                         params.dump(),
                         "workspace/symbol failed");

    const auto result_json = nlohmann::json::parse(result_text);
    return parse_workspace_symbols(result_json);
}


std::vector<Location> Session::definition(const std::filesystem::path &path, Position pos) 
{
    const auto abs = std::filesystem::absolute(path).lexically_normal();
    ensure_query_document_available(abs);

    json params{
        {"textDocument", {
            {"uri", file_uri_from_path(abs)},
        }},
        {"position", json_position(pos)},
    };

    const std::string result_text =
        request_json_raw("textDocument/definition",
                         params.dump(),
                         "definition failed");

    const auto result_json = nlohmann::json::parse(result_text);
    return parse_locations(result_json);
}


std::vector<Location> Session::implementation(const std::filesystem::path &path, Position pos) 
{
    const auto abs = std::filesystem::absolute(path).lexically_normal();
    ensure_query_document_available(abs);

    json params{
        {"textDocument", {
            {"uri", file_uri_from_path(abs)},
        }},
        {"position", json_position(pos)},
    };

    const std::string result_text =
        request_json_raw("textDocument/implementation",
                         params.dump(),
                         "implementation failed");

    const auto result_json = nlohmann::json::parse(result_text);
    return parse_locations(result_json);
}


std::vector<Location> Session::references(const std::filesystem::path &path,
                                          Position pos,
                                          bool include_declaration) 
{
    const auto abs = std::filesystem::absolute(path).lexically_normal();
    ensure_query_document_available(abs);

    json params{
        {"textDocument", {
            {"uri", file_uri_from_path(abs)},
        }},
        {"position", json_position(pos)},
        {"context", {
            {"includeDeclaration", include_declaration},
        }},
    };

    const std::string result_text =
        request_json_raw("textDocument/references",
                         params.dump(),
                         "references failed");

    const auto result_json = nlohmann::json::parse(result_text);
    return parse_locations(result_json);
}


std::vector<CallHierarchyItem> Session::prepare_call_hierarchy(const std::filesystem::path &path,
                                                               Position pos) 
{
    const auto abs = std::filesystem::absolute(path).lexically_normal();
    ensure_query_document_available(abs);

    json params{
        {"textDocument", {
            {"uri", file_uri_from_path(abs)},
        }},
        {"position", json_position(pos)},
    };

    const std::string result_text =
        request_json_raw("textDocument/prepareCallHierarchy",
                         params.dump(),
                         "prepareCallHierarchy failed");

    const auto result_json = nlohmann::json::parse(result_text);
    return parse_call_hierarchy_items(result_json);
}


std::vector<OutgoingCall> Session::outgoing_calls(const CallHierarchyItem &item) 
{
    json params{
        {"item", json_call_hierarchy_item(item)},
    };

    const std::string result_text =
        request_json_raw("callHierarchy/outgoingCalls",
                         params.dump(),
                         "outgoingCalls failed");

    const auto result_json = nlohmann::json::parse(result_text);
    return parse_outgoing_calls(result_json);
}


std::vector<IncomingCall> Session::incoming_calls(const CallHierarchyItem &item) 
{
    json params{
        {"item", json_call_hierarchy_item(item)},
    };

    const std::string result_text =
        request_json_raw("callHierarchy/incomingCalls",
                         params.dump(),
                         "incomingCalls failed");

    const auto result_json = nlohmann::json::parse(result_text);
    return parse_incoming_calls(result_json);
}


}  // namespace clspc



