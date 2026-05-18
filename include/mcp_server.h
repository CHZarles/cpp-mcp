/**
 * @file mcp_server.h
 * @brief MCP Server implementation
 *
 * This file implements the server-side functionality for the Model Context Protocol.
 * Supports the 2025-11-25 Streamable HTTP transport.
 */

#ifndef MCP_SERVER_H
#define MCP_SERVER_H

#include "mcp_message.h"
#include "mcp_prompt.h"
#include "mcp_resource.h"
#include "mcp_tool.h"
#include "mcp_thread_pool.h"
#include "mcp_logger.h"

// Include the HTTP library
#include "httplib.h"

#include <string>
#include <map>
#include <vector>
#include <memory>
#include <mutex>
#include <thread>
#include <functional>
#include <chrono>
#include <condition_variable>
#include <future>
#include <atomic>
#include <optional>
#include <queue>
#include <set>


namespace mcp {

using method_handler = std::function<json(const json&, const std::string&)>;
using tool_handler = method_handler;
using prompt_handler = method_handler;
using notification_handler = std::function<void(const json&, const std::string&)>;
using auth_handler = std::function<bool(const std::string&, const std::string&)>;
using session_cleanup_handler = std::function<void(const std::string&)>;

class event_dispatcher {
public:
    enum class wait_result {
        message,
        timeout,
        closed,
        write_failed
    };

    event_dispatcher() {
    }
    
    ~event_dispatcher() {
        close();
    }

    wait_result wait_event_result(httplib::DataSink* sink, const std::chrono::milliseconds& timeout = std::chrono::milliseconds(10000)) {
        if (!sink || closed_.load(std::memory_order_acquire)) {
            return wait_result::closed;
        }

        std::string message_copy;
        {
            std::unique_lock<std::mutex> lk(m_);

            if (closed_.load(std::memory_order_acquire)) {
                return wait_result::closed;
            }

            bool result = cv_.wait_for(lk, timeout, [&] {
                return !messages_.empty() || closed_.load(std::memory_order_acquire);
            });

            if (closed_.load(std::memory_order_acquire)) {
                return wait_result::closed;
            }

            if (!result) {
                return wait_result::timeout;
            }

            if (messages_.empty()) {
                return wait_result::timeout;
            }

            message_copy = std::move(messages_.front());
            messages_.pop();
        }

        try {
            if (!message_copy.empty()) {
                if (!sink->write(message_copy.data(), message_copy.size())) {
                    close();
                    return wait_result::write_failed;
                }
            }
            return wait_result::message;
        } catch (...) {
            close();
            return wait_result::write_failed;
        }
    }

    bool wait_event(httplib::DataSink* sink, const std::chrono::milliseconds& timeout = std::chrono::milliseconds(10000)) {
        return wait_event_result(sink, timeout) == wait_result::message;
    }

    bool send_event(std::string message) {
        if (closed_.load(std::memory_order_acquire) || message.empty()) {
            return false;
        }
        
        try {
            std::lock_guard<std::mutex> lk(m_);
            
            if (closed_.load(std::memory_order_acquire)) {
                return false;
            }
            
            messages_.push(std::move(message));
            cv_.notify_one();
            return true;
        } catch (...) {
            return false;
        }
    }
    
    void close() {
        bool was_closed = closed_.exchange(true, std::memory_order_release);
        if (was_closed) {
            return;
        }
        
        try {
            cv_.notify_all();
        } catch (...) {
            // Ignore exceptions
        }
    }
    
    bool is_closed() const {
        return closed_.load(std::memory_order_acquire);
    }
    
    // Get the last activity time
    std::chrono::steady_clock::time_point last_activity() const {
        std::lock_guard<std::mutex> lk(m_);
        return last_activity_;
    }
    
    // Update the activity time (when sending or receiving a message)
    void update_activity() {
        std::lock_guard<std::mutex> lk(m_);
        last_activity_ = std::chrono::steady_clock::now();
    }

private:
    mutable std::mutex m_;
    std::condition_variable cv_;
    std::queue<std::string> messages_;
    std::atomic<bool> closed_{false};
    std::chrono::steady_clock::time_point last_activity_{std::chrono::steady_clock::now()};
};

/**
 * @class server
 * @brief Main MCP server class
 * 
 * The server class implements an HTTP server that handles JSON-RPC requests
 * according to the Model Context Protocol specification.
 */
class server {
public:

    /**
     * @struct configuration
     * @brief Configuration settings for the server.
     *
     * This struct holds all configurable parameters for the server, including
     * network bindings, identification, and endpoint paths. If SSL is enabled,
     * it also includes paths to the server certificate and private key.
     */
    struct configuration {
        /** Host to bind to (e.g., "localhost", "0.0.0.0") */
        std::string host{ "localhost" };

        /** Port to listen on */
        int port{ 8080 };

        /** Server name */
        std::string name{ "MCP Server" };

