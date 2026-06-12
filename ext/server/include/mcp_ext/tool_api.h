#ifndef MCP_EXT_TOOL_API_H
#define MCP_EXT_TOOL_API_H

#define TOOL_PLUGIN_API __attribute__((visibility("default")))

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Single tool definition
 */
typedef struct {
    const char* name;           // Tool name (e.g., "calculator")
    const char* description;     // Tool description
    const char* inputSchema;     // JSON Schema for tool arguments
} ToolPlugin;

/**
 * @brief Plugin API - each .so provides one or more related tools
 *
 * Design principle: one shared library should expose a cohesive family of tools.
 * A plugin may expose a single tool, or multiple related tools through the
 * tools array. The server passes tool_index to HandleRequest so the plugin can
 * dispatch each tool independently.
 */
typedef struct {
    ToolPlugin* tools;          // Array of tool definitions
    int tool_count;             // Number of tools exposed by this plugin
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

#endif // MCP_EXT_TOOL_API_H
