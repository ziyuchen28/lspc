#include "clspc/session.h"
#include "clspc/uri.h"
#include "test_helper.h"

#include <filesystem>
#include <string>
#include <string_view>
#include <vector>

#include <pcr/proc/piped_child.h>
#include <pcr/proc/proc_spec.h>

namespace fs = std::filesystem;
using namespace clspc;

namespace {

std::size_t count_substring(std::string_view haystack, std::string_view needle) 
{
    std::size_t count = 0;
    std::size_t pos = 0;
    while ((pos = haystack.find(needle, pos)) != std::string_view::npos) {
        ++count;
        pos += needle.size();
    }
    return count;
}

std::string_view logical_name(std::string_view s) 
{
    const auto p = s.find('(');
    return (p == std::string_view::npos) ? s : s.substr(0, p);
}

}  // namespace


int main() 
{
    const fs::path root =
        fs::temp_directory_path() / "clspc-test-session-call-hierarchy";

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
    int finalizeCheckout() { return pricingEngine.quoteTotal(); }
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
checkout_uri = sys.argv[2]
pricing_uri = sys.argv[3]

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
                    "callHierarchyProvider": True
                }
            }
        })
    elif method == "textDocument/prepareCallHierarchy":
        log({
            "method": "textDocument/prepareCallHierarchy",
            "params": msg["params"]
        })
        send_message({
            "jsonrpc": "2.0",
            "id": msg["id"],
            "result": [
                {
                    "name": "finalizeCheckout()",
                    "kind": 6,
                    "uri": checkout_uri,
                    "range": {
                        "start": {"line": 2, "character": 4},
                        "end":   {"line": 2, "character": 20}
                    },
                    "selectionRange": {
                        "start": {"line": 2, "character": 8},
                        "end":   {"line": 2, "character": 24}
                    },
                    "data": "java-internal-id-98765"
                }
            ]
        })
    elif method == "callHierarchy/outgoingCalls":
        log({
            "method": "callHierarchy/outgoingCalls",
            "params": msg["params"]
        })
        send_message({
            "jsonrpc": "2.0",
            "id": msg["id"],
            "result": [
                {
                    "to": {
                        "name": "quoteTotal()",
                        "kind": 6,
                        "uri": pricing_uri,
                        "range": {
                            "start": {"line": 1, "character": 4},
                            "end":   {"line": 1, "character": 14}
                        },
                        "selectionRange": {
                            "start": {"line": 1, "character": 8},
                            "end":   {"line": 1, "character": 18}
                        }
                    },
                    "fromRanges": [
                        {
                            "start": {"line": 2, "character": 43},
                            "end":   {"line": 2, "character": 53}
                        }
                    ]
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
    spec.args.push_back(file_uri_from_path(checkout_file));
    spec.args.push_back(file_uri_from_path(pricing_file));

    auto child = pcr::proc::PipedChild::spawn(std::move(spec));

    SessionOptions options;
    options.root_dir = root;
    options.client_name = "clspc-test";
    options.client_version = "0.1";

    Session session(std::move(child), options);

    const InitializeResult init = session.initialize();
    require(init.has_call_hierarchy_provider,
            "expected callHierarchyProvider");

    session.initialized();

    const Position anchor{
        .line = 2,
        .character = 12,
    };

    const std::vector<CallHierarchyItem> items =
        session.prepare_call_hierarchy(checkout_file, anchor);

    require(items.size() == 1, "expected one call hierarchy item");
    require(logical_name(items[0].name) == "finalizeCheckout",
            "unexpected call hierarchy item name: " + items[0].name);
    require(items[0].path == fs::absolute(checkout_file).lexically_normal(),
            "unexpected call hierarchy item path");

    const std::vector<OutgoingCall> outgoing =
        session.outgoing_calls(items[0]);

    require(outgoing.size() == 1, "expected one outgoing call");
    require(logical_name(outgoing[0].to.name) == "quoteTotal",
            "unexpected outgoing target name: " + outgoing[0].to.name);
    require(outgoing[0].to.path == fs::absolute(pricing_file).lexically_normal(),
            "unexpected outgoing target path");

    require(outgoing[0].from_ranges.size() == 1,
            "expected one fromRange");
    require(outgoing[0].from_ranges[0].start.line == 2,
            "unexpected fromRange start line");
    require(outgoing[0].from_ranges[0].start.character == 43,
            "unexpected fromRange start character");

    session.shutdown_and_exit();
    session.wait();

    const std::string log = read_file(log_path);

    require(contains(log, "\"method\":\"textDocument/prepareCallHierarchy\""),
            "expected prepareCallHierarchy request in log");
    require(contains(log, "\"method\":\"callHierarchy/outgoingCalls\""),
            "expected outgoingCalls request in log");

    require(contains(log, "\"data\":\"java-internal-id-98765\""),
            "expected outgoingCalls request to preserve call hierarchy item data");

    require(contains(log, "\"uri\":\"" +
                          file_uri_from_path(fs::absolute(checkout_file).lexically_normal()) +
                          "\""),
            "expected checkout URI in call hierarchy request");
    require(contains(log, "\"line\":2"),
            "expected anchor line in call hierarchy request");
    require(contains(log, "\"character\":12"),
            "expected anchor character in call hierarchy request");

    require(contains(log, "\"name\":\"finalizeCheckout()\""),
            "expected outgoingCalls request to contain the selected item name");

    require(count_substring(log, "\"method\":\"textDocument/prepareCallHierarchy\"") == 1,
            "expected exactly one prepareCallHierarchy request");
    require(count_substring(log, "\"method\":\"callHierarchy/outgoingCalls\"") == 1,
            "expected exactly one outgoingCalls request");

    fs::remove_all(root, ec);

    std::cout << "test_session_call_hierarchy passed\n";
    return 0;
}


