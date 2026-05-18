# 通过示例、测试与扩展服务器看懂仓库用法和边界

## 这一篇回答什么问题

前面几篇主要在讲抽象接口、协议对象和 transport 主链。这一篇把视角切回“这些抽象是怎样被作者自己用起来的”。也就是说，我们不再只看 `include/` 和 `src/`，而是直接看 `examples/`、`test/`、`ext/server/` 这三块外围代码。

这篇要回答三个更实际的问题。第一，库作者预期用户怎样创建 server、怎样创建 client、怎样把工具调用接进一个 agent-like 循环。第二，测试到底在试图验证什么，哪些链路真的被执行过，哪些又只是看起来声明过。第三，`ext/server` 这种“再包一层”的代码说明了什么，它和核心库本身的边界在哪里。

## 最小前置知识

这里先分清三类文件的角色：

- `examples/` 是“用法演示”，重点是帮助你建立调用顺序和 API 使用直觉，不保证代表最稳健的生产写法。
- `test/` 是“作者打算验证的行为集合”，能帮助你判断哪些功能至少被自动化跑过，哪些功能虽然有实现但没有接进默认测试链。
- `ext/server/` 是“基于核心库再封一层的应用代码”，它使用 `mcp::server` 和插件加载器拼出一个可执行服务端，但它不是核心库 API 的一部分。

因此这一篇的阅读姿势不是“看规范”，而是“看这个仓库怎样使用自己”。这通常比只读头文件更容易暴露真实边界。

## 代码地图

- `examples/server_example.cpp`
- `examples/sse_client_example.cpp`
- `examples/stdio_client_example.cpp`
- `examples/streamable_http_client_example.cpp`
- `examples/agent_example.cpp`
- `examples/CMakeLists.txt`
- `test/mcp_test.cpp`
- `test/streamable_http_test.cpp`
- `test/CMakeLists.txt`
- `ext/server/src/main.cpp`
- `ext/server/src/plugin_loader.cpp`
- `ext/server/src/plugin_loader.h`
- `ext/server/plugins/tool_api.h`
- `ext/server/README.md`

## 主调用链

最适合把外围代码串起来的读法是下面这条链：

1. 先看 `examples/server_example.cpp`，理解“最小 MCP server”在这个仓库里的标准搭法：配置 `mcp::server`、设置 capabilities、注册 tools、启动监听。
2. 再看三个 client example，也就是 `examples/sse_client_example.cpp`、`examples/stdio_client_example.cpp`、`examples/streamable_http_client_example.cpp`，对照前面的第 07 篇，把同一个 `mcp::client` 抽象如何落在不同 transport 上重新看一遍。
3. 接着看 `examples/agent_example.cpp`，它把“本地 MCP server + MCP client + 外部 LLM HTTP 调用”串成一个最小闭环，说明这套库在作者心里最接近哪类使用场景。
4. 然后看 `test/mcp_test.cpp`，确认自动化测试主要覆盖了 message 格式、initialize、versioning、ping、tools 这些主路径。
5. 再单独看 `test/streamable_http_test.cpp` 和 `test/CMakeLists.txt` 的关系，确认 streamable HTTP 测试文件虽然存在，但没有接进默认测试目标。
6. 最后看 `ext/server/src/main.cpp` 和 `ext/server/src/plugin_loader.cpp`，理解“把核心库包装成插件式服务端”这件事是怎样完成的，以及它哪里开始偏离核心库的通用抽象。

如果你只想快速建立“这个库到底怎么被作者自己使用”的直觉，前五个 example 文件就足够；如果你想评估成熟度，必须把 `test/` 和 `ext/server/` 一起看。

## 关键实现细节

### `server_example.cpp` 代表最标准的服务端接入方式

`examples/server_example.cpp` 的价值不在于功能复杂，而在于它基本把核心 server API 都摆到了台面上：

- 构造 `mcp::server::configuration`
- 创建 `mcp::server`
- `set_server_info(...)`
- `set_capabilities(...)`
- 用 `tool_builder` 创建 `get_time`、`echo`、`calculator`、`hello`
- `register_tool(...)`
- `start(true)`

也就是说，如果你想知道“核心库作者期望别人怎样写 server”，这就是最接近参考答案的文件。它同时也暴露了一个事实：示例重点明显偏向 tools，resources 注册代码被注释掉了，说明当前仓库最成熟的对外能力仍然是工具调用链。

### 三个 client example 分别对应三种 transport 的标准用法

`examples/sse_client_example.cpp` 展示的是 legacy SSE 用法。它先声明 `roots` capability，设置 timeout，执行 `initialize(...)`，然后调用 `ping()`、`get_server_capabilities()`、`get_tools()` 和多次 `call_tool(...)`。这和第 07 篇讲的实现链是对齐的：从使用者视角看，SSE transport 仍然表现为一个同步 client API。

`examples/stdio_client_example.cpp` 展示的是本地子进程场景。它直接把 server 命令行作为 `stdio_client` 构造参数传进去，还额外传了环境变量 JSON。这个例子最有价值的地方在于，它清楚说明了 stdio client 并不依赖仓库自带 server；它是为了接任何支持 stdio transport 的 MCP server。

`examples/streamable_http_client_example.cpp` 则展示了新 transport 的显式 session 语义：`initialize(...)` 后读取 `get_session_id()`，之后再 `ping()`、`get_tools()`、`call_tool(...)`，并且额外演示了 `set_notification_handler(...)`、`start_sse_stream()`、`stop_sse_stream()` 这一条“服务端主动通知”链。也就是说，主请求路径和通知路径已经被拆成两条。

