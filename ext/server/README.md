# MCP Extended Server

基于 cpp-mcp 的扩展服务示例。当前实现提供一个独立的 MCP Streamable HTTP server，并在启动时把动态库插件注册为 MCP tools。

当前插件 ABI 只覆盖 Tools。ext server 额外注册了 WSL 扫描报告的 Resource Templates；Prompts、Sampling、热插拔和插件版本管理尚未实现。

## 架构

```
┌─────────────────────────────────────────────────────────────┐
│                    MCP Extended Server                      │
│                                                             │
│  ┌──────────────────────────────────────────────────────┐   │
│  │                  mcp-ext-server                       │   │
│  │  - 启动 cpp-mcp server                                │   │
│  │  - 设置 tools/resources capability                     │   │
│  │  - 将插件工具注册为 MCP tools                         │   │
│  │  - 注册 WSL scan report resource templates             │   │
│  └──────────────────────────────────────────────────────┘   │
│                          │                                  │
│  ┌──────────────────────────────────────────────────────┐   │
│  │                   PluginLoader                        │   │
│  │  - 扫描插件目录的一层文件                              │   │
│  │  - 只加载文件名匹配 lib*.so 的动态库                   │   │
│  │  - 通过 CreateToolPlugin / DestroyToolPlugin 加载 ABI  │   │
│  └──────────────────────────────────────────────────────┘   │
│                          │                                  │
│  ┌───────────────┬───────────────┬───────────────┐         │
│  │libcalculator.so│libwsl_tools.so│  libother.so  │         │
│  │   (1 tool)    │   (2 tools)   │   (N tools)   │         │
│  └───────────────┴───────────────┴───────────────┘         │
└─────────────────────────────────────────────────────────────┘
```

## 构建

从 cpp-mcp 仓库根目录构建，需要打开 `MCP_BUILD_EXT`：

```bash
cmake -B build -DMCP_BUILD_EXT=ON
cmake --build build --config Release
```

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
libwsl_tools.so
```

当前实现使用 `dlopen` / `dlsym`，因此实际支持的是 Linux `.so` 插件，不是 Windows `.dll`。

## 目录结构

```text
ext/server/
├── CMakeLists.txt
├── README.md
├── src/
│   ├── main.cpp                 # MCP server 启动编排
│   ├── plugin_loader.h          # 插件加载器声明
│   ├── plugin_loader.cpp        # dlopen/dlsym 加载实现
│   ├── plugin_registry.h        # 插件工具注册声明
│   ├── plugin_registry.cpp      # ToolPluginAPI -> MCP tools 注册逻辑
│   ├── wsl_resources.h          # WSL Resource Template 注册声明
│   └── wsl_resources.cpp        # wsl://scan/{scan_id}/... 资源读取逻辑
└── plugins/
    ├── tool_api.h               # 插件 C ABI
    ├── plugin_helpers.h          # 插件返回值构造 helper
    ├── calculator.cpp           # 单工具插件示例
    └── wsl_tools/
        ├── wsl_tools.cpp         # 多工具插件入口和分发逻辑
        ├── wsl_common.h          # WSL 工具公共路径校验逻辑
        ├── wsl_create_directory.cpp
        ├── wsl_list_distros.cpp
        ├── wsl_scan_files.cpp
        ├── wsl_recommend_cleanup.cpp
        └── wsl_safe_delete.cpp
```

## 插件 ABI

插件必须包含 `tool_api.h` 并导出两个 C 符号：

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
#include "tool_api.h"
#include "plugin_helpers.h"
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
target_include_directories(hello PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/plugins)
target_link_libraries(hello PRIVATE nlohmann_json::nlohmann_json)
set_target_properties(hello PROPERTIES
    LIBRARY_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/plugins
)
```

## 多工具插件模式

`wsl_tools` 是当前多工具插件示例。它把多个工具编译进同一个 `libwsl_tools.so`：

```text
plugins/wsl_tools/
├── wsl_tools.cpp
├── wsl_common.h
├── wsl_create_directory.cpp
├── wsl_list_distros.cpp
├── wsl_scan_files.cpp
├── wsl_recommend_cleanup.cpp
└── wsl_safe_delete.cpp
```

入口文件 `wsl_tools.cpp` 负责：

- 声明各工具 handler。
- 定义 `ToolPlugin tools[]`。
- 在 `handleRequest` 中根据 `tool_index` 分发到具体 handler。
- 导出 `CreateToolPlugin` 和 `DestroyToolPlugin`。

当前实现方式是入口文件集中维护工具元数据和 schema，子工具文件只导出 handler，例如：

