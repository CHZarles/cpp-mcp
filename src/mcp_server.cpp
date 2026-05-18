/**
 * @file mcp_server.cpp
 * @brief Implementation of the MCP server
 * 
 * This file implements the server-side functionality for the Model Context Protocol.
 * Follows the 2025-11-25 Streamable HTTP transport.
 */

#include "mcp_server.h"
#include <sys/stat.h>

namespace {
bool file_exists(const std::string& path) {
    struct stat st;
    return ::stat(path.c_str(), &st) == 0;
}
} // anonymous namespace

namespace mcp {

namespace {

bool is_jsonrpc_response_message(const json& message) {
    return message.is_object() &&
        !message.contains("method") &&
        message.contains("id") &&
        (message.contains("result") || message.contains("error"));
}

bool is_string_member(const json& object, const std::string& key) {
    return object.contains(key) && object[key].is_string();
}

void validate_prompt_content(const json& content) {
    if (!content.is_object() || !is_string_member(content, "type")) {
        throw mcp_exception(error_code::internal_error, "Prompt message content must be an object with a string type");
    }

    const std::string type = content["type"].get<std::string>();
    if (type == "text") {
        if (!is_string_member(content, "text")) {
            throw mcp_exception(error_code::internal_error, "Text prompt content must include string text");
        }
        return;
    }

    if (type == "image" || type == "audio") {
        if (!is_string_member(content, "data") || !is_string_member(content, "mimeType")) {
            throw mcp_exception(error_code::internal_error, type + " prompt content must include string data and mimeType");
        }
        return;
    }

    if (type == "resource") {
        if (!content.contains("resource") || !content["resource"].is_object()) {
            throw mcp_exception(error_code::internal_error, "Resource prompt content must include a resource object");
        }

        const json& resource = content["resource"];
        if (!is_string_member(resource, "uri")) {
            throw mcp_exception(error_code::internal_error, "Embedded prompt resource must include a string uri");
        }
        if (!resource.contains("text") && !resource.contains("blob")) {
            throw mcp_exception(error_code::internal_error, "Embedded prompt resource must include text or blob");
        }
        if (resource.contains("text") && !resource["text"].is_string()) {
            throw mcp_exception(error_code::internal_error, "Embedded prompt resource text must be a string");
        }
        if (resource.contains("blob") && !resource["blob"].is_string()) {
            throw mcp_exception(error_code::internal_error, "Embedded prompt resource blob must be a string");
        }
        if (resource.contains("mimeType") && !resource["mimeType"].is_string()) {
            throw mcp_exception(error_code::internal_error, "Embedded prompt resource mimeType must be a string");
        }
        return;
    }

    throw mcp_exception(error_code::internal_error, "Unsupported prompt content type: " + type);
}

void validate_prompt_result(const json& result) {
    if (!result.is_object() || !result.contains("messages") || !result["messages"].is_array()) {
        throw mcp_exception(error_code::internal_error, "Prompt handler must return an object with a messages array");
    }

    if (result.contains("description") && !result["description"].is_string()) {
        throw mcp_exception(error_code::internal_error, "Prompt result description must be a string");
    }

    for (const auto& message : result["messages"]) {
        if (!message.is_object() || !is_string_member(message, "role") || !message.contains("content")) {
            throw mcp_exception(error_code::internal_error, "Prompt messages must include role and content");
        }

        const std::string role = message["role"].get<std::string>();
        if (role != "user" && role != "assistant") {
            throw mcp_exception(error_code::internal_error, "Prompt message role must be user or assistant");
        }

        validate_prompt_content(message["content"]);
    }
}

} // namespace


server::server(const server::configuration& conf)
    : host_(conf.host)
    , port_(conf.port)
    , name_(conf.name)
    , version_(conf.version)
    , mcp_endpoint_(conf.mcp_endpoint)
    , thread_pool_(conf.threadpool_size)
    , max_sessions_(conf.max_sessions)
    , session_timeout_(conf.session_timeout)
{
    #ifdef MCP_SSL
    if (conf.ssl.server_cert_path && conf.ssl.server_private_key_path) {
        if (!file_exists(*conf.ssl.server_cert_path)) {
            LOG_ERROR("SSL certificate file '", *conf.ssl.server_cert_path, "' not found");
        }

        if (!file_exists(*conf.ssl.server_private_key_path)) {
            LOG_ERROR("SSL key file '", *conf.ssl.server_private_key_path, "' not found");
        }

        http_server_ = std::make_unique<httplib::SSLServer>(conf.ssl.server_cert_path->c_str(),
            conf.ssl.server_private_key_path->c_str());
    } else {
        http_server_ = std::make_unique<httplib::Server>();
    }
    #else
     http_server_ = std::make_unique<httplib::Server>();
    #endif
}

server::~server() {
    stop();
}


bool server::start(bool blocking) {
    if (running_) {
        return true;  // Already running
    }
    
    LOG_INFO("Starting MCP server on ", host_, ":", port_);

    // Setup CORS handling
    http_server_->Options(".*", [](const httplib::Request& req, httplib::Response& res) {
        res.set_header("Access-Control-Allow-Origin", "*");
        res.set_header("Access-Control-Allow-Methods", "GET, POST, DELETE, OPTIONS");
        res.set_header("Access-Control-Allow-Headers", "Content-Type, Accept, Mcp-Session-Id");
        res.set_header("Access-Control-Expose-Headers", "Mcp-Session-Id");
        res.status = 204; // No Content
    });

    // Streamable HTTP transport
    http_server_->Post(mcp_endpoint_.c_str(), [this](const httplib::Request& req, httplib::Response& res) {
        this->handle_mcp_post(req, res);
        LOG_INFO(req.remote_addr, ":", req.remote_port, " - \"POST ", req.path, " HTTP/1.1\" ", res.status);
    });

    http_server_->Get(mcp_endpoint_.c_str(), [this](const httplib::Request& req, httplib::Response& res) {
        this->handle_mcp_get(req, res);
        LOG_INFO(req.remote_addr, ":", req.remote_port, " - \"GET ", req.path, " HTTP/1.1\" ", res.status);
    });

    http_server_->Delete(mcp_endpoint_.c_str(), [this](const httplib::Request& req, httplib::Response& res) {
        this->handle_mcp_delete(req, res);
        LOG_INFO(req.remote_addr, ":", req.remote_port, " - \"DELETE ", req.path, " HTTP/1.1\" ", res.status);
    });
    
    // Start resource check thread (only start in non-blocking mode)
    if (!blocking) {
        maintenance_thread_run_ = true;
        maintenance_thread_ = std::make_unique<std::thread>([this]() {
            while (true) {
                // Check inactive sessions every 10 seconds
                std::unique_lock<std::mutex> lock(maintenance_mutex_);
                auto should_exit = maintenance_cond_.wait_for(lock, std::chrono::seconds(10), [this] {
                    return !maintenance_thread_run_;
                });
                if (should_exit) {
                    LOG_INFO("Maintenance thread exiting");
                    return;
                }
                lock.unlock();

                try {
                    check_inactive_sessions();
                } catch (const std::exception& e) {
                    LOG_ERROR("Exception in maintenance thread: ", e.what());
                } catch (...) {
                    LOG_ERROR("Unknown exception in maintenance thread");
                }
            }
        });
    }
    
    // Start server
    if (blocking) {
        running_ = true;
        LOG_INFO("Starting server in blocking mode");
        if (!http_server_->listen(host_.c_str(), port_)) {
            running_ = false;
            LOG_ERROR("Failed to start server on ", host_, ":", port_);
            return false;
        }
        return true;
    } else {
        // Start server in a separate thread
        server_thread_ = std::make_unique<std::thread>([this]() {
            LOG_INFO("Starting server in separate thread");
            if (!http_server_->listen(host_.c_str(), port_)) {
                LOG_ERROR("Failed to start server on ", host_, ":", port_);
                running_ = false;
                return;
            }
        });
        running_ = true;
        return true;
    }
}

void server::stop() {
    if (!running_) {
        return;
    }
    
    LOG_INFO("Stopping MCP server on ", host_, ":", port_);
    running_ = false;

    // Close maintenance thread
    if (maintenance_thread_ && maintenance_thread_->joinable()) {
        {
            std::unique_lock<std::mutex> lock(maintenance_mutex_);
            maintenance_thread_run_ = false;
        }

        maintenance_cond_.notify_one();

        try {
            maintenance_thread_->join();
        } catch (...) {
            maintenance_thread_->detach();
        }
    }
    
    // Copy session IDs first. close_session() takes mutex_, so never call it
    // while holding the server lock.
    std::vector<std::string> session_ids;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        session_ids.reserve(session_dispatchers_.size());
        for (const auto& [session_id, _] : session_dispatchers_) {
            session_ids.push_back(session_id);
        }
    }  // End of mutex lock scope