### `agent_example.cpp` 说明仓库定位更接近“给 agent 提供 MCP 接入层”

`examples/agent_example.cpp` 不是完整 agent framework，但它非常能说明仓库定位。它做了三件事：

1. 在本地创建一个 `mcp::server` 并注册 `calculator`；
2. 再创建一个 `mcp::sse_client` 连回本地 server；
3. 用 `httplib::Client` 直接请求外部 chat completions 接口，把模型返回的 `tool_calls` 转成 `client.call_tool(...)`。

这说明仓库作者的真实想象场景不是“server 独立存在”，而是“宿主应用可能一边接 LLM，一边通过 MCP 调工具”。但同时也必须看到：这里没有规划器、权限系统、持久状态、长任务恢复，也没有把 tool schema 与模型 API 抽象统一收口，因此它只能算最小演示，不是完整 runtime。

### `mcp_test.cpp` 展示了作者真正重视的主链

`test/mcp_test.cpp` 的覆盖重点很明确：

- message 格式：`request::create`、`request::create_notification`、`response::create_success`、`response::create_error`
- lifecycle：`initialize`
- versioning：支持/不支持版本
- ping
- tools

从这个覆盖分布可以反推仓库成熟度：核心 message 与 SSE 主链是被优先验证的；而 prompts、完整 client-provided features、复杂资源变更通知，并没有在测试里形成同等级覆盖。

### `streamable_http_test.cpp` 是存在的，但默认测试链没有接上

`test/streamable_http_test.cpp` 本身已经写成一个可执行集成测试：它起本地 server，创建 `streamable_http_client`，验证 `initialize`、`ping`、`get_tools`、`call_tool`、SSE stream 开关，以及多次顺序请求。

但 `test/CMakeLists.txt` 的 `TEST_SOURCES` 只有 `mcp_test.cpp`。这说明：

- 这个测试文件存在；
- 它代表作者已经开始补新 transport 的验证；
- 但默认 `MCP_BUILD_TESTS=ON` 的构建路径不会自动把它编进 `mcp_tests`。

因此你不能把“仓库里有 `streamable_http_test.cpp`”直接等价成“streamable HTTP 已被默认测试链稳定覆盖”。

### `ext/server` 是二次封装示例，不是核心库的一部分

`ext/server/src/main.cpp` 的结构很直接：

- 创建 `PluginLoader`
- 从 `./plugins` 目录加载动态库
- 创建 `mcp::server`
- 把每个插件转成一个 `tool` 和一个 `tool_handler`
- 注册到 server
- 启动服务

这里最值得注意的是插件元数据的传播方式。`ext/server/plugins/tool_api.h` 里的 `ToolPlugin` 明明带有 `inputSchema` 字段，但 `ext/server/src/main.cpp` 在构建 `mcp::tool` 时只用了 `name` 和 `description`，没有把 `inputSchema` 重新灌回 `tool_builder` 或 `tool.parameters_schema`。这意味着插件层声明的 schema 在进入 MCP tool 元数据时并没有被完整传播，client 看到的工具参数信息会丢失或弱化。

再看 `ext/server/src/plugin_loader.cpp`，它只扫描 `lib*.so` 文件并用 `dlopen` / `dlsym` 加载，这说明当前插件层是明显偏 Linux/Unix 的，而且只处理 tools，没有扩展到 resources 或 prompts。

## 和 mcp.pdf 的对应关系

这一篇对应 `mcp.pdf` 里“把协议模型映射回真实程序”的那部分，但写法是工程化重组后的版本。`mcp.pdf` 更像告诉你 MCP 能怎样被使用；这里则直接让你看作者在这个仓库里怎样使用它自己提供的抽象。

从叙事位置上看，这篇应该出现在你已经理解 protocol、server、client 之后。因为只有先知道抽象层在干什么，你再看 examples 和 tests 时，才能分辨“这是核心能力”还是“只是演示脚手架”。

## 当前实现边界或问题

这一层必须说得直接一些：

- `examples/` 展示了库作者预期的使用方式，但它们不是稳定性证明。很多示例依赖本地固定端口、手动启动顺序和最小能力集。
- `examples/server_example.cpp` 仍保留旧版规范注释，和仓库当前 `2025-03-26` 主线存在表述漂移。
- `examples/agent_example.cpp` 只是最小演示，里面直接拼外部 LLM HTTP 请求和 MCP 调用链，没有抽象成可复用 runtime。
- `test/mcp_test.cpp` 以 SSE 主链为主，覆盖面并不能代表整个仓库，尤其不能代表新 transport 已经完全稳定。
- `test/streamable_http_test.cpp` 未接入 `test/CMakeLists.txt`，默认测试构建不会自动执行它。
- `ext/server` 是基于核心库再包一层的插件式服务端，而不是核心库本身；它只覆盖 tools，且插件 schema 没有完整传播到 MCP tool 元数据。
- `plugin_loader.cpp` 当前只识别 `lib*.so` 并依赖 `dlopen`，跨平台支持和插件能力范围都比较有限。

## 下一篇看什么

下一篇：`09-known-issues-and-reading-notes.md`。最后把整套仓库阅读过程中最容易误判的点收束成一份风险清单。