        /** Server version */
        std::string version{ "0.0.1" };

        /** Streamable HTTP endpoint path (2025-11-25 transport) */
        std::string mcp_endpoint{ "/mcp" };

        unsigned int threadpool_size{ std::thread::hardware_concurrency() };

        /** Maximum concurrent sessions (0 = unlimited) */
        unsigned int max_sessions{ MCP_MAX_SESSIONS };

        /** Inactive session timeout in seconds (0 = disabled) */
        unsigned int session_timeout{ MCP_SESSION_TIMEOUT };

        #ifdef MCP_SSL        
        /**
         * @brief SSL configuration settings.
         *
         * Contains optional paths to the server certificate and private key.
         * These are used when SSL support is enabled.
         */
        struct {
            /** Path to the server certificate */
            std::optional<std::string> server_cert_path{ std::nullopt };

            /** Path to the server private key */
            std::optional<std::string> server_private_key_path{ std::nullopt };
        } ssl;
        #endif
    };

    /**
     * @brief Constructor
     * @param conf The server configuration
     */
    server(const server::configuration& conf);   
    
    /**
     * @brief Destructor
     */
    ~server();
    
    /**
     * @brief Start the server
     * @param blocking If true, this call blocks until the server stops
     * @return True if the server started successfully
     */
    bool start(bool blocking = true);
    
    /**
     * @brief Stop the server
     */
    void stop();
    
    /**
     * @brief Check if the server is running
     * @return True if the server is running
     */
    bool is_running() const;
    
    /**
     * @brief Set server information
     * @param name The name of the server
     * @param version The version of the server
     */
    void set_server_info(const std::string& name, const std::string& version);
    
    /**
     * @brief Set server capabilities
     * @param capabilities The capabilities of the server
     */
    void set_capabilities(const json& capabilities);

    /**
     * @brief Set server instructions (returned in initialize response)
     * @param instructions Human-readable instructions for the client
     */
    void set_instructions(const std::string& instructions);

    /**
     * @brief Register a method handler
     * @param method The method name
     * @param handler The function to call when the method is invoked
     */
    void register_method(const std::string& method, method_handler handler);
    
    /**
     * @brief Register a notification handler
     * @param method The notification method name
     * @param handler The function to call when the notification is received
     */
    void register_notification(const std::string& method, notification_handler handler);
    
    /**
     * @brief Register a resource
     * @param path The path to mount the resource at
     * @param resource The resource to register
     */
    void register_resource(const std::string& path, std::shared_ptr<resource> resource);

    /**
     * @brief Register a resource template
     * @param uri_template RFC 6570 URI template (e.g. "myapp://items/{id}")
     * @param name Human-readable name
     * @param mime_type MIME type of the resource
     * @param description Description of the template
     * @param handler Function called with (uri, uriParams, session_id) to produce content
     */
    using resource_template_handler = std::function<json(
        const std::string& uri,
        const std::map<std::string, std::string>& uri_params,
        const std::string& session_id)>;

    void register_resource_template(
        const std::string& uri_template,
        const std::string& name,
        const std::string& mime_type,
        const std::string& description,
        resource_template_handler handler,
        const json& metadata = json::object());

    /**
     * @brief Register a prompt
     * @param prompt_def The prompt metadata to expose through prompts/list
     * @param handler Function called by prompts/get with prompt arguments
     */
    void register_prompt(const prompt& prompt_def, prompt_handler handler);

    /**
     * @brief Register a tool
     * @param tool The tool to register
     * @param handler The function to call when the tool is invoked
     */
    void register_tool(const tool& tool, tool_handler handler);

    /**
     * @brief Register a session cleanup handler
     * @param key Tool or resource name to be cleaned up
     * @param handler The function to call when the session is closed
     */
    void register_session_cleanup(const std::string& key, session_cleanup_handler handler);
    
    /**
     * @brief Get the list of available tools
     * @return JSON array of available tools
     */
    std::vector<tool> get_tools() const;
    
    /**
     * @brief Set authentication handler
     * @param handler Function that takes a token and returns true if valid
     * @note The handler should return true if the token is valid, otherwise false
     * @note Not used in the current implementation
     */
    void set_auth_handler(auth_handler handler);

    /**
     * @brief Send a request (or notification) to a client
     * @param session_id The session ID of the client
     * @param req The request to send
     */
    void send_request(const std::string& session_id, const request& req);

    /**
     * @brief Broadcast a notification to all connected, initialized sessions
     * @param notification The notification to send (must be a JSON-RPC notification, i.e. no id)
     */
    void broadcast_notification(const request& notification);

    /**
     * @brief Notify subscribed sessions that a resource was updated.
     * @param uri The URI of the updated resource
     */
    void notify_resource_updated(const std::string& uri);

    /**
     * @brief Notify initialized sessions that the prompt list changed.
     * @note The notification is sent only when prompts.listChanged is advertised.
     */
    void notify_prompts_list_changed();