    for (const auto& session_id : session_ids) {
        close_session(session_id);
    }

    // Give threads some time to handle close events
    std::this_thread::sleep_for(std::chrono::milliseconds(300));
    
    if (server_thread_ && server_thread_->joinable()) {
        http_server_->stop();
        try {
            server_thread_->join();
        } catch (...) {
            server_thread_->detach();
        }
    } else {
        http_server_->stop();
    }
    
    LOG_INFO("MCP server stopped");
}

bool server::is_running() const {
    return running_;
}

void server::set_server_info(const std::string& name, const std::string& version) {
    std::lock_guard<std::mutex> lock(mutex_);
    name_ = name;
    version_ = version;
}

void server::set_capabilities(const json& capabilities) {
    std::lock_guard<std::mutex> lock(mutex_);
    capabilities_ = capabilities;
}

void server::set_instructions(const std::string& instructions) {
    std::lock_guard<std::mutex> lock(mutex_);
    instructions_ = instructions;
}

void server::register_method(const std::string& method, method_handler handler) {
    std::lock_guard<std::mutex> lock(mutex_);
    method_handlers_[method] = handler;
}

void server::register_notification(const std::string& method, notification_handler handler) {
    std::lock_guard<std::mutex> lock(mutex_);
    notification_handlers_[method] = handler;
}

// Simple URI template matching: extracts {param} segments from a template.
// e.g. "myapp://items/{id}" matches "myapp://items/abc" with params["id"]="abc"
static bool match_uri_template(const std::string& tmpl,
                               const std::string& uri,
                               std::map<std::string, std::string>& params)
{
    params.clear();
    size_t ti = 0, ui = 0;
    while (ti < tmpl.size() && ui < uri.size()) {
        if (tmpl[ti] == '{') {
            size_t end = tmpl.find('}', ti);
            if (end == std::string::npos) return false;
            std::string key = tmpl.substr(ti + 1, end - ti - 1);
            ti = end + 1;
            // Consume URI chars until we hit the next literal from the template (or end)
            size_t val_end;
            if (ti < tmpl.size()) {
                val_end = uri.find(tmpl[ti], ui);
                if (val_end == std::string::npos) return false;
            } else {
                val_end = uri.size();
            }
            params[key] = uri.substr(ui, val_end - ui);
            ui = val_end;
        } else {
            if (tmpl[ti] != uri[ui]) return false;
            ++ti;
            ++ui;
        }
    }
    return ti == tmpl.size() && ui == uri.size();
}

bool server::has_resource(const std::string& uri) const
{
    if (resources_.find(uri) != resources_.end()) {
        return true;
    }

    for (const auto& tmpl : resource_templates_) {
        std::map<std::string, std::string> uri_params;
        if (match_uri_template(tmpl.uri_template, uri, uri_params)) {
            return true;
        }
    }

    return false;
}

static json resource_template_to_json(const std::string& uri_template,
                                      const std::string& name,
                                      const std::string& description,
                                      const std::string& mime_type,
                                      const json& metadata)
{
    json result = {
        {"uriTemplate", uri_template},
        {"name", name},
        {"description", description},
        {"mimeType", mime_type}
    };

    if (metadata.is_object()) {
        for (const auto& item : metadata.items()) {
            if (!item.value().is_null() && !(item.value().is_array() && item.value().empty()) &&
                !(item.value().is_object() && item.value().empty())) {
                result[item.key()] = item.value();
            }
        }
    }

    return result;
}

