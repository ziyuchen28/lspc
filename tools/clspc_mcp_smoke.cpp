#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <ctime>
#include <iomanip>
#include <iostream>
#include <optional>
#include <sstream>
#include <string>
#include <string_view>

#include <unistd.h>

#include <nlohmann/json.hpp>

#include "clspc/service.h"

#define ERR_PARSE -32700
#define ERR_INVAL_REQ -32600
#define ERR_INVAL_PARAM -32602
#define ERR_NO_METHOD -32601

constexpr const char* k_protocol_version = "2025-11-25";

using json = nlohmann::json;

namespace {

std::string getenv_or(std::string_view name, std::string fallback = {})
{
    if (const char *v = std::getenv(std::string(name).c_str())) {
        return *v ? std::string(v) : fallback;
    }
    return fallback;
}

std::string getenv_required(std::string_view name)
{
    if (const char *v = std::getenv(std::string(name).c_str())) {
        if (*v) {
            return std::string(v);
        }
    }
    throw std::runtime_error("missing required environment variable: " + std::string(name));
}

json initialize_result_json(const clspc::InitializeResult &r)
{
    return json{
        {"serverName", r.server_name},
        {"serverVersion", r.server_version},
        {"hasDefinitionProvider", r.has_definition_provider},
        {"hasImplementationProvider", r.has_implementation_provider},
        {"hasReferencesProvider", r.has_references_provider},
        {"hasHoverProvider", r.has_hover_provider},
        {"hasDocumentSymbolProvider", r.has_document_symbol_provider},
        {"hasCallHierarchyProvider", r.has_call_hierarchy_provider},
        {"hasWorkspaceSymbolProvider", r.has_workspace_symbol_provider}
    };
}

json initialize_probe_response_json(const clspc::service::InitializeProbeResponse &r,
                                    const clspc::jdtls::LaunchOptions &launch)
{
    return json{
        {"ok", true},
        {"root", launch.root_dir.string()},
        {"workspaceDir", launch.workspace_dir.string()},
        {"jdtlsHome", launch.jdtls_home.string()},
        {"javaBin", launch.java_bin},
        {"initialize", initialize_result_json(r.initialize)}
    };
}

bool trace_enabled() 
{
    const char *env = std::getenv("CLSPC_MCP_TRACE");
    if (!env) return false;
    const std::string v(env);
    return !v.empty() && v != "0" && v != "false" && v != "FALSE";
}

void log_line(std::string_view msg) 
{
    if (!trace_enabled()) return;
    std::cerr << "[clspc_mcp_smoke] " << msg << "\n";
    std::cerr.flush();
}

std::string now_utc_iso8601() 
{
    std::time_t t = std::time(nullptr);
    std::tm tm{};
    gmtime_r(&t, &tm);

    std::ostringstream out;
    out << std::put_time(&tm, "%Y-%m-%dT%H:%M:%SZ");
    return out.str();
}


void send_json(const json &msg) 
{
    // MCP stdio transport requires one JSON-RPC message per line.
    std::cout << msg.dump() << "\n";
    std::cout.flush();
}


json make_result(const json &id, json result) 
{
    return json{
        {"jsonrpc", "2.0"},
        {"id", id},
        {"result", std::move(result)},
    };
}


json make_error(const json &id,
                int code,
                std::string message,
                std::optional<json> data = std::nullopt) 
{
    json err{
        {"code", code},
        {"message", std::move(message)},
    };
    if (data.has_value()) {
        err["data"] = std::move(*data);
    }

    return json{
        {"jsonrpc", "2.0"},
        {"id", id},
        {"error", std::move(err)},
    };
}


std::string uppercase_copy(std::string s) {
    std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c) {
        return static_cast<char>(std::toupper(c));
    });
    return s;
}


