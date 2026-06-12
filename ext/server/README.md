# MCP Extended Server

基于 cpp-mcp 的扩展服务示例。当前实现提供一个独立的 MCP Streamable HTTP server，并在启动时把动态库插件注册为 MCP tools。

当前插件 ABI 只覆盖 Tools。Resources、Prompts、Sampling、热插拔和插件版本管理尚未实现为插件 ABI。

## 架构

```
┌─────────────────────────────────────────────────────────────┐
│                    MCP Extended Server                      │
│                                                             │
│  ┌──────────────────────────────────────────────────────┐   │
│  │                  mcp-ext-server                       │   │
│  │  - 启动 cpp-mcp server                                │   │
│  │  - 设置 tools capability                               │   │
│  │  - 将插件工具注册为 MCP tools                         │   │
│  └──────────────────────────────────────────────────────┘   │
│                          │                                  │
│  ┌──────────────────────────────────────────────────────┐   │
│  │                   PluginLoader                        │   │
│  │  - 扫描插件目录的一层文件                              │   │
│  │  - 只加载文件名匹配 lib*.so 的动态库                   │   │
│  │  - 通过 CreateToolPlugin / DestroyToolPlugin 加载 ABI  │   │
│  └──────────────────────────────────────────────────────┘   │
│                          │                                  │
│  ┌───────────────┬───────────────────┬───────────────┐    │
│  │libcalculator.so│libsynology_tools.so│ libother.so   │    │
│  │   (1 tool)    │     (N tools)      │   (N tools)   │    │
│  └───────────────┴───────────────────┴───────────────┘    │
└─────────────────────────────────────────────────────────────┘
```

## 构建

从 cpp-mcp 仓库根目录构建，需要打开 `MCP_BUILD_EXT`：

```bash
cmake -B build -DMCP_BUILD_EXT=ON
cmake --build build --config Release
```

`synology_tools` is an opt-in usage-example plugin. Build it only when running
the Synology NAS example:

```bash
cmake -B build -DMCP_BUILD_EXT=ON -DMCP_BUILD_SYNOLOGY_EXAMPLE=ON
cmake --build build --target mcp-ext-server synology_tools
```

The Python backend lives in `ext/server/plugins/synology/backend`; see
`ext/server/plugins/synology/` for runtime configuration.

也可以单独配置 `ext/server`，但它仍需要能链接到 `mcp` 库；日常开发建议使用上面的根目录构建方式：

```bash
cmake -S ext/server -B ext/server/build
cmake --build ext/server/build --config Release
```

根目录构建时，插件库会输出到：

```text
build/plugins/
```

运行：

```bash
./build/ext/server/mcp-ext-server
```

默认监听：

```text
localhost:8888
```

## 插件发现规则

服务启动时会先创建当前工作目录下的 `./plugins`，然后选择插件目录：

- 默认使用当前工作目录的 `./plugins`。
- Linux 下如果可执行文件所在目录的 `../plugins` 存在，则优先使用该目录。
- 根目录构建并运行 `./build/ext/server/mcp-ext-server` 时，通常会加载 `build/plugins`。

加载器当前只扫描插件目录的一层普通文件，不递归扫描子目录。插件文件名必须以 `lib` 开头，并包含 `.so`，例如：

```text
libcalculator.so
libsynology_tools.so
```

当前实现使用 `dlopen` / `dlsym`，因此实际支持的是 Linux `.so` 插件，不是 Windows `.dll`。

## 目录结构

```text
ext/server/
├── CMakeLists.txt
├── README.md
├── include/
│   └── mcp_ext/
│       ├── tool_api.h           # 插件 C ABI
│       └── plugin_helpers.h     # 插件返回值构造 helper
├── src/
│   ├── main.cpp                 # MCP server 启动编排
│   ├── plugin_loader.h          # 插件加载器声明
│   ├── plugin_loader.cpp        # dlopen/dlsym 加载实现
│   ├── plugin_registry.h        # 插件工具注册声明
│   └── plugin_registry.cpp      # ToolPluginAPI -> MCP tools 注册逻辑
└── plugins/
    ├── default/
    │   └── calculator.cpp       # 最简单的单工具插件示例
    ├── synology/
    │   ├── synology_tools.cpp   # Synology HTTP 后端 adapter 插件
    │   └── backend/             # Synology Python HTTP 后端
```