void server::register_resource(const std::string& path, std::shared_ptr<resource> resource) {
    std::lock_guard<std::mutex> lock(mutex_);
    resources_[path] = resource;

    // Register methods for resource access
    if (method_handlers_.find("resources/read") == method_handlers_.end()) {
        method_handlers_["resources/read"] = [this](const json& params, const std::string& session_id) -> json {
            if (!params.contains("uri")) {
                throw mcp_exception(error_code::invalid_params, "Missing 'uri' parameter");
            }

            std::string uri = params["uri"];

            // Try static resources first
            auto it = resources_.find(uri);
            if (it != resources_.end()) {
                json contents = json::array();
                contents.push_back(it->second->read());
                return json{{"contents", contents}};
            }

            // Try resource templates
            for (const auto& tmpl : resource_templates_) {
                std::map<std::string, std::string> uri_params;
                if (match_uri_template(tmpl.uri_template, uri, uri_params)) {
                    json result = tmpl.handler(uri, uri_params, session_id);
                    json contents = json::array();
                    contents.push_back(result);
                    return json{{"contents", contents}};
                }
            }

            throw mcp_exception(error_code::invalid_params, "Resource not found: " + uri);
        };
    }
    
    if (method_handlers_.find("resources/list") == method_handlers_.end()) {
        method_handlers_["resources/list"] = [this](const json& /*params*/, const std::string& /*session_id*/) -> json {
            json resources = json::array();
        
            for (const auto& [uri, res] : resources_) {
                resources.push_back(res->get_metadata());
            }
            
            return {
                {"resources", resources}
            };
        };
    }
    
    if (method_handlers_.find("resources/subscribe") == method_handlers_.end()) {
        method_handlers_["resources/subscribe"] = [this](const json& params, const std::string& session_id) -> json {
            if (!params.contains("uri")) {
                throw mcp_exception(error_code::invalid_params, "Missing 'uri' parameter");
            }
            
            std::string uri = params["uri"];
            if (!has_resource(uri)) {
                throw mcp_exception(error_code::invalid_params, "Resource not found: " + uri);
            }

            {
                std::lock_guard<std::mutex> lock(mutex_);
                resource_subscriptions_[session_id].insert(uri);
            }

            return json::object();
        };
    }

    if (method_handlers_.find("resources/unsubscribe") == method_handlers_.end()) {
        method_handlers_["resources/unsubscribe"] = [this](const json& params, const std::string& session_id) -> json {
            if (!params.contains("uri")) {
                throw mcp_exception(error_code::invalid_params, "Missing 'uri' parameter");
            }

            std::string uri = params["uri"];
            if (!has_resource(uri)) {
                throw mcp_exception(error_code::invalid_params, "Resource not found: " + uri);
            }

            {
                std::lock_guard<std::mutex> lock(mutex_);
                auto subscriptions_it = resource_subscriptions_.find(session_id);
                if (subscriptions_it != resource_subscriptions_.end()) {
                    subscriptions_it->second.erase(uri);
                    if (subscriptions_it->second.empty()) {
                        resource_subscriptions_.erase(subscriptions_it);
                    }
                }
            }

            return json::object();
        };
    }
    
    if (method_handlers_.find("resources/templates/list") == method_handlers_.end()) {
        method_handlers_["resources/templates/list"] = [this](const json& /*params*/, const std::string& /*session_id*/) -> json {
            json templates_json = json::array();
            for (const auto& tmpl : resource_templates_) {
                templates_json.push_back(resource_template_to_json(
                    tmpl.uri_template,
                    tmpl.name,
                    tmpl.description,
                    tmpl.mime_type,
                    tmpl.metadata));
            }
            return json{{"resourceTemplates", templates_json}};
        };
    }
}

void server::register_resource_template(
    const std::string& uri_template,
    const std::string& name,
    const std::string& mime_type,
    const std::string& description,
    resource_template_handler handler,
    const json& metadata)
{
    std::lock_guard<std::mutex> lock(mutex_);
    resource_templates_.push_back({uri_template, name, mime_type, description, metadata, std::move(handler)});

    // Ensure resource read/list/template handlers are registered
    // (they may already exist if register_resource was called first)
    if (method_handlers_.find("resources/read") == method_handlers_.end()) {
        // Force registration by calling register_resource with a dummy,
        // or just register the read handler directly.
        method_handlers_["resources/read"] = [this](const json& params, const std::string& session_id) -> json {
            if (!params.contains("uri")) {
                throw mcp_exception(error_code::invalid_params, "Missing 'uri' parameter");
            }
            std::string uri = params["uri"];
            auto it = resources_.find(uri);
            if (it != resources_.end()) {
                json contents = json::array();
                contents.push_back(it->second->read());
                return json{{"contents", contents}};
            }
            for (const auto& tmpl : resource_templates_) {
                std::map<std::string, std::string> uri_params;
                if (match_uri_template(tmpl.uri_template, uri, uri_params)) {
                    json result = tmpl.handler(uri, uri_params, session_id);
                    json contents = json::array();
                    contents.push_back(result);
                    return json{{"contents", contents}};
                }
            }
            throw mcp_exception(error_code::invalid_params, "Resource not found: " + uri);
        };
    }

    if (method_handlers_.find("resources/templates/list") == method_handlers_.end()) {
        method_handlers_["resources/templates/list"] = [this](const json& /*params*/, const std::string& /*session_id*/) -> json {
            json templates_json = json::array();
            for (const auto& tmpl : resource_templates_) {
                templates_json.push_back(resource_template_to_json(
                    tmpl.uri_template,
                    tmpl.name,
                    tmpl.description,
                    tmpl.mime_type,
                    tmpl.metadata));
            }
            return json{{"resourceTemplates", templates_json}};
        };
    }

    if (method_handlers_.find("resources/list") == method_handlers_.end()) {
        method_handlers_["resources/list"] = [this](const json& /*params*/, const std::string& /*session_id*/) -> json {
            json resources = json::array();

            for (const auto& [uri, res] : resources_) {
                resources.push_back(res->get_metadata());
            }

            return {{"resources", resources}};
        };
    }

    if (method_handlers_.find("resources/subscribe") == method_handlers_.end()) {
        method_handlers_["resources/subscribe"] = [this](const json& params, const std::string& session_id) -> json {
            if (!params.contains("uri")) {
                throw mcp_exception(error_code::invalid_params, "Missing 'uri' parameter");
            }

            std::string uri = params["uri"];
            if (!has_resource(uri)) {
                throw mcp_exception(error_code::invalid_params, "Resource not found: " + uri);
            }

            {
                std::lock_guard<std::mutex> lock(mutex_);
                resource_subscriptions_[session_id].insert(uri);
            }

            return json::object();
        };
    }

    if (method_handlers_.find("resources/unsubscribe") == method_handlers_.end()) {
        method_handlers_["resources/unsubscribe"] = [this](const json& params, const std::string& session_id) -> json {
            if (!params.contains("uri")) {
                throw mcp_exception(error_code::invalid_params, "Missing 'uri' parameter");
            }

            std::string uri = params["uri"];
            if (!has_resource(uri)) {
                throw mcp_exception(error_code::invalid_params, "Resource not found: " + uri);
            }

            {
                std::lock_guard<std::mutex> lock(mutex_);
                auto subscriptions_it = resource_subscriptions_.find(session_id);
                if (subscriptions_it != resource_subscriptions_.end()) {
                    subscriptions_it->second.erase(uri);
                    if (subscriptions_it->second.empty()) {
                        resource_subscriptions_.erase(subscriptions_it);
                    }
                }
            }

            return json::object();
        };
    }
}

