#ifndef MCP_EXT_PLUGIN_REGISTRY_H
#define MCP_EXT_PLUGIN_REGISTRY_H

#include "mcp_server.h"
#include "plugin_loader.h"

namespace mcp_ext {

void register_plugin_tools(mcp::server& server, const PluginLoader& loader);

} // namespace mcp_ext

#endif // MCP_EXT_PLUGIN_REGISTRY_H
