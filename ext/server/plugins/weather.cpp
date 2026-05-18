/**
 * @file weather.cpp
 * @brief Weather Tool Plugin
 */

#include "tool_api.h"
#include <string>
#include <nlohmann/json.hpp>

using json = nlohmann::json;

static char* handleWeather(int tool_index, const json& req) {
    std::string city = req.value("city", "");

    json weather_data;
    weather_data["city"] = city;
    weather_data["temperature"] = 22.5;
    weather_data["humidity"] = 65;
    weather_data["condition"] = "Sunny";

    json content_item;
    content_item["type"] = "text";
    content_item["text"] = weather_data.dump(2);

    json response;
    response["content"] = json::array({content_item});
    response["isError"] = false;

    return strdup(response.dump().c_str());
}

static char* handleRequest(int tool_index, const char* request_json) {
    try {
        json req = json::parse(request_json);
        return handleWeather(tool_index, req);
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
    "properties": {
        "city": {
            "type": "string",
            "description": "City name to get weather for"
        }
    },
    "required": ["city"]
})SCHEMA";

static ToolPlugin weather_tool = {
    "weather",
    "Get weather information for a city (simulated)",
    INPUT_SCHEMA
};

static ToolPluginAPI plugin_api = {
    &weather_tool,
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