void server::register_prompt(const prompt& prompt_def, prompt_handler handler) {
    std::lock_guard<std::mutex> lock(mutex_);
    prompts_[prompt_def.name] = std::make_pair(prompt_def, std::move(handler));
    if (!capabilities_.is_object()) {
        capabilities_ = json::object();
    }
    if (!capabilities_.contains("prompts") || !capabilities_["prompts"].is_object()) {
        capabilities_["prompts"] = json::object();
    }

    if (method_handlers_.find("prompts/list") == method_handlers_.end()) {
        method_handlers_["prompts/list"] = [this](const json& params, const std::string&) -> json {
            json prompts_json = json::array();
            {
                std::lock_guard<std::mutex> lock(mutex_);
                for (const auto& [name, prompt_pair] : prompts_) {
                    prompts_json.push_back(prompt_pair.first.to_json());
                }
            }

            if (params.contains("cursor") && !params["cursor"].is_string()) {
                throw mcp_exception(error_code::invalid_params, "'cursor' must be a string");
            }
            return json{{"prompts", prompts_json}};
        };
    }

    if (method_handlers_.find("prompts/get") == method_handlers_.end()) {
        method_handlers_["prompts/get"] = [this](const json& params, const std::string& session_id) -> json {
            if (!params.contains("name") || !params["name"].is_string()) {
                throw mcp_exception(error_code::invalid_params, "Missing or invalid 'name' parameter");
            }

            const std::string name = params["name"].get<std::string>();
            json arguments = params.value("arguments", json::object());
            if (!arguments.is_object()) {
                throw mcp_exception(error_code::invalid_params, "'arguments' must be an object");
            }

            prompt metadata;
            prompt_handler handler;
            {
                std::lock_guard<std::mutex> lock(mutex_);
                auto it = prompts_.find(name);
                if (it == prompts_.end()) {
                    throw mcp_exception(error_code::invalid_params, "Prompt not found: " + name);
                }
                metadata = it->second.first;
                handler = it->second.second;
            }

            for (const auto& [argument_name, argument_value] : arguments.items()) {
                if (!argument_value.is_string()) {
                    throw mcp_exception(
                        error_code::invalid_params,
                        "Prompt argument must be a string: " + argument_name);
                }
            }

            for (const auto& argument : metadata.arguments) {
                if (argument.required && !arguments.contains(argument.name)) {
                    throw mcp_exception(
                        error_code::invalid_params,
                        "Missing required prompt argument: " + argument.name);
                }
            }

            json result = handler(arguments, session_id);
            if (!metadata.description.empty() && !result.contains("description")) {
                result["description"] = metadata.description;
            }
            validate_prompt_result(result);
            return result;
        };
    }
}

void server::register_tool(const tool& tool, tool_handler handler) {
    std::lock_guard<std::mutex> lock(mutex_);
    tools_[tool.name] = std::make_pair(tool, handler);
    
    // Register methods for tool listing and calling
    if (method_handlers_.find("tools/list") == method_handlers_.end()) {
        method_handlers_["tools/list"] = [this](const json& params, const std::string& session_id) -> json {
            json tools_json = json::array();
            for (const auto& [name, tool_pair] : tools_) {
                tools_json.push_back(tool_pair.first.to_json());
            }
            return json{{"tools", tools_json}};
        };
    }
    
    if (method_handlers_.find("tools/call") == method_handlers_.end()) {
        method_handlers_["tools/call"] = [this](const json& params, const std::string& session_id) -> json {
            if (!params.contains("name")) {
                throw mcp_exception(error_code::invalid_params, "Missing 'name' parameter");
            }
            
            std::string tool_name = params["name"];
            auto it = tools_.find(tool_name);
            if (it == tools_.end()) {
                throw mcp_exception(error_code::invalid_params, "Tool not found: " + tool_name);
            }
            
            json tool_args = params.contains("arguments") ? params["arguments"] : json::object();

            if (tool_args.is_string()) {
                try {
                    tool_args = json::parse(tool_args.get<std::string>());
                } catch (const json::exception& e) {
                    throw mcp_exception(error_code::invalid_params, "Invalid JSON arguments: " + std::string(e.what()));
                }
            }

            json tool_result = it->second.second(tool_args, session_id);

            // If result is not an object with content, wrap it
            if (!tool_result.is_object() || !tool_result.contains("content")) {
                json wrapped;
                wrapped["content"] = json::array({tool_result});
                wrapped["isError"] = tool_result.value("isError", false);
                tool_result = wrapped;
            } else {
                // Ensure isError field exists
                tool_result["isError"] = tool_result.value("isError", false);
            }

            return tool_result;
        };
    }
}

