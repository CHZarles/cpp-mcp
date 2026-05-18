/**
 * @file wsl_list_distros.cpp
 * @brief WSL List Distros Tool
 *
 * Lists the Linux distribution running inside WSL.
 */

#include "tool_api.h"
#include <string>
#include <cstdlib>
#include <array>
#include <fstream>
#include <nlohmann/json.hpp>

using json = nlohmann::json;

static char* create_error_response(const std::string& message) {
    json content_item;
    content_item["type"] = "text";
    content_item["text"] = message;

    json response;
    response["content"] = json::array({content_item});
    response["isError"] = false;
    return strdup(response.dump().c_str());
}

static char* handleListDistros(const json& req) {
    (void)req;

    // Read /etc/os-release to get distribution info
    std::ifstream os_release("/etc/os-release");
    if (!os_release.is_open()) {
        return create_error_response("Cannot read /etc/os-release");
    }

    std::string name, version, pretty_name;
    std::string line;
    while (std::getline(os_release, line)) {
        if (line.find("NAME=") == 0) {
            name = line.substr(5);
            // Remove quotes
            if (name.size() >= 2 && name.front() == '"' && name.back() == '"') {
                name = name.substr(1, name.size() - 2);
            }
        } else if (line.find("VERSION=") == 0) {
            version = line.substr(8);
            if (version.size() >= 2 && version.front() == '"' && version.back() == '"') {
                version = version.substr(1, version.size() - 2);
            }
        } else if (line.find("PRETTY_NAME=") == 0) {
            pretty_name = line.substr(12);
            if (pretty_name.size() >= 2 && pretty_name.front() == '"' && pretty_name.back() == '"') {
                pretty_name = pretty_name.substr(1, pretty_name.size() - 2);
            }
        }
    }
    os_release.close();

    if (pretty_name.empty()) {
        if (name.empty()) {
            return create_error_response("Cannot determine distribution");
        }
        pretty_name = name + (version.empty() ? "" : " " + version);
    }

    // Get kernel info
    std::ifstream kernel_version("/proc/version");
    std::string kernel;
    if (kernel_version.is_open()) {
        std::getline(kernel_version, line);
        kernel_version.close();
        // Extract just the version number
        size_t pos = line.find("version");
        if (pos != std::string::npos) {
            kernel = line.substr(pos + 8);
            pos = kernel.find(" ");
            if (pos != std::string::npos) {
                kernel = kernel.substr(0, pos);
            }
        }
    }

    // Get architecture
    std::ifstream cpuinfo("/proc/cpuinfo");
    std::string arch = "unknown";
    while (std::getline(cpuinfo, line)) {
        if (line.find("model name") == 0) {
            size_t pos = line.find(":");
            if (pos != std::string::npos) {
                arch = line.substr(pos + 2);
                break;
            }
        }
    }
    cpuinfo.close();

    // Build response
    json content_item;
    content_item["type"] = "text";

    std::string output = "WSL Distribution Information:\n";
    output += "Name: " + pretty_name + "\n";
    if (!version.empty() && version != pretty_name) {
        output += "Version: " + version + "\n";
    }
    output += "Kernel: " + (kernel.empty() ? "unknown" : kernel) + "\n";
    output += "Architecture: " + arch + "\n";

    content_item["text"] = output;

    json response;
    response["content"] = json::array({content_item});
    response["isError"] = false;
    return strdup(response.dump().c_str());
}

// Export for use in main plugin file
extern "C" char* wsl_list_distros_handler(const json& req) {
    return handleListDistros(req);
}