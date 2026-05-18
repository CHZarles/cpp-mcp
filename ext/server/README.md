# MCP Extended Server

基于 cpp-mcp 框架的服务层实现，支持工具插件热插拔。

## 架构

```
┌─────────────────────────────────────────────────────────────┐
│                      MCP Extended Server                    │
│  ┌─────────────┐  ┌─────────────┐  ┌─────────────┐        │
│  │  Plugin A   │  │  Plugin B   │  │  Plugin C   │        │
│  │ (Calculator)│  │  (Weather)  │  │   (Custom)  │        │
│  └─────────────┘  └─────────────┘  └─────────────┘        │
│         │               │               │                  │
│  ┌──────┴───────────────┴───────────────┴──────────────┐   │
│  │                Plugin Loader                        │   │
│  │  - 扫描 plugins/ 目录                               │   │
│  │  - 动态加载 .so/.dll 文件                           │   │
│  │  - 注册为 MCP tools                                 │   │
│  └─────────────────────────────────────────────────────┘   │
│         │                                                   │
│  ┌──────┴──────────────────────────────────────────────┐   │
│  │                cpp-mcp (Framework)                   │   │
│  │  - JSON-RPC 2.0 通信                                │   │
│  │  - Streamable HTTP 传输层                            │   │
│  │  - 消息编解码                                       │   │
│  │  - 服务器实现                                       │   │
│  └─────────────────────────────────────────────────────┘   │
└─────────────────────────────────────────────────────────────┘
```

## 构建

```bash
# 从 cpp-mcp 根目录构建
cmake -B build -DMCP_BUILD_EXT=ON
cmake --build build --config Release

# 或者只构建 ext/server
cd ext/server
cmake -B build
cmake --build build --config Release
```

## 使用

### 编写插件

创建 `plugins/my_plugin.cpp`：

```cpp
#include "tool_api.h"
#include <nlohmann/json.hpp>
#include <string>
#include <cstring>

using json = nlohmann::json;

struct MyPlugin {
    ToolPluginAPI api;

    static char* handleRequest(const char* request) {
        try {
            json req = json::parse(request);
            std::string param = req["param"].get<std::string>();

            json response = {{{"type", "text"}, {"text", "Result: " + param}}};
            return strdup(response.dump().c_str());
        } catch (const std::exception& e) {
            json error = {{{"type", "text"}, {"text", "Error"}}};
            return strdup(error.dump().c_str());
        }
    }

    MyPlugin() {
        api.tool.name = "my_tool";
        api.tool.description = "My custom tool";
        api.tool.inputSchema = R"({
            "type": "object",
            "properties": {
                "param": {"type": "string"}
            },
            "required": ["param"]
        })";
        api.HandleRequest = handleRequest;
    }
};

static MyPlugin instance;

extern "C" {
    ToolPluginAPI* CreateToolPlugin() {
        return &instance.api;
    }

    void DestroyToolPlugin(ToolPluginAPI*) {
        // Static instance, no cleanup
    }
}
```

### 编译插件

```bash
g++ -shared -fPIC -o plugins/libmy_plugin.so plugins/my_plugin.cpp -lnlohmann_json
```

### 运行服务器

```bash
./build/ext/server/mcp-ext-server
```

服务器会自动加载 `./plugins/` 目录下的所有 `.so` 文件。

## 目录结构

```
ext/server/
├── CMakeLists.txt
├── README.md
├── src/
│   ├── main.cpp              # 主程序
│   ├── plugin_loader.h       # 插件加载器头文件
│   └── plugin_loader.cpp     # 插件加载器实现
└── plugins/
    ├── tool_api.h            # 插件接口定义
    ├── calculator.cpp        # 计算器插件示例
    └── weather.cpp           # 天气插件示例
```

## 插件接口

```c
typedef struct {
    const char* name;
    const char* description;
    const char* inputSchema;  // JSON schema
} ToolPlugin;

typedef struct {
    ToolPlugin tool;
    char* (*HandleRequest)(const char* request_json);
} ToolPluginAPI;
```

每个插件必须导出：
- `CreateToolPlugin()` - 创建插件实例
- `DestroyToolPlugin()` - 销毁插件实例

## 扩展

未来可添加：
- 资源插件 (Resources)
- 提示词插件 (Prompts)
- 热插拔支持 (文件监控)
- 插件依赖管理
- 插件版本控制
