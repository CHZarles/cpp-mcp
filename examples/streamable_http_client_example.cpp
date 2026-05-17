/**
 * @file streamable_http_client_example.cpp
 * @brief Example of an MCP Streamable HTTP client
 *
 * This file demonstrates how to create an MCP client that connects to a server
 * using the Streamable HTTP transport (2025-03-26 specification).
 */

#include "mcp_streamable_http_client.h"
#include <iostream>
#include <string>

int main() {
    // Create a Streamable HTTP client
    mcp::streamable_http_client client("http://localhost:8888", "/mcp");

    // Set capabilities
    mcp::json capabilities = {
        {"roots", {{"listChanged", true}}}
    };
    client.set_capabilities(capabilities);

    // Set timeout
    client.set_timeout(10);

    try {
        // Initialize the connection
        std::cout << "Initializing connection to MCP server..." << std::endl;
        bool initialized = client.initialize("StreamableHTTPClient", mcp::MCP_VERSION);

        if (!initialized) {
            std::cerr << "Failed to initialize connection to server" << std::endl;
            return 1;
        }

        std::cout << "Session ID: " << client.get_session_id() << std::endl;

        // Ping the server
        std::cout << "Pinging server..." << std::endl;
        if (!client.ping()) {
            std::cerr << "Failed to ping server" << std::endl;
            return 1;
        }

        // Get server capabilities
        std::cout << "Getting server capabilities..." << std::endl;
        mcp::json server_caps = client.get_server_capabilities();
        std::cout << "Server capabilities: " << server_caps.dump(4) << std::endl;

        // Get available tools
        std::cout << "\nGetting available tools..." << std::endl;
        auto tools = client.get_tools();
        std::cout << "Available tools:" << std::endl;
        for (const auto& tool : tools) {
            std::cout << "- " << tool.name << ": " << tool.description << std::endl;
        }

        // Set up notification handler for server-initiated notifications
        client.set_notification_handler([](const std::string& method, const mcp::json& params) {
            std::cout << "[Notification] Method: " << method
                      << ", Params: " << params.dump() << std::endl;
        });

        // Start GET SSE stream for server-initiated notifications
        std::cout << "\nStarting SSE stream for server notifications..." << std::endl;
        client.start_sse_stream();

        // Call tools
        if (!tools.empty()) {
            // Call the get_time tool
            std::cout << "\nCalling get_time tool..." << std::endl;
            mcp::json time_result = client.call_tool("get_time");
            std::cout << "Current time: " << time_result["content"][0]["text"].get<std::string>() << std::endl;

            // Call the echo tool
            std::cout << "\nCalling echo tool..." << std::endl;
            mcp::json echo_params = {
                {"text", "Hello, Streamable HTTP!"},
                {"uppercase", true}
            };
            mcp::json echo_result = client.call_tool("echo", echo_params);
            std::cout << "Echo result: " << echo_result["content"][0]["text"].get<std::string>() << std::endl;

            // Call the calculator tool
            std::cout << "\nCalling calculator tool..." << std::endl;
            mcp::json calc_params = {
                {"operation", "multiply"},
                {"a", 6},
                {"b", 7}
            };
            mcp::json calc_result = client.call_tool("calculator", calc_params);
            std::cout << "6 * 7 = " << calc_result["content"][0]["text"].get<std::string>() << std::endl;
        }

        // Stop SSE stream
        client.stop_sse_stream();

    } catch (const mcp::mcp_exception& e) {
        std::cerr << "MCP error: " << e.what() << " (code: " << static_cast<int>(e.code()) << ")" << std::endl;
        return 1;
    } catch (const std::exception& e) {
        std::cerr << "Error: " << e.what() << std::endl;
        return 1;
    }

    std::cout << "\nStreamable HTTP client example completed successfully" << std::endl;
    return 0;
}