void server::register_session_cleanup(const std::string& key, session_cleanup_handler handler) {
    std::lock_guard<std::mutex> lock(mutex_);
    session_cleanup_handler_[key] = handler;
}

std::vector<tool> server::get_tools() const {
    std::lock_guard<std::mutex> lock(mutex_);
    std::vector<tool> tools;
    
    for (const auto& [name, tool_pair] : tools_) {
        tools.push_back(tool_pair.first);
    }
    
    return tools;
}

void server::set_auth_handler(auth_handler handler) {
    std::lock_guard<std::mutex> lock(mutex_);
    auth_handler_ = handler;
}

// ---------------------------------------------------------------------------
// Streamable HTTP transport
// ---------------------------------------------------------------------------

request server::parse_jsonrpc_message(const json& j) const {
    request req;
    req.jsonrpc = j.value("jsonrpc", "2.0");
    if (j.contains("id") && !j["id"].is_null()) {
        req.id = j["id"];
    }
    if (j.contains("method")) {
        req.method = j["method"].get<std::string>();
    }
    if (j.contains("params")) {
        req.params = j["params"];
    }
    return req;
}

void server::handle_mcp_post(const httplib::Request& req, httplib::Response& res) {
    // CORS headers
    res.set_header("Access-Control-Allow-Origin", "*");
    res.set_header("Access-Control-Allow-Methods", "GET, POST, DELETE, OPTIONS");
    res.set_header("Access-Control-Allow-Headers", "Content-Type, Accept, Mcp-Session-Id");
    res.set_header("Access-Control-Expose-Headers", "Mcp-Session-Id");

    // Parse JSON body
    json body;
    try {
        body = json::parse(req.body);
        if (!body.is_null()) {
            LOG_INFO("POST body: ", body.dump());
        }
        LOG_INFO("Raw POST body: '", req.body, "'");
    } catch (const json::exception& e) {
        LOG_ERROR("Failed to parse JSON: ", e.what());
        res.status = 400;
        res.set_content(
            response::create_error(nullptr, error_code::parse_error, "Invalid JSON").to_json().dump(),
            "application/json");
        return;
    }

    // Get or create session
    std::string session_id = req.get_header_value("Mcp-Session-Id");

    // Check if this is an initialize request (no session needed)
    bool is_initialize = false;
    if (body.is_object() && body.contains("method") && body["method"] == "initialize") {
        is_initialize = true;
    } else if (body.is_array()) {
        // Check if batch contains an initialize request
        for (const auto& item : body) {
            if (item.is_object() && item.value("method", "") == "initialize") {
                is_initialize = true;
                break;
            }
        }
    }

    // Reject re-initialization on an existing session
    if (is_initialize && !session_id.empty()) {
        std::lock_guard<std::mutex> lock(mutex_);
        if (session_dispatchers_.find(session_id) != session_dispatchers_.end()) {
            res.status = 400;
            res.set_content("{\"error\":\"Session already initialized. Delete and re-create.\"}",
                            "application/json");
            return;
        }
    }

    // Validate session for non-initialize requests
    if (!is_initialize) {
        if (session_id.empty()) {
            res.status = 400;
            res.set_content("{\"error\":\"Missing Mcp-Session-Id header\"}", "application/json");
            return;
        }
        std::lock_guard<std::mutex> lock(mutex_);
        if (session_dispatchers_.find(session_id) == session_dispatchers_.end()) {
            // Session expired or invalid — client must re-initialize
            res.status = 404;
            res.set_content("{\"error\":\"Session not found\"}", "application/json");
            return;
        }
    }

    if (!session_id.empty()) {
        touch_session_activity(session_id);
    }

    // Handle batched requests / notifications / client responses
    std::vector<json> items;
    if (body.is_array()) {
        for (const auto& item : body) {
            items.push_back(item);
        }
    } else {
        items.push_back(body);
    }

    // Process each item independently.
    // For initialize: create session, return inline JSON with Mcp-Session-Id header
    if (is_initialize) {
        // Enforce session limit
        if (max_sessions_ > 0) {
            std::lock_guard<std::mutex> lock(mutex_);
            if (session_dispatchers_.size() >= max_sessions_) {
                res.status = 503;
                res.set_content("{\"error\":\"Too many sessions\"}", "application/json");
                return;
            }
        }

        session_id = generate_session_id();

        // Create session dispatcher for server-push via GET
        auto session_dispatcher = std::make_shared<event_dispatcher>();
        session_dispatcher->update_activity();
        {
            std::lock_guard<std::mutex> lock(mutex_);
            session_dispatchers_[session_id] = session_dispatcher;
        }

        auto mcp_req = parse_jsonrpc_message(items[0]);
        json result = handle_initialize(mcp_req, session_id);

        res.set_header("Mcp-Session-Id", session_id);
        res.set_header("Content-Type", "application/json");
        res.set_content(result.dump(), "application/json");
        return;
    }

    // Non-initialize requests: check Accept header to decide response mode
    std::string accept = req.get_header_value("Accept");
    bool client_accepts_sse = accept.find("text/event-stream") != std::string::npos;

    // Process all items, collect responses for requests
    json responses = json::array();
    for (const auto& item : items) {
        if (is_jsonrpc_response_message(item)) {
            handle_client_response(item);
            continue;
        }

        auto mcp_req = parse_jsonrpc_message(item);

        if (mcp_req.is_notification()) {
            // Fire-and-forget
            process_request(mcp_req, session_id);
            continue;
        }

        // Process request synchronously (inline response)
        json result = process_request(mcp_req, session_id);
        responses.push_back(result);
    }

    if (responses.empty()) {
        res.status = 202;
        return;
    }

    // If client accepts SSE and we might want to stream, use SSE
    // For now, use inline JSON for simplicity — SSE streaming on POST
    // can be added later for long-running operations
    if (client_accepts_sse && responses.size() > 1) {
        // Stream responses as SSE events
        res.set_header("Content-Type", "text/event-stream");
        res.set_header("Cache-Control", "no-cache");
        std::string sse_body;
        for (const auto& r : responses) {
            sse_body += "event: message\r\ndata: " + r.dump() + "\r\n\r\n";
        }
        res.set_content(sse_body, "text/event-stream");
        return;
    }

    // Single response or client prefers JSON
    res.set_header("Content-Type", "application/json");
    if (responses.size() == 1) {
        res.set_content(responses[0].dump(), "application/json");
    } else {
        // Batch response
        res.set_content(responses.dump(), "application/json");
    }
}

