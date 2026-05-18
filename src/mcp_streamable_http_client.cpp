/**
 * @file mcp_streamable_http_client.cpp
 * @brief Implementation of the MCP Streamable HTTP client
 *
 * This file implements the client-side functionality for the Model Context Protocol
 * using the Streamable HTTP transport (2025-11-25 specification).
 */

#include "mcp_streamable_http_client.h"

#include <algorithm>

namespace mcp {

streamable_http_client::streamable_http_client(const std::string& server_url,
    const std::string& mcp_endpoint,
    bool validate_certificates,
    const std::string& ca_cert_path)
    : server_url_(server_url), mcp_endpoint_(mcp_endpoint) {
    init_client(server_url, validate_certificates, ca_cert_path);
}

streamable_http_client::~streamable_http_client() {
    stop_sse_stream();

    if (session_active_) {
        close_session();
    }
}

void streamable_http_client::init_client(const std::string& server_url,
    bool validate_certificates, const std::string& ca_cert_path) {
    http_client_ = std::make_unique<httplib::Client>(server_url.c_str());
    notification_stream_client_ = std::make_unique<httplib::Client>(server_url.c_str());

    http_client_->set_connection_timeout(timeout_seconds_, 0);
    http_client_->set_read_timeout(timeout_seconds_, 0);
    http_client_->set_write_timeout(timeout_seconds_, 0);

    notification_stream_client_->set_connection_timeout(timeout_seconds_ * 2, 0);
    notification_stream_client_->set_write_timeout(timeout_seconds_, 0);
    const int notification_read_timeout = std::max(timeout_seconds_ * 2, 30);
    notification_stream_client_->set_read_timeout(notification_read_timeout, 0);

#ifdef MCP_SSL
    http_client_->enable_server_certificate_verification(validate_certificates);
    notification_stream_client_->enable_server_certificate_verification(validate_certificates);
    if (!ca_cert_path.empty()) {
        http_client_->set_ca_cert_path(ca_cert_path.c_str());
        notification_stream_client_->set_ca_cert_path(ca_cert_path.c_str());
    }
#endif
}

bool streamable_http_client::initialize(const std::string& client_name, const std::string& client_version) {
    LOG_INFO("Initializing MCP Streamable HTTP client...");

    request req = request::create("initialize", {
        {"protocolVersion", MCP_VERSION},
        {"capabilities", capabilities_},
        {"clientInfo", {
            {"name", client_name},
            {"version", client_version}
        }}
    });

    try {
        json req_json = req.to_json();
        std::string req_body = req_json.dump();

        httplib::Headers headers;
        headers.emplace("Content-Type", "application/json");
        headers.emplace("Accept", "application/json, text/event-stream");

        for (const auto& [key, value] : default_headers_) {
            headers.emplace(key, value);
        }

        auto result = http_client_->Post(mcp_endpoint_.c_str(), headers, req_body, "application/json");

        if (!result) {
            auto err = result.error();
            std::string error_msg = httplib::to_string(err);
            LOG_ERROR("Initialize request failed: ", error_msg);
            throw mcp_exception(error_code::internal_error, error_msg);
        }

        if (result->status / 100 != 2) {
            LOG_ERROR("Initialize failed with status: ", result->status);
            throw mcp_exception(error_code::internal_error,
                "Initialize failed with HTTP status " + std::to_string(result->status));
        }

        // Extract session ID from response header
        std::string new_session_id = result->get_header_value("Mcp-Session-Id");
        if (new_session_id.empty()) {
            LOG_ERROR("Server did not return Mcp-Session-Id header");
            throw mcp_exception(error_code::internal_error,
                "Server did not return Mcp-Session-Id header in initialize response");
        }

        {
            std::lock_guard<std::mutex> lock(mutex_);
            session_id_ = new_session_id;
            session_active_ = true;
        }

        LOG_INFO("Session established: ", session_id_);

        // Parse initialize response
        json res_json = json::parse(result->body);
        if (res_json.contains("result")) {
            server_capabilities_ = res_json["result"]["capabilities"];
        }

        // Send initialized notification
        request notification = request::create_notification("initialized");
        send_post(notification);

        LOG_INFO("Streamable HTTP client initialized successfully");
        return true;
    } catch (const mcp_exception&) {
        throw;
    } catch (const json::exception& e) {
        LOG_ERROR("JSON parse error during initialization: ", e.what());
        return false;
    } catch (const std::exception& e) {
        LOG_ERROR("Initialization failed: ", e.what());
        return false;
    }
}

bool streamable_http_client::ping() {
    request req = request::create("ping", {});

    try {
        json result = send_post(req);
        return result.empty() || result.is_object();
    } catch (...) {
        return false;
    }
}

void streamable_http_client::set_auth_token(const std::string& token) {
    std::lock_guard<std::mutex> lock(mutex_);
    auth_token_ = token;
    set_header("Authorization", "Bearer " + auth_token_);
}

void streamable_http_client::set_header(const std::string& key, const std::string& value) {
    std::lock_guard<std::mutex> lock(mutex_);
    default_headers_[key] = value;

    if (http_client_) {
        http_client_->set_default_headers({{key, value}});
    }
    if (notification_stream_client_) {
        notification_stream_client_->set_default_headers({{key, value}});
    }
}

void streamable_http_client::set_timeout(int timeout_seconds) {
    std::lock_guard<std::mutex> lock(mutex_);
    timeout_seconds_ = timeout_seconds;

    if (http_client_) {
        http_client_->set_connection_timeout(timeout_seconds_, 0);
        http_client_->set_write_timeout(timeout_seconds_, 0);
    }

    if (notification_stream_client_) {
        notification_stream_client_->set_connection_timeout(timeout_seconds_ * 2, 0);
        notification_stream_client_->set_write_timeout(timeout_seconds_, 0);
        notification_stream_client_->set_read_timeout(std::max(timeout_seconds_ * 2, 30), 0);
    }
}

void streamable_http_client::set_capabilities(const json& capabilities) {
    std::lock_guard<std::mutex> lock(mutex_);
    capabilities_ = capabilities;
}

response streamable_http_client::send_request(const std::string& method, const json& params) {
    request req = request::create(method, params);
    json result = send_post(req);

    response res;
    res.jsonrpc = "2.0";
    res.id = req.id;
    res.result = result;

    return res;
}

void streamable_http_client::send_notification(const std::string& method, const json& params) {
    request req = request::create_notification(method, params);
    send_post(req);
}

json streamable_http_client::get_server_capabilities() {
    return server_capabilities_;
}

json streamable_http_client::call_tool(const std::string& tool_name, const json& arguments) {
    return send_request("tools/call", {
        {"name", tool_name},
        {"arguments", arguments}
    }).result;
}

std::vector<tool> streamable_http_client::get_tools() {
    json response_json = send_request("tools/list", {}).result;
    std::vector<tool> tools;

    json tools_json;
    if (response_json.contains("tools") && response_json["tools"].is_array()) {
        tools_json = response_json["tools"];
    } else if (response_json.is_array()) {
        tools_json = response_json;
    } else {
        return tools;
    }

    for (const auto& tool_json : tools_json) {
        tool t;
        t.name = tool_json["name"];
        t.description = tool_json["description"];

        if (tool_json.contains("inputSchema")) {
            t.parameters_schema = tool_json["inputSchema"];
        }

        tools.push_back(t);
    }

    return tools;
}

json streamable_http_client::get_capabilities() {
    return capabilities_;
}

json streamable_http_client::list_resources(const std::string& cursor) {
    json params = json::object();
    if (!cursor.empty()) {
        params["cursor"] = cursor;
    }
    return send_request("resources/list", params).result;
}

json streamable_http_client::read_resource(const std::string& resource_uri) {
    return send_request("resources/read", {
        {"uri", resource_uri}
    }).result;
}

json streamable_http_client::subscribe_to_resource(const std::string& resource_uri) {
    return send_request("resources/subscribe", {
        {"uri", resource_uri}
    }).result;
}

json streamable_http_client::unsubscribe_from_resource(const std::string& resource_uri) {
    return send_request("resources/unsubscribe", {
        {"uri", resource_uri}
    }).result;
}

json streamable_http_client::list_resource_templates(const std::string& cursor) {
    json params = json::object();
    if (!cursor.empty()) {
        params["cursor"] = cursor;
    }
    return send_request("resources/templates/list", params).result;
}

bool streamable_http_client::is_running() const {
    return session_active_;
}

std::string streamable_http_client::get_session_id() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return session_id_;
}