json smoke_echo_tool_definition() 
{
    return json{
        {"name", "smoke_echo"},
        {"title", "Smoke Echo"},
        {"description", "A tiny smoke-test tool that echoes a message and returns process metadata."},
        {"inputSchema", {
            {"type", "object"},
            {"properties", {
                {"message", {
                    {"type", "string"},
                    {"description", "The message to echo back."}
                }},
                {"uppercase", {
                    {"type", "boolean"},
                    {"description", "If true, uppercase the echoed message."},
                    {"default", false}
                }}
            }},
            {"required", json::array({"message"})}
        }},
        {"outputSchema", {
            {"type", "object"},
            {"properties", {
                {"ok", {{"type", "boolean"}}},
                {"echoed", {{"type", "string"}}},
                {"uppercase", {{"type", "boolean"}}},
                {"pid", {{"type", "integer"}}},
                {"timestamp_utc", {{"type", "string"}}}
            }},
            {"required", json::array({"ok", "echoed", "uppercase", "pid", "timestamp_utc"})}
        }}
    };
}


json java_initialize_probe_tool_definition()
{
    return json{
        {"name", "java_initialize_probe"},
        {"title", "Java Initialize Probe"},
        {"description", "Launch JDTLS, initialize an LSP session, and return the server capabilities."},
        {"inputSchema", {
            {"type", "object"},
            {"properties", {
                {"root", {
                    {"type", "string"},
                    {"description", "Absolute path to the repo root."}
                }},
                {"workspaceDir", {
                    {"type", "string"},
                    {"description", "Absolute path to the persistent JDTLS workspace/data dir."}
                }},
                {"jdtlsHome", {
                    {"type", "string"},
                    {"description", "Optional JDTLS install dir. Defaults to CLSPC_JDTLS_HOME."}
                }},
                {"javaBin", {
                    {"type", "string"},
                    {"description", "Optional java executable. Defaults to CLSPC_JAVA_BIN or 'java'."}
                }}
            }},
            {"required", json::array({"root", "workspaceDir"})}
        }}
    };
}


json smoke_echo_result(const json &arguments) 
{
    if (!arguments.is_object()) {
        throw std::runtime_error("arguments must be an object");
    }

    if (!arguments.contains("message") || !arguments.at("message").is_string()) {
        throw std::runtime_error("argument 'message' is required and must be a string");
    }

    const std::string message = arguments.at("message").get<std::string>();
    const bool uppercase = arguments.value("uppercase", false);

    const std::string echoed = uppercase ? uppercase_copy(message) : message;

    json structured {
        {"ok", true},
        {"echoed", echoed},
        {"uppercase", uppercase},
        {"pid", static_cast<int>(::getpid())},
        {"timestamp_utc", now_utc_iso8601()}
    };

    return json {
        {"content", json::array({
            {
                {"type", "text"},
                {"text", "smoke_echo ok: " + echoed}
            }
        })},
        {"structuredContent", structured}
    };
}


json java_initialize_probe_result(const json &arguments)
{
    if (!arguments.is_object()) {
        throw std::runtime_error("arguments must be an object");
    }

    if (!arguments.contains("root") || !arguments.at("root").is_string()) {
        throw std::runtime_error("argument 'root' is required and must be a string");
    }

    if (!arguments.contains("workspaceDir") || !arguments.at("workspaceDir").is_string()) {
        throw std::runtime_error("argument 'workspaceDir' is required and must be a string");
    }

    clspc::service::InitializeProbeRequest req;
    req.launch.root_dir =
        std::filesystem::absolute(arguments.at("root").get<std::string>()).lexically_normal();
    req.launch.workspace_dir =
        std::filesystem::absolute(arguments.at("workspaceDir").get<std::string>()).lexically_normal();
    req.launch.jdtls_home =
        arguments.contains("jdtlsHome") && arguments.at("jdtlsHome").is_string()
            ? std::filesystem::absolute(arguments.at("jdtlsHome").get<std::string>()).lexically_normal()
            : std::filesystem::absolute(getenv_required("CLSPC_JDTLS_HOME")).lexically_normal();
    req.launch.java_bin =
        arguments.contains("javaBin") && arguments.at("javaBin").is_string()
            ? arguments.at("javaBin").get<std::string>()
            : getenv_or("CLSPC_JAVA_BIN", "java");

    const clspc::service::InitializeProbeResponse resp =
        clspc::service::run_initialize_probe(req);

    const json structured = initialize_probe_response_json(resp, req.launch);

    return json{
        {"content", json::array({
            {
                {"type", "text"},
                {"text", "java_initialize_probe ok: " + resp.initialize.server_name}
            }
        })},
        {"structuredContent", structured}
    };
}


