#include "clspc/session.h"

#include "clspc/uri.h"

#include <stdexcept>
#include <utility>
#include <fstream>
#include <sstream>
#include <stdexcept>

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
    // optional handler
    impl_->rpc.on_notification("window/logMessage", [](const pcr::rpc::Notification&) {});
    impl_->rpc.on_notification("window/showMessage", [](const pcr::rpc::Notification&) {});
    impl_->rpc.on_notification("$/progress", [](const pcr::rpc::Notification&) {});
    impl_->rpc.on_notification("telemetry/event", [](const pcr::rpc::Notification&) {});

    impl_->rpc.on_request("window/workDoneProgress/create", [](const pcr::rpc::Request&) {
        return pcr::rpc::HandlerResult::ok("null");
    });

    impl_->rpc.on_request("client/registerCapability", [](const pcr::rpc::Request&) {
        return pcr::rpc::HandlerResult::ok("null");
    });

    impl_->rpc.on_request("client/unregisterCapability", [](const pcr::rpc::Request&) {
        return pcr::rpc::HandlerResult::ok("null");
    });

    impl_->rpc.on_request("workspace/workspaceFolders", [this](const pcr::rpc::Request&) {
        json folders = json::array({
            {
                {"uri", file_uri_from_path(impl_->options.root_dir)},
                {"name", impl_->options.root_dir.filename().string()},
            }
        });
        return pcr::rpc::HandlerResult::ok(folders.dump());
    });

    impl_->rpc.on_request("workspace/configuration", [](const pcr::rpc::Request &req) {
        json result = json::array();
        try {
            if (!req.params_json) {
                return pcr::rpc::HandlerResult::ok(result.dump());
            }

            const auto params = json::parse(*req.params_json);
            if (params.contains("items") && params.at("items").is_array()) {
                // no need to handle for now so returning null just for satisfying protocol
                for (const auto &_ : params.at("items")) {
                    (void)_;
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


InitializeResult Session::initialize() 
{
    using nlohmann::json;

    json caps = json::object();
    caps["workspace"]["workspaceFolders"] = true;

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

    const pcr::rpc::Id id = impl_->rpc.send_request("initialize", params.dump());

    for (;;) {
        if (auto response = impl_->rpc.take_response(id); response.has_value()) {
            if (response->error) {
                throw std::runtime_error("initialize failed: " + response->error->message);
            }

            const auto result_json =
                response->result_json ? json::parse(*response->result_json) : json(nullptr);

            InitializeResult result;

            if (result_json.contains("serverInfo") && result_json.at("serverInfo").is_object()) {
                result.server_name = result_json.at("serverInfo").value("name", "");
                result.server_version = result_json.at("serverInfo").value("version", "");
            }

            const json capabilities = result_json.value("capabilities", json::object());
            result.has_definition_provider = has_provider(capabilities, "definitionProvider");
            result.has_references_provider = has_provider(capabilities, "referencesProvider");
            result.has_hover_provider = has_provider(capabilities, "hoverProvider");
            result.has_document_symbol_provider = has_provider(capabilities, "documentSymbolProvider");
            result.has_call_hierarchy_provider = has_provider(capabilities, "callHierarchyProvider");

            return result;
        }

        if (!impl_->rpc.pump_once()) {
            throw std::runtime_error("LSP EOF while waiting for initialize response");
        }
    }
}

// three-way handshake
void Session::initialized() 
{
    impl_->rpc.send_notification("initialized", "{}");
}


void Session::shutdown_and_exit() 
{
    const pcr::rpc::Id id = impl_->rpc.send_request("shutdown", "null");

    for (;;) {
        if (auto response = impl_->rpc.take_response(id); response.has_value()) {
            break;
        }
        if (!impl_->rpc.pump_once()) {
            break;
        }
    }

    impl_->rpc.send_notification("exit", "null");
    impl_->child.close_stdin_write();
}


void Session::wait() 
{
    impl_->child.wait();
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



}  // namespace clspc