// --- SSE stream management ---

void streamable_http_client::set_notification_handler(notification_callback callback) {
    std::lock_guard<std::mutex> lock(mutex_);
    notification_handler_ = std::move(callback);
}

bool streamable_http_client::start_sse_stream() {
    std::string session_id;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (!session_active_ || session_id_.empty()) {
            LOG_ERROR("Cannot start SSE stream: session not established");
            return false;
        }
        session_id = session_id_;
    }

    if (sse_running_.load()) {
        LOG_WARNING("SSE stream already running");
        return true;
    }

    sse_running_ = true;

    LOG_INFO("Starting GET SSE stream for session: ", session_id);

    sse_thread_ = std::make_unique<std::thread>([this, session_id]() {
        int retry_count = 0;
        const int max_retries = 5;
        const int retry_delay_base = 1000;

        while (sse_running_) {
            try {
                httplib::Headers headers;
                headers.emplace("Mcp-Session-Id", session_id);
                headers.emplace("MCP-Protocol-Version", MCP_VERSION);
                headers.emplace("Accept", "text/event-stream");

                for (const auto& [key, value] : default_headers_) {
                    headers.emplace(key, value);
                }

                std::string buffer;
                auto res = notification_stream_client_->Get(mcp_endpoint_.c_str(), headers,
                    [&, this](const char* data, size_t data_length) {
                        buffer.append(data, data_length);

                        // Normalize CRLF to LF
                        size_t crlf_pos = buffer.find("\r\n");
                        while (crlf_pos != std::string::npos) {
                            buffer.replace(crlf_pos, 2, "\n");
                            crlf_pos = buffer.find("\r\n", crlf_pos + 1);
                        }

                        // Process complete events
                        size_t start_pos = 0;
                        while ((start_pos = buffer.find("\n\n", start_pos)) != std::string::npos) {
                            size_t end_pos = start_pos + 2;
                            std::string event = buffer.substr(0, start_pos);
                            buffer.erase(0, end_pos);
                            start_pos = 0;

                            if (!parse_sse_event(event.data(), event.size())) {
                                LOG_ERROR("SSE stream: Failed to parse event");
                            }
                        }

                        return sse_running_.load();
                    });

                if (!res || res->status / 100 != 2) {
                    std::string error_msg = "GET SSE stream failed: ";
                    if (res) {
                        error_msg += "HTTP " + std::to_string(res->status);
                    } else {
                        error_msg += httplib::to_string(res.error());
                    }
                    throw std::runtime_error(error_msg);
                }

                retry_count = 0;
                LOG_INFO("GET SSE stream: Connection ended normally");
            } catch (const std::exception& e) {
                if (!sse_running_) {
                    LOG_INFO("SSE stream actively closed, no retry needed");
                    break;
                }

                if (++retry_count > max_retries) {
                    LOG_ERROR("Maximum retry count reached, stopping SSE stream");
                    break;
                }

                LOG_ERROR("SSE stream error: ", e.what());

                int delay = retry_delay_base * (1 << (retry_count - 1));
                LOG_INFO("Will retry in ", delay, " ms (attempt ", retry_count, "/", max_retries, ")");

                const int check_interval = 100;
                for (int waited = 0; waited < delay && sse_running_; waited += check_interval) {
                    std::this_thread::sleep_for(std::chrono::milliseconds(check_interval));
                }

                if (!sse_running_) {
                    LOG_INFO("SSE stream actively closed during retry wait");
                    break;
                }
            }
        }

        LOG_INFO("GET SSE stream thread exiting");
    });

    return true;
}