json initialize_result(const json &params) 
{
    std::string negotiated = k_protocol_version;
    if (params.is_object() && params.contains("protocolVersion") && params.at("protocolVersion").is_string()) {
        const std::string requested = params.at("protocolVersion").get<std::string>();
        if (requested == k_protocol_version) {
            negotiated = requested;
        }
    }

    return json{
        {"protocolVersion", negotiated},
        {"capabilities", {
            {"tools", json::object()}
        }},
        {"serverInfo", {
            {"name", "clspc-mcp-smoke"},
            {"version", "0.1.0"}
        }},
        {"instructions", "Use smoke_echo for simple end-to-end MCP verification."}
    };
}

} // namespace


int main() 
{
    log_line("server starting");

    std::string line;
    while (std::getline(std::cin, line)) {
        if (line.empty()) {
            continue;
        }

        log_line("rx: " + line);

        json msg;
        try {
            msg = json::parse(line);
        } catch (const std::exception &ex) {
            send_json(make_error(nullptr, ERR_PARSE, std::string("parse error: ") + ex.what()));
            continue;
        }

        if (!msg.is_object() || msg.value("jsonrpc", "") != "2.0") {
            send_json(make_error(msg.value("id", nullptr), ERR_INVAL_REQ, "invalid request"));
            continue;
        }

        const std::string method = msg.value("method", "");
        const json id = msg.contains("id") ? msg.at("id") : json(nullptr);
        const bool is_request = msg.contains("id");
        const json params = msg.value("params", json::object());

        if (method == "initialize") {
            if (!is_request) {
                continue;
            }
            send_json(make_result(id, initialize_result(params)));
            continue;
        }

        if (method == "notifications/initialized") {
            log_line("client initialized");
            continue;
        }

        if (method == "ping") {
            if (!is_request) {
                continue;
            }
            send_json(make_result(id, json::object()));
            continue;
        }

        if (method == "tools/list") {
            if (!is_request) {
                continue;
            }
            send_json(make_result(id, json{
                {"tools", json::array({
                    smoke_echo_tool_definition(),
                    java_initialize_probe_tool_definition()
                })}
            }));
            continue;
        }

        if (method == "tools/call") {
            if (!is_request) {
                continue;
            }

            if (!params.is_object()) {
                send_json(make_error(id, ERR_INVAL_PARAM, "invalid params"));
                continue;
            }

            const std::string tool_name = params.value("name", "");
            const json arguments = params.value("arguments", json::object());

            try {
                if (tool_name == "smoke_echo") {
                    send_json(make_result(id, smoke_echo_result(arguments)));
                    continue;
                }

                if (tool_name == "java_initialize_probe") {
                    send_json(make_result(id, java_initialize_probe_result(arguments)));
                    continue;
                }

                // Per MCP, unknown tool should be a protocol-level error,
                // not an isError tool result.
                send_json(make_error(
                    id,
                    ERR_NO_METHOD,
                    "tool not found",
                    json{{"tool", tool_name}}
                ));
            } catch (const std::exception &ex) {
                send_json(make_result(id, json{
                    {"content", json::array({
                        {
                            {"type", "text"},
                            {"text", tool_name + " failed: " + std::string(ex.what())}
                        }
                    })},
                    {"structuredContent", {
                        {"ok", false},
                        {"error", ex.what()},
                        {"tool", tool_name}
                    }},
                    {"isError", true}
                }));
            }

            continue;
        }

        if (!is_request) {
            // Unknown notification: ignore.
            continue;
        }

        send_json(make_error(id, -32601, "method not found"));
    }

    log_line("stdin closed, exiting");
    return 0;
}

