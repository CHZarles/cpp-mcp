/**
 * @file mcp_prompt.h
 * @brief Prompt definitions for MCP servers.
 */

#ifndef MCP_PROMPT_H
#define MCP_PROMPT_H

#include "mcp_message.h"

#include <string>
#include <vector>

namespace mcp {

struct prompt_argument {
    std::string name;
    std::string description;
    bool required = false;
    std::string title;
    json metadata = json::object();

    json to_json() const {
        json result = {
            {"name", name},
            {"required", required}
        };

        if (!title.empty()) {
            result["title"] = title;
        }

        if (!description.empty()) {
            result["description"] = description;
        }

        if (!metadata.empty()) {
            result["_meta"] = metadata;
        }

        return result;
    }
};

struct prompt {
    std::string name;
    std::string description;
    std::vector<prompt_argument> arguments;
    std::string title;
    json icons = json::array();
    json metadata = json::object();

    json to_json() const {
        json result = {{"name", name}};

        if (!title.empty()) {
            result["title"] = title;
        }

        if (!description.empty()) {
            result["description"] = description;
        }

        if (!icons.empty()) {
            result["icons"] = icons;
        }

        if (!arguments.empty()) {
            json args = json::array();
            for (const auto& argument : arguments) {
                args.push_back(argument.to_json());
            }
            result["arguments"] = args;
        }

        if (!metadata.empty()) {
            result["_meta"] = metadata;
        }

        return result;
    }
};

} // namespace mcp

#endif // MCP_PROMPT_H
