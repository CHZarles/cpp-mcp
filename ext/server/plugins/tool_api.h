#ifndef TOOL_PLUGIN_API_H
#define TOOL_PLUGIN_API_H

#define TOOL_PLUGIN_API __attribute__((visibility("default")))

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Single tool definition
 */
typedef struct {
    const char* name;           // Tool name (e.g., "wsl_create_directory")
    const char* description;     // Tool description
    const char* inputSchema;     // JSON Schema for tool arguments
} ToolPlugin;

/**
 * @brief Plugin API - each .so provides one or more related tools
 *
 * Design principle: One .so = one family of related tools, each tool is independent.
 * For example, wsl_create_directory.so provides exactly one tool "wsl_create_directory"
 */
typedef struct {
    ToolPlugin* tools;          // Array of tool definitions
    int tool_count;             // Number of tools (usually 1)
    /**
     * @brief Handle a request for a specific tool
     * @param tool_index Which tool in the tools array (0-based)
     * @param request_json JSON string with tool arguments
     * @return Response JSON string (caller must free with free())
     */
    char* (*HandleRequest)(int tool_index, const char* request_json);
} ToolPluginAPI;

/**
 * @brief Create a plugin instance
 * @return ToolPluginAPI pointer
 */
TOOL_PLUGIN_API ToolPluginAPI* CreateToolPlugin();

/**
 * @brief Destroy a plugin instance
 * @param plugin Plugin instance to destroy
 */
TOOL_PLUGIN_API void DestroyToolPlugin(ToolPluginAPI* plugin);

#ifdef __cplusplus
}
#endif

#endif // TOOL_PLUGIN_API_H