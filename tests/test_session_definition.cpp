#include "clspc/session.h"
#include "clspc/uri.h"
#include "test_helper.h"

#include <filesystem>
#include <string>
#include <vector>

#include <pcr/proc/piped_child.h>
#include <pcr/proc/proc_spec.h>

namespace fs = std::filesystem;
using namespace clspc;


int main() 
{
    const fs::path root =
        fs::temp_directory_path() / "clspc-test-session-definition";

    std::error_code ec;
    fs::remove_all(root, ec);
    fs::create_directories(root, ec);
    require(!ec, "failed to create temp root");

    const fs::path log_path = root / "server.log";
    const fs::path script = root / "fake_lsp_server.py";
    const fs::path checkout_file = root / "CheckoutService.java";
    const fs::path pricing_file = root / "PricingEngine.java";

    touch_file(checkout_file, R"(class CheckoutService {
    PricingEngine pricingEngine;
    int f() { return pricingEngine.quoteTotal(); }
}
)");

    touch_file(pricing_file, R"(class PricingEngine {
    int quoteTotal() { return 42; }
}
)");

    write_executable_script(script, R"PY(#!/usr/bin/env python3
import json
import sys

log_path = sys.argv[1]
target_uri = sys.argv[2]

def log(obj):
    with open(log_path, "a", encoding="utf-8") as f:
        f.write(json.dumps(obj, separators=(",", ":"), sort_keys=True))
        f.write("\n")

def read_message():
    headers = {}
    while True:
        line = sys.stdin.buffer.readline()
        if not line:
            return None
        if line in (b"\r\n", b"\n"):
            break
        text = line.decode("utf-8").strip()
        if ":" in text:
            k, v = text.split(":", 1)
            headers[k.strip().lower()] = v.strip()

    length = int(headers.get("content-length", "0"))
    if length <= 0:
        return None

    body = sys.stdin.buffer.read(length)
    if not body:
        return None

    return json.loads(body.decode("utf-8"))

def send_message(obj):
    body = json.dumps(obj).encode("utf-8")
    sys.stdout.buffer.write(f"Content-Length: {len(body)}\r\n\r\n".encode("utf-8"))
    sys.stdout.buffer.write(body)
    sys.stdout.buffer.flush()

while True:
    msg = read_message()
    if msg is None:
        break

    method = msg.get("method")

    if method == "initialize":
        send_message({
            "jsonrpc": "2.0",
            "id": msg["id"],
            "result": {
                "serverInfo": {
                    "name": "fake-lsp",
                    "version": "0.1"
                },
                "capabilities": {
                    "definitionProvider": True
                }
            }
        })
    elif method == "textDocument/definition":
        log(msg["params"])
        send_message({
            "jsonrpc": "2.0",
            "id": msg["id"],
            "result": [
                {
                    "uri": target_uri,
                    "range": {
                        "start": {"line": 1, "character": 8},
                        "end":   {"line": 1, "character": 18}
                    }
                }
            ]
        })
    elif method == "shutdown":
        send_message({
            "jsonrpc": "2.0",
            "id": msg["id"],
            "result": None
        })
    elif method == "exit":
        break
    else:
        pass
)PY");

    pcr::proc::ProcessSpec spec;
    spec.exe = script.string();
    spec.args.push_back(log_path.string());
    spec.args.push_back(file_uri_from_path(pricing_file));

    auto child = pcr::proc::PipedChild::spawn(std::move(spec));

    SessionOptions options;
    options.root_dir = root;
    options.client_name = "clspc-test";
    options.client_version = "0.1";

    Session session(std::move(child), options);

    const InitializeResult init = session.initialize();
    require(init.has_definition_provider,
            "expected definitionProvider");

    session.initialized();

    const Position pos{
        .line = 2,
        .character = 31,
    };

    const std::vector<Location> defs =
        session.definition(checkout_file, pos);

    require(defs.size() == 1, "expected exactly one definition location");
    require(defs[0].path == fs::absolute(pricing_file).lexically_normal(),
            "unexpected definition path: " + defs[0].path.string());
    require(defs[0].range.start.line == 1,
            "unexpected definition start line");
    require(defs[0].range.start.character == 8,
            "unexpected definition start character");
    require(defs[0].range.end.line == 1,
            "unexpected definition end line");
    require(defs[0].range.end.character == 18,
            "unexpected definition end character");

    session.shutdown_and_exit();
    session.wait();

    const std::string log = read_file(log_path);

    require(contains(log, "\"uri\":\"" + file_uri_from_path(fs::absolute(checkout_file).lexically_normal()) + "\""),
            "expected definition request URI in server log");
    require(contains(log, "\"line\":2"),
            "expected requested line in server log");
    require(contains(log, "\"character\":31"),
            "expected requested character in server log");

    fs::remove_all(root, ec);

    std::cout << "test_session_definition passed\n";
    return 0;
}
