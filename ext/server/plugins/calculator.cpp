/**
 * @file calculator.cpp
 * @brief Calculator Tool Plugin
 */

#include "tool_api.h"
#include <string>
#include <nlohmann/json.hpp>

using json = nlohmann::json;

static char* handleCalculator(int tool_index, const json& req) {
    std::string op = req.value("operation", "");
    double a = req.value("a", 0.0);
    double b = req.value("b", 0.0);

    double result = 0.0;
    if (op == "add") {
        result = a + b;
    } else if (op == "subtract") {
        result = a - b;
    } else if (op == "multiply") {
        result = a * b;
    } else if (op == "divide") {
        if (b == 0.0) throw std::runtime_error("Division by zero");
        result = a / b;
    } else {
        throw std::runtime_error("Unknown operation: " + op);
    }

    json content_item;
    content_item["type"] = "text";
    content_item["text"] = std::to_string(result);

    json response;
    response["content"] = json::array({content_item});
    response["isError"] = false;

    return strdup(response.dump().c_str());
}

static char* handleRequest(int tool_index, const char* request_json) {
    try {
        json req = json::parse(request_json);
        return handleCalculator(tool_index, req);
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
        "operation": {
            "type": "string",
            "description": "Arithmetic operation",
            "enum": ["add", "subtract", "multiply", "divide"]
        },
        "a": {
            "type": "number",
            "description": "First operand"
        },
        "b": {
            "type": "number",
            "description": "Second operand"
        }
    },
    "required": ["operation", "a", "b"]
})SCHEMA";

static ToolPlugin calculator_tool = {
    "calculator",
    "Perform basic arithmetic calculations (add, subtract, multiply, divide)",
    INPUT_SCHEMA
};

static ToolPluginAPI plugin_api = {
    &calculator_tool,
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