/**
 * @file mcp_streamable_http_client.h
 * @brief MCP Streamable HTTP Client implementation
 *
 * This file implements the client-side functionality for the Model Context Protocol
 * using the Streamable HTTP transport (2025-11-25 specification).
 */

#ifndef MCP_STREAMABLE_HTTP_CLIENT_H
#define MCP_STREAMABLE_HTTP_CLIENT_H

#include "mcp_client.h"
#include "mcp_message.h"
#include "mcp_tool.h"
#include "mcp_logger.h"

#include "httplib.h"

#include <string>
#include <map>
#include <vector>
#include <memory>
#include <mutex>
#include <functional>
#include <atomic>
#include <thread>

namespace mcp {

/**
 * @class streamable_http_client
 * @brief Client for connecting to MCP servers via Streamable HTTP transport
 *
 * Uses a single /mcp endpoint for all communication:
 *   POST /mcp  — send JSON-RPC requests, receive inline JSON responses
 *   GET  /mcp  — optional SSE stream for server-initiated notifications
 *   DELETE /mcp — close the session
 *
 * Session is managed via the Mcp-Session-Id header.
 */
class streamable_http_client : public client {
public:
    /**
     * @brief Constructor
     * @param server_url The base URL of the server (e.g., "http://localhost:8080")
     * @param mcp_endpoint The MCP endpoint path (default: "/mcp")
     * @param validate_certificates Whether to validate SSL certificates (default: true)
     * @param ca_cert_path Path to CA certificate file for SSL validation (optional)
     */
    streamable_http_client(const std::string& server_url,
        const std::string& mcp_endpoint = "/mcp",
        bool validate_certificates = true,
        const std::string& ca_cert_path = "");

    /**
     * @brief Destructor — stops SSE stream and closes session
     */
    ~streamable_http_client();

    // client interface implementations

    bool initialize(const std::string& client_name, const std::string& client_version) override;

    bool ping() override;

    void set_capabilities(const json& capabilities) override;

    response send_request(const std::string& method, const json& params = json::object()) override;

    void send_notification(const std::string& method, const json& params = json::object()) override;

    json get_server_capabilities() override;

    json call_tool(const std::string& tool_name, const json& arguments = json::object()) override;

    std::vector<tool> get_tools() override;

    json get_capabilities() override;

    json list_resources(const std::string& cursor = "") override;

    json read_resource(const std::string& resource_uri) override;

    json subscribe_to_resource(const std::string& resource_uri) override;

    json unsubscribe_from_resource(const std::string& resource_uri) override;

    json list_resource_templates(const std::string& cursor = "") override;

    bool is_running() const override;

    // Configuration

    /**
     * @brief Set authentication token
     * @param token The authentication token
     */
    void set_auth_token(const std::string& token);

    /**
     * @brief Set a request header that will be sent with all requests
     * @param key Header name
     * @param value Header value
     */
    void set_header(const std::string& key, const std::string& value);

    /**
     * @brief Set timeout for requests
     * @param timeout_seconds Timeout in seconds
     */
    void set_timeout(int timeout_seconds);

    // SSE stream management

    /**
     * @brief Callback type for server-initiated notifications received via GET SSE stream
     * @param method The notification method
     * @param params The notification parameters
     */
    using notification_callback = std::function<void(const std::string& method, const json& params)>;

    /**
     * @brief Set the handler for server-initiated notifications
     * @param callback Function called when a notification arrives on the GET SSE stream
     */
    void set_notification_handler(notification_callback callback);

    /**
     * @brief Start the GET SSE stream for server-initiated notifications
     * @return True if the stream was started successfully
     */
    bool start_sse_stream();

    /**
     * @brief Stop the GET SSE stream
     */
    void stop_sse_stream();

    /**
     * @brief Get the current session ID
     * @return The session ID, or empty string if not initialized
     */
    std::string get_session_id() const;

private:
    // Initialize HTTP client
    void init_client(const std::string& server_url, bool validate_certificates, const std::string& ca_cert_path);

    // Close session (DELETE /mcp)
    void close_session();

    // Send a JSON-RPC request via POST and return the response
    json send_post(const request& req);

    // Parse SSE events from the GET stream
    bool parse_sse_event(const char* data, size_t length);

    // Server URL
    std::string server_url_;

    // MCP endpoint path
    std::string mcp_endpoint_ = "/mcp";

    // HTTP client for POST requests
    std::unique_ptr<httplib::Client> http_client_;

    // HTTP client for GET /mcp notification stream
    std::unique_ptr<httplib::Client> notification_stream_client_;

    // SSE stream thread
    std::unique_ptr<std::thread> sse_thread_;

    // SSE stream running flag
    std::atomic<bool> sse_running_{false};

    // Session ID (set after initialize)
    std::string session_id_;

    // Authentication token
    std::string auth_token_;

    // Default request headers
    std::map<std::string, std::string> default_headers_;

    // Timeout (seconds)
    int timeout_seconds_ = 30;

    // Client capabilities
    json capabilities_;

    // Server capabilities
    json server_capabilities_;

    // Mutex for thread safety
    mutable std::mutex mutex_;

    // Notification handler for server-initiated notifications
    notification_callback notification_handler_;

    // Session established flag
    bool session_active_ = false;
};

} // namespace mcp

#endif // MCP_STREAMABLE_HTTP_CLIENT_H
