/**
 * @file wsl_list_distros.cpp
 * @brief WSL List Distributions Tool
 */

#include "tool_api.h"
#include <string>
#include <cstdlib>
#include <vector>
#include <nlohmann/json.hpp>

using json = nlohmann::json;

class WSLDistroManager {
public:
    static std::string get_os_info() {
        std::string info;
        FILE* f = fopen("/etc/os-release", "r");
        if (f) {
            char buf[256];
            while (fgets(buf, sizeof(buf), f)) {
                info += buf;
            }
            fclose(f);
        }
        return info.empty() ? "Unknown Linux distribution" : info;
    }
};

static char* handleListDistros(int tool_index, const json& req) {
    std::string os_info = WSLDistroManager::get_os_info();

    json content_item;
    content_item["type"] = "text";
    content_item["text"] = "Linux Distribution Info:\n" + os_info;

    json response;
    response["content"] = json::array({content_item});
    response["isError"] = false;

    return strdup(response.dump().c_str());
}

static char* handleRequest(int tool_index, const char* request_json) {
    try {
        json req = json::parse(request_json);
        return handleListDistros(tool_index, req);
    } catch (const std::exception& e) {
        json content_item;
        content_item["type"] = "text";
        content_item["text"] = std::string("Error: ") + e.what();

        json response;
        response["content"] = json::array({content_item});
        response["isError"] = true;

        return strdup(response.dump().c_str());
    }
}

static const char* INPUT_SCHEMA = R"SCHEMA({
    "type": "object",
    "properties": {},
    "required": []
})SCHEMA";

static ToolPlugin list_distros_tool = {
    "wsl_list_distros",
    "List all available WSL distributions on the system.",
    INPUT_SCHEMA
};

static ToolPluginAPI plugin_api = {
    &list_distros_tool,
    1,
    handleRequest
};

extern "C" {
    TOOL_PLUGIN_API ToolPluginAPI* CreateToolPlugin() {
        return &plugin_api;
    }

    TOOL_PLUGIN_API void DestroyToolPlugin(ToolPluginAPI*) {
    }
}