void server::handle_mcp_get(const httplib::Request& req, httplib::Response& res) {
    // CORS headers
    res.set_header("Access-Control-Allow-Origin", "*");
    res.set_header("Access-Control-Allow-Headers", "Content-Type, Accept, Mcp-Session-Id");
    res.set_header("Access-Control-Expose-Headers", "Mcp-Session-Id");


    std::string session_id = req.get_header_value("Mcp-Session-Id");
    if (session_id.empty()) {
        res.status = 400;
        res.set_content("{\"error\":\"Missing Mcp-Session-Id header\"}", "application/json");
        return;
    }

    // Validate session
    std::shared_ptr<event_dispatcher> dispatcher;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        auto it = session_dispatchers_.find(session_id);
        if (it == session_dispatchers_.end()) {
            res.status = 404;
            res.set_content("{\"error\":\"Session not found\"}", "application/json");
            return;
        }
        dispatcher = it->second;
    }

    if (!is_session_initialized(session_id)) {
        res.status = 400;
        res.set_content("{\"error\":\"Session not initialized\"}", "application/json");
        return;
    }

    // Open SSE stream for server-initiated notifications
    res.set_header("Content-Type", "text/event-stream");
    res.set_header("Cache-Control", "no-cache");
    res.set_header("Connection", "keep-alive");

    // Use chunked content provider for server-initiated notifications.
    res.set_chunked_content_provider(
        "text/event-stream",
        [this, session_id, dispatcher](size_t, httplib::DataSink& sink) {
            try {
                if (dispatcher->is_closed() || !running_) {
                    return false;
                }
                dispatcher->update_activity();
                auto result = dispatcher->wait_event_result(&sink, std::chrono::seconds(15));
                if (result == event_dispatcher::wait_result::message) {
                    dispatcher->update_activity();
                    return true;
                }
                if (result == event_dispatcher::wait_result::timeout) {
                    static constexpr char keepalive[] = ": keepalive\r\n\r\n";
                    if (!sink.write(keepalive, sizeof(keepalive) - 1)) {
                        dispatcher->close();
                        return false;
                    }
                    dispatcher->update_activity();
                    return true;
                }
                return false;
            } catch (...) {
                return false;
            }
        });
}

void server::handle_mcp_delete(const httplib::Request& req, httplib::Response& res) {
    res.set_header("Access-Control-Allow-Origin", "*");

    std::string session_id = req.get_header_value("Mcp-Session-Id");
    if (session_id.empty()) {
        res.status = 400;
        return;
    }

    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (session_dispatchers_.find(session_id) == session_dispatchers_.end()) {
            res.status = 404;
            return;
        }
    }

    close_session(session_id);
    res.status = 200;
}

json server::process_request(const request& req, const std::string& session_id) {
    // Check if it is a notification
    if (req.is_notification()) {
        if (req.method == "notifications/initialized") {
            set_session_initialized(session_id, true);
        }
        return json::object();
    }
    
    // Process method call
    try {
        LOG_INFO("Processing method call: ", req.method);
        
        // Special case: initialization
        if (req.method == "initialize") {
            return handle_initialize(req, session_id);
        } else if (req.method == "ping") {
            return response::create_success(req.id, json::object()).to_json();
        }

        if (!is_session_initialized(session_id)) {
            LOG_WARNING("Session not initialized: ", session_id);
            return response::create_error(
                req.id,
                error_code::invalid_request,
                "Session not initialized"
            ).to_json();
        }
        
        // Find registered method handler
        method_handler handler;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            auto it = method_handlers_.find(req.method);
            if (it != method_handlers_.end()) {
                handler = it->second;
            }
        }

        LOG_INFO("Looking for handler for method: '", req.method, "'");

        if (handler) {
            // Call handler
            LOG_INFO("Calling method handler: ", req.method);            
            json result = handler(req.params, session_id);
            
            // Create success response
            LOG_INFO("Method call successful: ", req.method);
            return response::create_success(req.id, result).to_json();
        }
        
        // Method not found
        LOG_WARNING("Method not found: ", req.method);
        return response::create_error(
            req.id,
            error_code::method_not_found,
            "Method not found: " + req.method
        ).to_json();
    } catch (const mcp_exception& e) {
        // MCP exception
        LOG_ERROR("MCP exception: ", e.what(), ", code: ", static_cast<int>(e.code()));
        return response::create_error(
            req.id,
            e.code(),
            e.what()
        ).to_json();
    } catch (const std::exception& e) {
        // Other exceptions
        LOG_ERROR("Exception while processing request: ", e.what());
        return response::create_error(
            req.id,
            error_code::internal_error,
            "Internal error: " + std::string(e.what())
        ).to_json();
    } catch (...) {
        // Unknown exception
        LOG_ERROR("Unknown exception while processing request");
        return response::create_error(
            req.id,
            error_code::internal_error,
            "Unknown internal error"
        ).to_json();
    }
}