void streamable_http_client::stop_sse_stream() {
    if (!sse_running_.load()) {
        return;
    }

    LOG_INFO("Stopping GET SSE stream...");
    sse_running_ = false;

    if (sse_thread_ && sse_thread_->joinable()) {
        auto timeout = std::chrono::seconds(5);
        auto start = std::chrono::steady_clock::now();

        while (sse_thread_->joinable() &&
            std::chrono::steady_clock::now() - start < timeout) {
            try {
                sse_thread_->join();
                LOG_INFO("SSE stream thread joined successfully");
                break;
            } catch (const std::exception& e) {
                LOG_ERROR("Error joining SSE stream thread: ", e.what());
                std::this_thread::sleep_for(std::chrono::milliseconds(100));
            }
        }

        if (sse_thread_->joinable()) {
            LOG_WARNING("SSE stream thread did not end within timeout, detaching");
            sse_thread_->detach();
        }
    }
}

// --- Private methods ---

json streamable_http_client::send_post(const request& req) {
    std::string session_id;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        session_id = session_id_;
    }

    json req_json = req.to_json();
    std::string req_body = req_json.dump();

    httplib::Headers headers;
    headers.emplace("Content-Type", "application/json");
    headers.emplace("Accept", "application/json, text/event-stream");

    // Include session ID for non-initialize requests
    if (!session_id.empty() && req.method != "initialize") {
        headers.emplace("Mcp-Session-Id", session_id);
        headers.emplace("MCP-Protocol-Version", MCP_VERSION);
    }

    for (const auto& [key, value] : default_headers_) {
        headers.emplace(key, value);
    }

    if (req.is_notification()) {
        auto result = http_client_->Post(mcp_endpoint_.c_str(), headers, req_body, "application/json");

        if (!result) {
            auto err = result.error();
            std::string error_msg = httplib::to_string(err);
            LOG_ERROR("Notification POST failed: ", error_msg);
            throw mcp_exception(error_code::internal_error, error_msg);
        }

        return json::object();
    }

    auto result = http_client_->Post(mcp_endpoint_.c_str(), headers, req_body, "application/json");

    if (!result) {
        auto err = result.error();
        std::string error_msg = httplib::to_string(err);
        LOG_ERROR("POST request failed: ", error_msg);
        throw mcp_exception(error_code::internal_error, error_msg);
    }

    if (result->status / 100 != 2) {
        try {
            json res_json = json::parse(result->body);

            if (res_json.contains("error")) {
                int code = res_json["error"]["code"];
                std::string message = res_json["error"]["message"];
                throw mcp_exception(static_cast<error_code>(code), message);
            }

            if (res_json.contains("result")) {
                return res_json["result"];
            }

            return json::object();
        } catch (const mcp_exception&) {
            throw;
        } catch (const json::exception& e) {
            throw mcp_exception(error_code::parse_error,
                "Failed to parse response: " + std::string(e.what()));
        }
    }

    // Parse response
    try {
        json res_json = json::parse(result->body);

        if (res_json.contains("result")) {
            return res_json["result"];
        } else if (res_json.contains("error")) {
            int code = res_json["error"]["code"];
            std::string message = res_json["error"]["message"];
            throw mcp_exception(static_cast<error_code>(code), message);
        }

        return json::object();
    } catch (const mcp_exception&) {
        throw;
    } catch (const json::exception& e) {
        throw mcp_exception(error_code::parse_error,
            "Failed to parse JSON-RPC response: " + std::string(e.what()));
    }
}

