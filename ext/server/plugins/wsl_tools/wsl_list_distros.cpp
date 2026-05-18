/**
 * @file wsl_list_distros.cpp
 * @brief WSL List Distros Tool
 */

#include "tool_api.h"
#include <string>
#include <cstdlib>
#include <iostream>
#include <fstream>
#include <array>
#include <nlohmann/json.hpp>

using json = nlohmann::json;

// Tool implementation
static char* handleListDistros(const json& req) {
    (void)req;

    // Check if WSL is available
    int result = system("wsl.exe --help > /dev/null 2>&1");
    if (result != 0) {
        json content_item;
        content_item["type"] = "text";
        content_item["text"] = "WSL is not installed or not available";

        json response;
        response["content"] = json::array({content_item});
        response["isError"] = false;
        return strdup(response.dump().c_str());
    }

    // Run wsl --list --verbose
    std::string cmd = "wsl.exe --list --verbose 2>/dev/null | tail -n +2 | head -20";
    std::array<char, 256> buffer;
    std::string output;

    FILE* pipe = popen(cmd.c_str(), "r");
    if (!pipe) {
        json content_item;
        content_item["type"] = "text";
        content_item["text"] = "Failed to execute wsl command";

        json response;
        response["content"] = json::array({content_item});
        response["isError"] = false;
        return strdup(response.dump().c_str());
    }

    while (fgets(buffer.data(), buffer.size(), pipe) != nullptr) {
        output += buffer.data();
    }
    pclose(pipe);

    json content_item;
    content_item["type"] = "text";

    if (output.empty()) {
        content_item["text"] = "No WSL distributions found";
    } else {
        while (!output.empty() && (output.back() == '\n' || output.back() == '\r')) {
            output.pop_back();
        }
        content_item["text"] = "WSL Distributions:\n" + output;
    }

    json response;
    response["content"] = json::array({content_item});
    response["isError"] = false;
    return strdup(response.dump().c_str());
}

// Export for use in main plugin file
extern "C" char* wsl_list_distros_handler(const json& req) {
    return handleListDistros(req);
}