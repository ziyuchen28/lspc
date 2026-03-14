#include "clspc/session.h"
#include "test_helper.h"

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>
#include <string_view>

#include <pcr/proc/piped_child.h>
#include <pcr/proc/proc_spec.h>

using namespace clspc;
namespace fs = std::filesystem;

static std::size_t count_substring(std::string_view haystack, std::string_view needle) 
{
    std::size_t count = 0;
    std::size_t pos = 0;
    while ((pos = haystack.find(needle, pos)) != std::string_view::npos) {
        ++count;
        pos += needle.size();
    }
    return count;
}



int main() 
{
    const fs::path root =
        fs::temp_directory_path() / "clspc-test-session-document-sync";

    std::error_code ec;
    fs::remove_all(root, ec);
    fs::create_directories(root, ec);
    require(!ec, "failed to create temp root");

    const fs::path log_path = root / "server.log";
    const fs::path script = root / "fake_lsp_server.py";
    const fs::path java_file = root / "Example.java";

    {
        std::ofstream out(java_file);
        require(static_cast<bool>(out), "failed to create java file");
        out << "class A {}\n";
    }

    write_executable_script(script, R"(#!/usr/bin/env python3
import json
import sys

log_path = sys.argv[1]

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
                "capabilities": {}
            }
        })
    elif method == "shutdown":
        send_message({
            "jsonrpc": "2.0",
            "id": msg["id"],
            "result": None
        })
    elif method == "initialized":
        log({"method":"initialized"})
    elif method == "textDocument/didOpen":
        td = msg["params"]["textDocument"]
        log({
            "method":"textDocument/didOpen",
            "uri":td["uri"],
            "languageId":td["languageId"],
            "version":td["version"],
            "text":td["text"]
        })
    elif method == "textDocument/didChange":
        td = msg["params"]["textDocument"]
        ch = msg["params"]["contentChanges"][0]
        log({
            "method":"textDocument/didChange",
            "uri":td["uri"],
            "version":td["version"],
            "text":ch["text"]
        })
    elif method == "textDocument/didClose":
        td = msg["params"]["textDocument"]
        log({
            "method":"textDocument/didClose",
            "uri":td["uri"]
        })
    elif method == "exit":
        log({"method":"exit"})
        break
)");

    pcr::proc::ProcessSpec spec;
    spec.exe = script.string();
    spec.args.push_back(log_path.string());

    auto child = pcr::proc::PipedChild::spawn(std::move(spec));

    SessionOptions options;
    options.root_dir = root;
    options.client_name = "clspc-test";
    options.client_version = "0.1";

    Session session(std::move(child), options);

    const InitializeResult init = session.initialize();
    require(init.server_name == "fake-lsp",
            "unexpected server_name: " + init.server_name);

    session.initialized();

    const int v1 = session.sync_disk_file(java_file);
    require(v1 == 1, "expected first sync version 1");

    const int v2 = session.sync_text(java_file, "class A { int x; }\n", "java");
    require(v2 == 2, "expected second sync version 2");

    const int v3 = session.sync_text(java_file, "class A { int x; }\n", "java");
    require(v3 == 2, "expected no-op sync to keep version 2");

    session.close_file(java_file);
    session.shutdown_and_exit();
    session.wait();

    const std::string log = read_file(log_path);

    require(contains(log, "\"method\":\"initialized\""),
            "expected initialized notification");
    require(contains(log, "\"method\":\"textDocument/didOpen\""),
            "expected didOpen notification");
    require(contains(log, "\"languageId\":\"java\""),
            "expected java language id");
    require(contains(log, "\"version\":1"),
            "expected open version 1");
    require(contains(log, "\"text\":\"class A {}\\n\""),
            "expected original file text in didOpen");

    require(contains(log, "\"method\":\"textDocument/didChange\""),
            "expected didChange notification");
    require(contains(log, "\"version\":2"),
            "expected didChange version 2");
    require(contains(log, "\"text\":\"class A { int x; }\\n\""),
            "expected updated file text in didChange");

    require(count_substring(log, "\"method\":\"textDocument/didChange\"") == 1,
            "expected exactly one didChange (no-op sync should not send a second one)");

    require(contains(log, "\"method\":\"textDocument/didClose\""),
            "expected didClose notification");
    require(contains(log, "\"method\":\"exit\""),
            "expected exit notification");

    fs::remove_all(root, ec);

    std::cout << "test_session_document_sync passed\n";
    return 0;
}