    /**
     * @brief Get list of active session IDs
     * @return Vector of session IDs for connected, initialized clients
     */
    std::vector<std::string> get_active_sessions() const;

    /**
     * @brief Set mount point for server
     * @param mount_point The mount point to set
     * @param dir The directory to serve from the mount point
     * @param headers Optional headers to include in the response
     * @return True if the mount point was set successfully
     */
    bool set_mount_point(const std::string& mount_point, const std::string& dir, httplib::Headers headers = httplib::Headers());

private:
    std::string host_;
    int port_;
    std::string name_;
    std::string version_;
    json capabilities_;
    std::string instructions_;

    // The HTTP server
    std::unique_ptr<httplib::Server> http_server_;
    
    // Server thread (for non-blocking mode)
    std::unique_ptr<std::thread> server_thread_;

    // Session-specific event dispatchers
    std::map<std::string, std::shared_ptr<event_dispatcher>> session_dispatchers_;

    // Endpoint paths
    std::string mcp_endpoint_;
    
    // Method handlers
    std::map<std::string, method_handler> method_handlers_;
    
    // Notification handlers
    std::map<std::string, notification_handler> notification_handlers_;
    
    // Resources map (path -> resource)
    std::map<std::string, std::shared_ptr<resource>> resources_;

    // Resource templates
    struct resource_template_entry {
        std::string uri_template;
        std::string name;
        std::string mime_type;
        std::string description;
        json metadata;
        resource_template_handler handler;
    };
    std::vector<resource_template_entry> resource_templates_;

    // Resource subscriptions (session_id -> subscribed resource URIs)
    std::map<std::string, std::set<std::string>> resource_subscriptions_;

    // Tools map (name -> handler)
    std::map<std::string, std::pair<tool, tool_handler>> tools_;

    // Prompts map (name -> metadata + handler)
    std::map<std::string, std::pair<prompt, prompt_handler>> prompts_;
    
    // Authentication handler
    auth_handler auth_handler_;
    
    // Mutex for thread safety
    mutable std::mutex mutex_;
    
    // Running flag
    bool running_ = false;

    // Max sessions limit
    unsigned int max_sessions_ = MCP_MAX_SESSIONS;

    // Inactive session timeout
    unsigned int session_timeout_ = MCP_SESSION_TIMEOUT;
    
    // Thread pool for async method handlers
    thread_pool thread_pool_;
    
    // Map to track session initialization status (session_id -> initialized)
    std::map<std::string, bool> session_initialized_;

    // Streamable HTTP transport
    void handle_mcp_post(const httplib::Request& req, httplib::Response& res);
    void handle_mcp_get(const httplib::Request& req, httplib::Response& res);
    void handle_mcp_delete(const httplib::Request& req, httplib::Response& res);

    // Parse a single JSON-RPC message from JSON
    request parse_jsonrpc_message(const json& j) const;

    // Send a JSON-RPC message to a client
    void send_jsonrpc(const std::string& session_id, const json& message);
    
    // Process a JSON-RPC request
    json process_request(const request& req, const std::string& session_id);
    
    // Handle initialization request
    json handle_initialize(const request& req, const std::string& session_id);
    
    // Check if a session is initialized
    bool is_session_initialized(const std::string& session_id) const;
    
    // Set session initialization status
    void set_session_initialized(const std::string& session_id, bool initialized);

    // Refresh session activity for timeout tracking
    void touch_session_activity(const std::string& session_id);

    // Check whether a URI resolves to a registered static resource or template.
    bool has_resource(const std::string& uri) const;

    // Generate a random session ID
    std::string generate_session_id() const;

    // Auxiliary function to create an async handler from a regular handler
    template<typename F>
    std::function<std::future<json>(const json&, const std::string&)> make_async_handler(F&& handler) {
        return [handler = std::forward<F>(handler)](const json& params, const std::string& session_id) -> std::future<json> {
            return std::async(std::launch::async, [handler, params, session_id]() -> json {
                return handler(params, session_id);
            });
        };
    }

    // Helper class to simplify lock management
    class auto_lock {
    public:
        explicit auto_lock(std::mutex& mutex) : lock_(mutex) {}
    private:
        std::lock_guard<std::mutex> lock_;
    };
    
    // Get auto lock
    auto_lock get_lock() const {
        return auto_lock(mutex_);
    }

    // Session management and maintenance
    void check_inactive_sessions();

    std::mutex maintenance_mutex_;
    std::condition_variable maintenance_cond_;
    std::unique_ptr<std::thread> maintenance_thread_;
    bool maintenance_thread_run_ = false;

    // Session cleanup handler
    std::map<std::string, session_cleanup_handler> session_cleanup_handler_;

    // Close session
    void close_session(const std::string& session_id);
};

} // namespace mcp

#endif // MCP_SERVER_H