bool streamable_http_client::parse_sse_event(const char* data, size_t length) {
    try {
        std::istringstream stream(std::string(data, length));
        std::string line;
        std::string event_type = "message";
        std::vector<std::string> data_lines;

        while (std::getline(stream, line)) {
            if (!line.empty() && line.back() == '\r') {
                line.pop_back();
            }

            if (!line.empty() && line[0] == ':') {
                continue;
            } else if (line.substr(0, 7) == "event: ") {
                event_type = line.substr(7);
            } else if (line.substr(0, 6) == "data: ") {
                data_lines.push_back(line.substr(6));
            } else if (line.empty()) {
                break;
            }
        }

        if (data_lines.empty()) {
            return true;
        }

        std::string data_content;
        for (size_t i = 0; i < data_lines.size(); ++i) {
            if (i > 0) data_content += '\n';
            data_content += data_lines[i];
        }

        if (event_type == "message") {
            try {
                json message = json::parse(data_content);

                if (message.contains("method")) {
                    std::string method = message["method"];
                    json params = message.value("params", json::object());

                    notification_callback handler;
                    {
                        std::lock_guard<std::mutex> lock(mutex_);
                        handler = notification_handler_;
                    }

                    if (handler) {
                        handler(method, params);
                    } else {
                        LOG_WARNING("Received server notification '", method, "' but no handler set");
                    }
                } else {
                    LOG_INFO("Received non-method message on SSE stream: ", data_content);
                }
            } catch (const json::exception& e) {
                LOG_ERROR("Failed to parse SSE message: ", e.what());
            }
            return true;
        }

        LOG_WARNING("Received unknown SSE event type: ", event_type);
        return true;
    } catch (const std::exception& e) {
        LOG_ERROR("Error parsing SSE event: ", e.what());
        return false;
    }
}

void streamable_http_client::close_session() {
    std::string session_id;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (!session_active_ || session_id_.empty()) {
            return;
        }
        session_id = session_id_;
    }

    LOG_INFO("Closing session: ", session_id);

    httplib::Headers headers;
    headers.emplace("Mcp-Session-Id", session_id);
    headers.emplace("MCP-Protocol-Version", MCP_VERSION);

    for (const auto& [key, value] : default_headers_) {
        headers.emplace(key, value);
    }

    auto result = http_client_->Delete(mcp_endpoint_.c_str(), headers);

    if (!result) {
        LOG_WARNING("Failed to send DELETE for session: ", httplib::to_string(result.error()));
    } else if (result->status / 100 != 2) {
        LOG_WARNING("DELETE session returned HTTP ", result->status);
    }

    {
        std::lock_guard<std::mutex> lock(mutex_);
        session_active_ = false;
        session_id_.clear();
    }

    LOG_INFO("Session closed");
}

} // namespace mcp
