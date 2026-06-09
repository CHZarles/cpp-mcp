/**
 * @file streamable_http_test.cpp
 * @brief Integration test for the Streamable HTTP client
 */

#include "mcp_streamable_http_client.h"
#include "mcp_server.h"
#include "mcp_tool.h"

#include <iostream>
#include <thread>
#include <chrono>
#include <atomic>
#include <cassert>

// Tool handlers
mcp::json echo_handler(const mcp::json& params, const std::string&) {
    std::string text = params.value("text", "");
    return {{{"type", "text"}, {"text", text}}};
}

mcp::json calc_handler(const mcp::json& params, const std::string&) {
    std::string op = params.value("operation", "");
    double a = params.value("a", 0.0);
    double b = params.value("b", 0.0);
    double result = 0.0;
    if (op == "add") result = a + b;
    else if (op == "multiply") result = a * b;
    return {{{"type", "text"}, {"text", std::to_string(result)}}};
}

static int passed = 0;
static int failed = 0;

#define TEST_ASSERT(cond, msg) \
    do { \
        if (cond) { \
            std::cout << "  PASS: " << msg << std::endl; \
            passed++; \
        } else { \
            std::cerr << "  FAIL: " << msg << " (line " << __LINE__ << ")" << std::endl; \
            failed++; \
        } \
    } while(0)

int main() {
    const int port = 19090;
    const std::string url = "http://localhost:" + std::to_string(port);

    // Configure and start server
    mcp::server::configuration conf;
    conf.host = "localhost";
    conf.port = port;
    conf.session_timeout = 0; // disable timeout for test

    mcp::server server(conf);
    server.set_server_info("TestServer", "1.0.0");
    server.set_capabilities({{"tools", mcp::json::object()}});

    mcp::tool echo_tool = mcp::tool_builder("echo")
        .with_description("Echo input")
        .with_string_param("text", "Text to echo")
        .build();

    mcp::tool calc_tool = mcp::tool_builder("calculator")
        .with_description("Calculator")
        .with_string_param("operation", "Operation")
        .with_number_param("a", "First operand")
        .with_number_param("b", "Second operand")
        .build();

    server.register_tool(echo_tool, echo_handler);
    server.register_tool(calc_tool, calc_handler);

    std::cout << "Starting server on port " << port << "..." << std::endl;
    server.start(false); // non-blocking
    std::this_thread::sleep_for(std::chrono::milliseconds(500));

    if (!server.is_running()) {
        std::cerr << "FATAL: Server failed to start" << std::endl;
        return 1;
    }
    std::cout << "Server started." << std::endl;

    int exit_code = 0;

    try {
        // === Test 1: Initialize ===
        std::cout << "\n[Test 1] Initialize" << std::endl;
        mcp::streamable_http_client client(url, "/mcp");
        client.set_timeout(5);

        bool init_ok = client.initialize("TestClient", "1.0.0");
        TEST_ASSERT(init_ok, "initialize() returned true");
        TEST_ASSERT(!client.get_session_id().empty(), "session ID is not empty");
        std::cout << "  Session ID: " << client.get_session_id() << std::endl;

        // === Test 2: Ping ===
        std::cout << "\n[Test 2] Ping" << std::endl;
        bool ping_ok = client.ping();
        TEST_ASSERT(ping_ok, "ping() returned true");

        // === Test 3: Get server capabilities ===
        std::cout << "\n[Test 3] Server capabilities" << std::endl;
        auto caps = client.get_server_capabilities();
        TEST_ASSERT(caps.contains("tools"), "capabilities contains 'tools'");
        std::cout << "  Capabilities: " << caps.dump(2) << std::endl;

        // === Test 4: List tools ===
        std::cout << "\n[Test 4] List tools" << std::endl;
        auto tools = client.get_tools();
        TEST_ASSERT(tools.size() == 2, "got 2 tools (got " + std::to_string(tools.size()) + ")");
        for (const auto& t : tools) {
            std::cout << "  Tool: " << t.name << " - " << t.description << std::endl;
        }

        // === Test 5: Call echo tool ===
        std::cout << "\n[Test 5] Call echo tool" << std::endl;
        mcp::json echo_result = client.call_tool("echo", {{"text", "hello streamable"}});
        TEST_ASSERT(echo_result.contains("content"), "echo result has 'content'");
        std::string echo_text = echo_result["content"][0]["text"].get<std::string>();
        TEST_ASSERT(echo_text == "hello streamable", "echo text matches (got: " + echo_text + ")");

        // === Test 6: Call calculator tool ===
        std::cout << "\n[Test 6] Call calculator tool" << std::endl;
        mcp::json calc_result = client.call_tool("calculator", {
            {"operation", "add"}, {"a", 10}, {"b", 32}
        });
        TEST_ASSERT(calc_result.contains("content"), "calc result has 'content'");
        std::string calc_text = calc_result["content"][0]["text"].get<std::string>();
        TEST_ASSERT(calc_text == "42.000000", "calc result is 42 (got: " + calc_text + ")");

        // === Test 7: GET SSE stream ===
        std::cout << "\n[Test 7] Start/stop GET SSE stream" << std::endl;
        bool stream_started = client.start_sse_stream();
        TEST_ASSERT(stream_started, "start_sse_stream() returned true");
        std::this_thread::sleep_for(std::chrono::milliseconds(500));
        TEST_ASSERT(client.is_running(), "client is still running during SSE stream");
        client.stop_sse_stream();
        TEST_ASSERT(true, "stop_sse_stream() completed without crash");

        // === Test 8: Multiple requests ===
        std::cout << "\n[Test 8] Multiple sequential requests" << std::endl;
        for (int i = 0; i < 5; i++) {
            mcp::json r = client.call_tool("echo", {{"text", "request " + std::to_string(i)}});
            std::string t = r["content"][0]["text"].get<std::string>();
            if (t != "request " + std::to_string(i)) {
                std::cerr << "  FAIL: request " << i << " got: " << t << std::endl;
                failed++;
            } else {
                passed++;
            }
        }
        std::cout << "  PASS: 5 sequential requests completed" << std::endl;

        // Destructor will call stop_sse_stream + close_session

    } catch (const std::exception& e) {
        std::cerr << "EXCEPTION: " << e.what() << std::endl;
        failed++;
    }

    // Summary
    std::cout << "\n========================================" << std::endl;
    std::cout << "Results: " << passed << " passed, " << failed << " failed" << std::endl;
    std::cout << "========================================" << std::endl;

    server.stop();
    return failed > 0 ? 1 : 0;
}
