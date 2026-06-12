#ifndef MCP_EXT_PLUGIN_HELPERS_H
#define MCP_EXT_PLUGIN_HELPERS_H

#include <cstring>
#include <nlohmann/json.hpp>
#include <string>

namespace mcp_ext::plugin {

inline char* make_result(const std::string& text, bool is_error) {
    nlohmann::json content;
    content["type"] = "text";
    content["text"] = text;

    nlohmann::json response;
    response["content"] = nlohmann::json::array({content});
    response["isError"] = is_error;

    return strdup(response.dump().c_str());
}

inline char* make_text_result(const std::string& text) {
    return make_result(text, false);
}

inline char* make_error_result(const std::string& text) {
    return make_result("Error: " + text, true);
}

inline char* make_json_result(const nlohmann::json& value, bool is_error = false) {
    return make_result(value.dump(2), is_error);
}

} // namespace mcp_ext::plugin

#endif // MCP_EXT_PLUGIN_HELPERS_H