json server::handle_initialize(const request& req, const std::string& session_id) {
    const json& params = req.params;

    // Version negotiation
    if (!params.contains("protocolVersion") || !params["protocolVersion"].is_string()) {
        LOG_ERROR("Missing or invalid protocolVersion parameter");
        return response::create_error(
            req.id, 
            error_code::invalid_params, 
            "Expected string for 'protocolVersion' parameter"
        ).to_json();
    }

    std::string requested_version = params["protocolVersion"].get<std::string>();
    LOG_INFO("Client requested protocol version: ", requested_version);

    // If a client requests a version we don't support,
    // respond with the latest version we DO support and let the client decide.
    std::string negotiated_version = MCP_VERSION;
    if (requested_version != MCP_VERSION) {
        LOG_WARNING("Client requested version ", requested_version,
                    ", server supports ", MCP_VERSION, ". Responding with server version.");
    }

    // Extract client info
    std::string client_name = "UnknownClient";
    std::string client_version = "UnknownVersion";

    if (params.contains("clientInfo")) {
        if (params["clientInfo"].contains("name")) {
            client_name = params["clientInfo"]["name"];
        }
        if (params["clientInfo"].contains("version")) {
            client_version = params["clientInfo"]["version"];
        }
    }

    {
        std::lock_guard<std::mutex> lock(mutex_);
        session_client_capabilities_[session_id] = params.value("capabilities", json::object());
    }

    // Log connection
    LOG_INFO("Client connected: ", client_name, " ", client_version);
    
    // Return server info and capabilities
    json server_info = {
        {"name", name_},
        {"version", version_}
    };

    json result = {
        {"protocolVersion", negotiated_version},
        {"capabilities", capabilities_},
        {"serverInfo", server_info}
    };

    if (!instructions_.empty()) {
        result["instructions"] = instructions_;
    }

    LOG_INFO("Initialization successful, waiting for notifications/initialized notification");
    
    return response::create_success(req.id, result).to_json();
}

void server::send_jsonrpc(const std::string& session_id, const json& message) {
    // Check if session ID is valid
    if (session_id.empty()) {
        LOG_WARNING("Cannot send message to empty session_id");
        return;
    }

    // Get session dispatcher
    std::shared_ptr<event_dispatcher> dispatcher;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        auto it = session_dispatchers_.find(session_id);
        if (it == session_dispatchers_.end()) {
            LOG_ERROR("Session not found: ", session_id);
            return;
        }
        dispatcher = it->second;
    }
    
    // Confirm dispatcher is still valid
    if (!dispatcher || dispatcher->is_closed()) {
        LOG_WARNING("Cannot send to closed session: ", session_id);
        return;
    }
    
    // Send message
    std::stringstream ss;
    ss << "event: message\r\ndata: " << message.dump() << "\r\n\r\n";
    bool result = dispatcher->send_event(ss.str());

    if (!result) {
        LOG_ERROR("Failed to send message to session: ", session_id);
    }
}

void server::send_request(const std::string& session_id, const request& req) {
    send_jsonrpc(session_id, req.to_json());
}

json server::request_sampling(
    const std::string& session_id,
    const json& params,
    std::chrono::milliseconds timeout)
{
    if (session_id.empty()) {
        throw mcp_exception(error_code::invalid_request, "Missing session_id for sampling request");
    }

    {
        std::lock_guard<std::mutex> lock(mutex_);
        auto it = session_initialized_.find(session_id);
        if (it == session_initialized_.end() || !it->second) {
            LOG_WARNING("Sampling requested for uninitialized session: ", session_id);
            throw mcp_exception(error_code::invalid_request, "Session not initialized");
        }

        auto capabilities_it = session_client_capabilities_.find(session_id);
        if (capabilities_it == session_client_capabilities_.end() ||
            !capabilities_it->second.is_object() ||
            !capabilities_it->second.contains("sampling")) {
            throw mcp_exception(
                error_code::invalid_request,
                "Client does not advertise sampling capability");
        }
    }

    request req = request::create("sampling/createMessage", params);
    auto pending = std::make_shared<pending_client_request>();

    std::future<json> future = pending->promise.get_future();
    {
        std::lock_guard<std::mutex> lock(mutex_);
        pending_client_requests_[req.id] = pending;
    }

    send_jsonrpc(session_id, req.to_json());

    if (future.wait_for(timeout) != std::future_status::ready) {
        LOG_WARNING("Timed out waiting for sampling response from session: ", session_id);
        std::lock_guard<std::mutex> lock(mutex_);
        auto it = pending_client_requests_.find(req.id);
        if (it != pending_client_requests_.end()) {
            pending_client_requests_.erase(it);
        }
        throw mcp_exception(error_code::internal_error, "Timed out waiting for sampling response");
    }

    return future.get();
}

void server::broadcast_notification(const request& notification) {
    std::vector<std::string> sessions;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        for (const auto& [sid, initialized] : session_initialized_) {
            if (initialized) {
                sessions.push_back(sid);
            }
        }
    }
    for (const auto& sid : sessions) {
        try {
            send_jsonrpc(sid, notification.to_json());
        } catch (...) {
            // Best-effort delivery; don't fail if one session is broken
        }
    }
}

void server::notify_resource_updated(const std::string& uri) {
    request notification = request::create_notification("resources/updated", {{"uri", uri}});

    std::vector<std::string> sessions;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        for (const auto& [session_id, subscriptions] : resource_subscriptions_) {
            auto initialized_it = session_initialized_.find(session_id);
            if (initialized_it != session_initialized_.end() &&
                initialized_it->second &&
                subscriptions.find(uri) != subscriptions.end()) {
                sessions.push_back(session_id);
            }
        }
    }

    for (const auto& session_id : sessions) {
        try {
            send_jsonrpc(session_id, notification.to_json());
        } catch (...) {
            // Best-effort delivery; broken sessions are cleaned up elsewhere.
        }
    }
}

bool server::handle_client_response(const json& message) {
    if (!is_jsonrpc_response_message(message)) {
        return false;
    }

    std::shared_ptr<pending_client_request> pending;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        auto it = pending_client_requests_.find(message["id"]);
        if (it == pending_client_requests_.end()) {
            LOG_WARNING("Received response for unknown request ID: ", message["id"]);
            return false;
        }
        pending = it->second;
        pending_client_requests_.erase(it);
    }

    try {
        if (message.contains("error")) {
            int code = message["error"].value("code", static_cast<int>(error_code::internal_error));
            std::string error_message = message["error"].value("message", "Client sampling error");
            pending->promise.set_exception(std::make_exception_ptr(
                mcp_exception(static_cast<error_code>(code), error_message)));
        } else {
            pending->promise.set_value(message["result"]);
        }
        return true;
    } catch (const std::exception& e) {
        LOG_ERROR("Failed to deliver client response: ", e.what());
        return false;
    }
}

