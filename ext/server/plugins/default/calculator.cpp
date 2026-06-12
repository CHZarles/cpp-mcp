/**
 * @file calculator.cpp
 * @brief Calculator Tool Plugin
 */

#include "mcp_ext/tool_api.h"
#include "mcp_ext/plugin_helpers.h"
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

    return mcp_ext::plugin::make_text_result(std::to_string(result));
}

static char* handleRequest(int tool_index, const char* request_json) {
    try {
        json req = json::parse(request_json);
        return handleCalculator(tool_index, req);
    } catch (const std::exception& e) {
        return mcp_ext::plugin::make_error_result(e.what());
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
