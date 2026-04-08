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

#define ERR_PARSE -32700
#define ERR_INVAL_REQ -32600
#define ERR_INVAL_PARAM -32602
#define ERR_NO_METHOD -32601

constexpr const char* k_protocol_version = "2025-11-25";

using json = nlohmann::json;

namespace {

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

    json structured{
        {"ok", true},
        {"echoed", echoed},
        {"uppercase", uppercase},
        {"pid", static_cast<int>(::getpid())},
        {"timestamp_utc", now_utc_iso8601()}
    };

    return json{
        {"content", json::array({
            {
                {"type", "text"},
                {"text", "smoke_echo ok: " + echoed}
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
                    smoke_echo_tool_definition()
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

            if (tool_name != "smoke_echo") {
                // per MCP, "tool not found" should be a protocol-level error,
                // not an isError tool result
                send_json(make_error(
                    id,
                    ERR_NO_METHOD,
                    "tool not found",
                    json{{"tool", tool_name}}
                ));
                continue;
            }

            try {
                send_json(make_result(id, smoke_echo_result(arguments)));
            } catch (const std::exception& ex) {
                // tool execution errors should be visible to the model as a tool result
                send_json(make_result(id, json{
                    {"content", json::array({
                        {
                            {"type", "text"},
                            {"text", std::string("smoke_echo failed: ") + ex.what()}
                        }
                    })},
                    {"structuredContent", {
                        {"ok", false},
                        {"error", ex.what()}
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