```cpp
#include "plugin_helpers.h"
#include <iterator>

extern "C" char* wsl_create_directory_handler(const json& req);
extern "C" char* wsl_list_distros_handler(const json& req);
extern "C" char* wsl_scan_files_handler(const json& req);
extern "C" char* wsl_recommend_cleanup_handler(const json& req);
extern "C" char* wsl_safe_delete_handler(const json& req);

static ToolPlugin tools[] = {
    {
        "wsl_create_directory",
        "Create a directory in WSL filesystem.",
        INPUT_SCHEMA_CREATE_DIR
    },
    {
        "wsl_list_distros",
        "List available WSL distributions.",
        INPUT_SCHEMA_LIST_DISTROS
    },
    {
        "wsl_scan_files",
        "Scan files under the current WSL home directory and save a JSON report.",
        INPUT_SCHEMA_SCAN_FILES
    },
    {
        "wsl_recommend_cleanup",
        "Generate cleanup recommendations from a wsl_scan_files report.",
        INPUT_SCHEMA_RECOMMEND_CLEANUP
    },
    {
        "wsl_safe_delete",
        "Move confirmed files or directories under $HOME to the WSL trash.",
        INPUT_SCHEMA_SAFE_DELETE
    }
};

static const ToolHandler handlers[] = {
    wsl_create_directory_handler,
    wsl_list_distros_handler,
    wsl_scan_files_handler,
    wsl_recommend_cleanup_handler,
    wsl_safe_delete_handler
};

static_assert(std::size(handlers) == std::size(tools), "handler table must match tool definitions");

static char* handleRequest(int tool_index, const char* request_json) {
    try {
        if (tool_index < 0 || tool_index >= static_cast<int>(std::size(handlers))) {
            return mcp_ext::plugin::make_error_result(
                "Unknown tool index: " + std::to_string(tool_index));
        }
        if (!request_json) {
            return mcp_ext::plugin::make_error_result("Request JSON is null");
        }

        json req = json::parse(request_json);
        return handlers[tool_index](req);
    } catch (const std::exception& e) {
        return mcp_ext::plugin::make_error_result(e.what());
    }
}

static ToolPluginAPI plugin_api = {
    tools,
    static_cast<int>(std::size(tools)),
    handleRequest
};
```

子工具文件只保留业务逻辑和 handler 导出。通用返回值使用 `plugin_helpers.h`，WSL 路径校验等公共逻辑放在 `wsl_common.h`。

对应 CMake：

```cmake
set(WSL_TOOLS_SOURCES
    plugins/wsl_tools/wsl_tools.cpp
    plugins/wsl_tools/wsl_create_directory.cpp
    plugins/wsl_tools/wsl_list_distros.cpp
    plugins/wsl_tools/wsl_scan_files.cpp
    plugins/wsl_tools/wsl_recommend_cleanup.cpp
    plugins/wsl_tools/wsl_safe_delete.cpp
)
add_library(wsl_tools SHARED ${WSL_TOOLS_SOURCES})
target_include_directories(wsl_tools PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/plugins
    ${CMAKE_CURRENT_SOURCE_DIR}/plugins/wsl_tools
)
target_link_libraries(wsl_tools PRIVATE nlohmann_json::nlohmann_json)
set_target_properties(wsl_tools PROPERTIES
    LIBRARY_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/plugins
)
```

## 现有插件

`calculator` 提供一个 `calculator` tool，支持：

- `add`
- `subtract`
- `multiply`
- `divide`

`wsl_tools` 当前提供五个 tools：

- `wsl_create_directory`
- `wsl_list_distros`
- `wsl_scan_files`
- `wsl_recommend_cleanup`
- `wsl_safe_delete`

WSL 路径策略：

- 空路径表示默认 workspace：`~/.wsl_workspace`。
- 相对路径会解析到 `~/.wsl_workspace` 下。
- 绝对路径必须位于 `~/.wsl_workspace` 下。
- 拒绝 `..` 路径穿越、`~` 展开和 shell 元字符。

`wsl_scan_files` / `wsl_recommend_cleanup` 与 Resources：

- `wsl_scan_files` 扫描当前 WSL `$HOME`，生成 JSON 报告并保存到 `~/.wsl_workspace/.reports`。
- `wsl_recommend_cleanup` 读取扫描报告，使用内置规则引擎生成建议并保存到 `~/.wsl_workspace/.reports`。
- ext server 在启动时注册 Resource Templates：
- `wsl://scan/{scan_id}/report` 读取 `{scan_id}_report.json`。
- `wsl://scan/{scan_id}/recommendations` 读取 `{scan_id}_recommendations.json`。
- 插件 ABI 本身仍只注册 Tools；WSL Resources 是 ext server 固定注册能力。

`wsl_safe_delete` 的删除策略：

- 只接受 `$HOME` 下的绝对路径。
- 默认 `require_confirmation=true`，未传 `confirmed=true` 时只返回确认 payload。
- 确认后移动到 `~/.local/share/Trash/files`，并在 `~/.local/share/Trash/info` 写入 `.trashinfo`。

## 当前限制

- 插件是同进程动态库，插件崩溃会影响 server 进程。
- 插件目录只扫描一层，不支持递归发现。
- 插件加载发生在 server 启动时，不支持运行时热插拔。
- 插件 ABI 只支持 Tools；WSL Resource Templates 由 ext server 固定注册，不由插件动态声明。
- `inputSchema` 和插件返回 JSON 必须合法，否则会在注册或调用阶段抛出解析异常。
- 加载器会跳过无效插件并继续加载其他插件，但 `loadPlugins()` 只在插件目录不存在时返回 `false`。

## 未来扩展

- [ ] 通用资源插件 ABI (Resources)
- [ ] 提示词插件 (Prompts)
- [ ] 热插拔支持 (文件监控)
- [ ] 插件依赖管理
- [ ] 插件版本控制
