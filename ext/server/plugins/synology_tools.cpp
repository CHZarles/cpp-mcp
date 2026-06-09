/**
 * @file synology_tools.cpp
 * @brief Adapter plugin that proxies tool calls to the synology-api-backend
 *        HTTP service.
 *
 * Tool schemas are fetched once from the backend's GET /tools endpoint when
 * the plugin loads, so the backend remains the single source of truth.
 * Each HandleRequest invocation forwards the call to POST /tools/call and
 * returns the backend's MCP-shaped response verbatim.
 *
 * Environment variables:
 *   SYNOLOGY_BACKEND_URL     Base URL of the backend. Default http://127.0.0.1:9000
 *   SYNOLOGY_BACKEND_TOKEN   Bearer token (required)
 *   SYNOLOGY_BACKEND_TIMEOUT Request timeout in seconds. Default 120.
 */

#include "tool_api.h"
#include "plugin_helpers.h"

#include <httplib.h>
#include <nlohmann/json.hpp>

#include <cstdlib>
#include <cstring>
#include <iostream>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

using json = nlohmann::json;

namespace {

struct BackendConfig {
    std::string scheme;
    std::string host;
    int port;
    std::string token;
    int timeout_seconds;
};

// Live for the entire plugin lifetime. ToolPlugin holds const char* into
// these vectors, so they must not be reallocated after CreateToolPlugin
// returns.
struct PluginState {
    BackendConfig config{};
    std::vector<std::string> name_storage;
    std::vector<std::string> desc_storage;
    std::vector<std::string> schema_storage;
    std::vector<ToolPlugin> tools;
    std::mutex client_mutex;
};

PluginState g_state;
ToolPluginAPI g_api{};

std::string env_or(const char* name, const std::string& fallback) {
    const char* value = std::getenv(name);
    return (value && *value) ? std::string(value) : fallback;
}

bool parse_url(const std::string& url, BackendConfig& out) {
    // Accept: http://host[:port] or https://host[:port]
    auto scheme_end = url.find("://");
    if (scheme_end == std::string::npos) return false;
    out.scheme = url.substr(0, scheme_end);
    if (out.scheme != "http" && out.scheme != "https") return false;

    std::string host_port = url.substr(scheme_end + 3);
    // Strip any trailing path.
    auto slash = host_port.find('/');
    if (slash != std::string::npos) host_port.resize(slash);
    if (host_port.empty()) return false;

    auto colon = host_port.find(':');
    if (colon == std::string::npos) {
        out.host = host_port;
        out.port = (out.scheme == "https") ? 443 : 80;
    } else {
        out.host = host_port.substr(0, colon);
        try {
            out.port = std::stoi(host_port.substr(colon + 1));
        } catch (const std::exception&) {
            return false;
        }
    }
    return !out.host.empty() && out.port > 0 && out.port < 65536;
}

bool load_config(BackendConfig& cfg) {
    std::string url = env_or("SYNOLOGY_BACKEND_URL", "http://127.0.0.1:9000");
    if (!parse_url(url, cfg)) {
        std::cerr << "[synology_tools] Invalid SYNOLOGY_BACKEND_URL: " << url << std::endl;
        return false;
    }

    const char* token = std::getenv("SYNOLOGY_BACKEND_TOKEN");
    if (!token || !*token) {
        std::cerr << "[synology_tools] SYNOLOGY_BACKEND_TOKEN is required" << std::endl;
        return false;
    }
    cfg.token = token;

    std::string timeout_str = env_or("SYNOLOGY_BACKEND_TIMEOUT", "120");
    try {
        cfg.timeout_seconds = std::stoi(timeout_str);
    } catch (const std::exception&) {
        std::cerr << "[synology_tools] SYNOLOGY_BACKEND_TIMEOUT must be an integer" << std::endl;
        return false;
    }
    if (cfg.timeout_seconds <= 0) cfg.timeout_seconds = 120;
    return true;
}

std::unique_ptr<httplib::Client> make_client(const BackendConfig& cfg) {
    auto client = std::make_unique<httplib::Client>(cfg.host, cfg.port);
#ifdef CPPHTTPLIB_OPENSSL_SUPPORT
    if (cfg.scheme == "https") {
        client->enable_server_certificate_verification(true);
    }
#endif
    client->set_bearer_token_auth(cfg.token);
    client->set_connection_timeout(10, 0);
    client->set_read_timeout(cfg.timeout_seconds, 0);
    client->set_write_timeout(cfg.timeout_seconds, 0);
    client->set_keep_alive(false);
    return client;
}

bool fetch_tool_schemas(PluginState& state) {
    auto client = make_client(state.config);
    auto res = client->Get("/tools");
    if (!res) {
        std::cerr << "[synology_tools] GET /tools failed: "
                  << httplib::to_string(res.error()) << std::endl;
        return false;
    }
    if (res->status != 200) {
        std::cerr << "[synology_tools] GET /tools returned HTTP " << res->status
                  << ": " << res->body << std::endl;
        return false;
    }

    json doc;
    try {
        doc = json::parse(res->body);
    } catch (const std::exception& e) {
        std::cerr << "[synology_tools] Invalid JSON from /tools: " << e.what() << std::endl;
        return false;
    }

    if (!doc.is_object() || !doc.contains("tools") || !doc["tools"].is_array()) {
        std::cerr << "[synology_tools] /tools response missing 'tools' array" << std::endl;
        return false;
    }

    const auto& tools = doc["tools"];
    state.name_storage.reserve(tools.size());
    state.desc_storage.reserve(tools.size());
    state.schema_storage.reserve(tools.size());
    state.tools.reserve(tools.size());

    for (const auto& tool : tools) {
        if (!tool.is_object() || !tool.contains("name") || !tool["name"].is_string()) continue;

        state.name_storage.push_back(tool["name"].get<std::string>());
        state.desc_storage.push_back(tool.value("description", std::string{}));
        json schema = tool.value("inputSchema", json::object());
        state.schema_storage.push_back(schema.dump());

        ToolPlugin entry{};
        entry.name = state.name_storage.back().c_str();
        entry.description = state.desc_storage.back().c_str();
        entry.inputSchema = state.schema_storage.back().c_str();
        state.tools.push_back(entry);
    }

    return !state.tools.empty();
}

char* call_backend(int tool_index, const json& req) {
    if (tool_index < 0 || tool_index >= static_cast<int>(g_state.tools.size())) {
        return mcp_ext::plugin::make_error_result(
            "Unknown tool index: " + std::to_string(tool_index));
    }

    json payload;
    payload["name"] = g_state.name_storage[tool_index];
    payload["arguments"] = req.is_null() ? json::object() : req;

    httplib::Result res;
    {
        // httplib::Client is not safe for concurrent requests; serialize.
        std::lock_guard<std::mutex> lock(g_state.client_mutex);
        auto client = make_client(g_state.config);
        res = client->Post("/tools/call", payload.dump(), "application/json");
    }

    if (!res) {
        return mcp_ext::plugin::make_error_result(
            "Backend request failed: " + httplib::to_string(res.error()));
    }

    // 2xx — pass the backend's MCP-shaped body straight through.
    if (res->status >= 200 && res->status < 300) {
        return strdup(res->body.c_str());
    }

    return mcp_ext::plugin::make_error_result(
        "Backend HTTP " + std::to_string(res->status) + ": " + res->body);
}

char* handleRequest(int tool_index, const char* request_json) {
    try {
        json req = (request_json && *request_json) ? json::parse(request_json) : json::object();
        return call_backend(tool_index, req);
    } catch (const std::exception& e) {
        return mcp_ext::plugin::make_error_result(e.what());
    }
}

}  // namespace

extern "C" {

TOOL_PLUGIN_API ToolPluginAPI* CreateToolPlugin() {
    if (!load_config(g_state.config)) {
        return nullptr;
    }
    if (!fetch_tool_schemas(g_state)) {
        std::cerr << "[synology_tools] No tools registered. "
                     "Is synology-api-backend running and reachable?" << std::endl;
        return nullptr;
    }

    g_api.tools = g_state.tools.data();
    g_api.tool_count = static_cast<int>(g_state.tools.size());
    g_api.HandleRequest = handleRequest;
    return &g_api;
}

TOOL_PLUGIN_API void DestroyToolPlugin(ToolPluginAPI*) {
}

}  // extern "C"