void server::notify_prompts_list_changed() {
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (!capabilities_.is_object() ||
            !capabilities_.contains("prompts") ||
            !capabilities_["prompts"].is_object()) {
            return;
        }

        const json& prompt_capabilities = capabilities_["prompts"];
        auto list_changed_it = prompt_capabilities.find("listChanged");
        if (list_changed_it == prompt_capabilities.end() ||
            !list_changed_it->is_boolean() ||
            !list_changed_it->get<bool>()) {
            return;
        }
    }

    broadcast_notification(request::create_notification("prompts/list_changed"));
}

std::vector<std::string> server::get_active_sessions() const {
    std::vector<std::string> sessions;
    std::lock_guard<std::mutex> lock(mutex_);
    for (const auto& [sid, initialized] : session_initialized_) {
        if (initialized) {
            sessions.push_back(sid);
        }
    }
    return sessions;
}

bool server::is_session_initialized(const std::string& session_id) const {
    // Check if session ID is valid
    if (session_id.empty()) {
        return false;
    }
    
    try {
        std::lock_guard<std::mutex> lock(mutex_);
        auto it = session_initialized_.find(session_id);
        return (it != session_initialized_.end() && it->second);
    } catch (const std::exception& e) {
        LOG_ERROR("Exception checking if session is initialized: ", e.what());
        return false;
    }
}

void server::set_session_initialized(const std::string& session_id, bool initialized) {
    // Check if session ID is valid
    if (session_id.empty()) {
        LOG_WARNING("Cannot set initialization state for empty session_id");
        return;
    }

    try {
        std::lock_guard<std::mutex> lock(mutex_);
        // Check if session still exists (either SSE or HTTP mode)
        auto it = session_dispatchers_.find(session_id);
        bool has_dispatcher = (it != session_dispatchers_.end());

        // For HTTP mode, we also track initialization in session_initialized_ map
        // So we allow setting initialized state even without a dispatcher for HTTP sessions
        if (!has_dispatcher) {
            LOG_DEBUG("Setting initialization state for HTTP session: ", session_id);
        }

        session_initialized_[session_id] = initialized;
    } catch (const std::exception& e) {
        LOG_ERROR("Exception setting session initialization state: ", e.what());
    }
}

void server::touch_session_activity(const std::string& session_id) {
    if (session_id.empty()) {
        return;
    }

    try {
        std::shared_ptr<event_dispatcher> dispatcher;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            auto it = session_dispatchers_.find(session_id);
            if (it != session_dispatchers_.end()) {
                dispatcher = it->second;
            }
        }

        if (dispatcher) {
            dispatcher->update_activity();
        }
    } catch (const std::exception& e) {
        LOG_ERROR("Exception touching session activity: ", e.what());
    }
}

std::string server::generate_session_id() const {
    std::random_device rd;
    std::mt19937 gen(rd());
    std::uniform_int_distribution<> dis(0, 15);

    std::stringstream ss;
    ss << std::hex;

    // UUID format: 8-4-4-4-12 hexadecimal digits
    for (int i = 0; i < 8; ++i) {
        ss << dis(gen);
    }
    ss << "-";

    for (int i = 0; i < 4; ++i) {
        ss << dis(gen);
    }
    ss << "-";

    for (int i = 0; i < 4; ++i) {
        ss << dis(gen);
    }
    ss << "-";

    for (int i = 0; i < 4; ++i) {
        ss << dis(gen);
    }
    ss << "-";

    for (int i = 0; i < 12; ++i) {
        ss << dis(gen);
    }

    return ss.str();
}

void server::check_inactive_sessions() {
    if (!running_ || session_timeout_ == 0) return;

    const auto now = std::chrono::steady_clock::now();
    const auto timeout = std::chrono::seconds(session_timeout_);
    
    std::vector<std::string> sessions_to_close;
    
    {
        std::lock_guard<std::mutex> lock(mutex_);
        for (const auto& [session_id, dispatcher] : session_dispatchers_) {
            if (now - dispatcher->last_activity() > timeout) {
                // Exceeded idle time limit
                sessions_to_close.push_back(session_id);
            }
        }
    }
    
    // Close inactive sessions
    for (const auto& session_id : sessions_to_close) {
        LOG_INFO("Closing inactive session: ", session_id);
        
        close_session(session_id);
    }
}

bool server::set_mount_point(const std::string& mount_point, const std::string& dir, httplib::Headers headers) {
    return http_server_->set_mount_point(mount_point, dir, headers);
}

void server::close_session(const std::string& session_id) {
    // Clean up resources safely
    try {
        for (const auto& [key, handler] : session_cleanup_handler_) {
            handler(key);
        }

        std::shared_ptr<event_dispatcher> dispatcher_to_close;

        {
            std::lock_guard<std::mutex> lock(mutex_);

            // Get dispatcher pointer
            auto dispatcher_it = session_dispatchers_.find(session_id);
            if (dispatcher_it != session_dispatchers_.end()) {
                dispatcher_to_close = dispatcher_it->second;
                session_dispatchers_.erase(dispatcher_it);
            }

            session_initialized_.erase(session_id);
            session_client_capabilities_.erase(session_id);
            resource_subscriptions_.erase(session_id);
        }

        // Close dispatcher outside the lock
        if (dispatcher_to_close && !dispatcher_to_close->is_closed()) {
            dispatcher_to_close->close();
        }
        
    } catch (const std::exception& e) {
        LOG_WARNING("Exception while cleaning up session resources: ", session_id, ", ", e.what());
    } catch (...) {
        LOG_WARNING("Unknown exception while cleaning up session resources: ", session_id);
    }
}

} // namespace mcp