## 插件 ABI

插件必须包含 `mcp_ext/tool_api.h` 并导出两个 C 符号：

```c
ToolPluginAPI* CreateToolPlugin();
void DestroyToolPlugin(ToolPluginAPI* plugin);
```

核心结构：

```c
typedef struct {
    const char* name;
    const char* description;
    const char* inputSchema;
} ToolPlugin;

typedef struct {
    ToolPlugin* tools;
    int tool_count;
    char* (*HandleRequest)(int tool_index, const char* request_json);
} ToolPluginAPI;
```

约定：

- 一个 `.so` 可以暴露一个或多个 tool。
- `tools` 指向 `ToolPlugin` 数组，`tool_count` 是数组长度。
- `inputSchema` 必须是合法 JSON Schema 字符串，server 注册工具时会解析它。
- `HandleRequest` 的 `tool_index` 对应 `tools[tool_index]`。
- `request_json` 是 MCP tool call 的 `params` JSON 字符串。
- `HandleRequest` 必须返回一个堆分配的 C 字符串，主程序会用 `free()` 释放。
- 当前示例使用 `strdup(response.dump().c_str())`，与主程序的 `free()` 匹配。

## Tool 返回格式

插件返回值必须是可解析的 JSON 字符串，并且应符合 MCP tool result 结构。例如：

```json
{
  "content": [
    {
      "type": "text",
      "text": "result text"
    }
  ],
  "isError": false
}
```

错误也应通过同样结构返回：

```json
{
  "content": [
    {
      "type": "text",
      "text": "Error: something failed"
    }
  ],
  "isError": true
}
```

## 单工具插件示例

```cpp
#include "mcp_ext/tool_api.h"
#include "mcp_ext/plugin_helpers.h"
#include <exception>
#include <nlohmann/json.hpp>
#include <string>

using json = nlohmann::json;

static char* handleRequest(int tool_index, const char* request_json) {
    (void)tool_index;

    try {
        json req = json::parse(request_json);
        std::string name = req.value("name", "World");
        return mcp_ext::plugin::make_text_result("Hello, " + name);
    } catch (const std::exception& e) {
        return mcp_ext::plugin::make_error_result(e.what());
    }
}

static const char* INPUT_SCHEMA = R"SCHEMA({
    "type": "object",
    "properties": {
        "name": {
            "type": "string",
            "description": "Name to greet"
        }
    },
    "required": []
})SCHEMA";

static ToolPlugin tool = {
    "hello",
    "Return a greeting message.",
    INPUT_SCHEMA
};

static ToolPluginAPI plugin_api = {
    &tool,
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
```

对应 CMake：

```cmake
add_library(hello SHARED plugins/hello.cpp)
target_include_directories(hello PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/include)
target_link_libraries(hello PRIVATE nlohmann_json::nlohmann_json)
set_target_properties(hello PROPERTIES
    LIBRARY_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/plugins
)
```

## 现有插件

`plugins/default` 存放最简单的功能插件。当前 `calculator` 提供一个
`calculator` tool，支持：

- `add`
- `subtract`
- `multiply`
- `divide`

`plugins/synology` 是 Synology 插件族，包含 `synology_tools.cpp` C++
adapter 和 `backend/` Python HTTP 后端。该插件默认不构建，只在
`MCP_BUILD_SYNOLOGY_EXAMPLE=ON` 时启用。

## 当前限制

- 插件 ABI 只支持 Tools；Resources 和 Prompts 还不能由插件动态声明。
- `inputSchema` 和插件返回 JSON 必须合法，否则会在注册或调用阶段抛出解析异常。
- 加载器会跳过无效插件并继续加载其他插件，但 `loadPlugins()` 只在插件目录不存在时返回 `false`。

## 未来扩展

- [ ] 通用资源插件 ABI (Resources)
- [ ] 提示词插件 (Prompts)
- [ ] 热插拔支持 (文件监控)
- [ ] 插件依赖管理
- [ ] 插件版本控制
