# 这个仓库里的核心角色：Host、Client 与 Server

## 这一篇回答什么问题

这一篇先把三个最容易混淆的角色摆正：MCP 协议里的 Host、Client、Server，在这个仓库里分别落到了哪些真实 C++ 对象上，它们在运行时怎么连起来，`examples/` 里的程序又分别站在哪一边。

如果你已经会 C++，真正想看的不是抽象定义，而是“我从哪个类入手、谁调用谁、谁在发 `initialize`、谁在处理 `tools/call`”。这一篇就做这件事。

## 最小前置知识

协议层里，这三个词的关系可以先粗暴理解成：

- Host 是“把 MCP 接进自己应用里的那一层”，它决定什么时候连服务器、什么时候发请求、什么时候把工具结果继续喂给上层对话流程。
- Client 是 Host 手里的协议适配器，负责把 `initialize`、`tools/list`、`tools/call`、`resources/read` 之类的方法翻成具体传输层请求。
- Server 是对外暴露 MCP 能力的一侧，负责声明 capabilities、暴露 tools/resources，并处理来自 client 的 JSON-RPC 调用。

但要注意：这是协议语义，不等于这个仓库一定把三者都各写成一个独立 C++ 类。后面你会看到，这个仓库里最“虚”的恰好就是 Host。

## 代码地图

先看最重要的落点：

- `include/mcp_client.h`：定义 `mcp::client` 抽象接口。这里不是一个可直接实例化的客户端，而是一组协议动作的纯虚接口，比如 `initialize(...)`、`send_request(...)`、`call_tool(...)`、`list_resources(...)`。
- `include/mcp_server.h`：定义 `mcp::server`。这是服务端核心入口，负责监听 HTTP/SSE/Streamable HTTP 端点、维护 session、保存 method/tool/resource 注册表，并在请求到来时分发处理。
- `examples/server_example.cpp`：一个“使用 `mcp::server` 的人”的例子。它创建 server、设置 capabilities、注册 tools，然后启动监听。
- `examples/sse_client_example.cpp`：一个“使用 `mcp::client` 具体实现的人”的例子，具体类型是 `mcp::sse_client`。
- `examples/stdio_client_example.cpp`：同样站在 client 侧，但传输改成 `mcp::stdio_client`，通过子进程 stdio 和服务器通信。
- `examples/streamable_http_client_example.cpp`：还是 client 侧，具体类型换成 `mcp::streamable_http_client`，走 2025-03-26 版的 Streamable HTTP。

从“接口”和“实现”这条线再压缩一遍：

- `mcp::client` 是抽象接口。
- 具体实现是 `sse_client`、`stdio_client`、`streamable_http_client`。
- `mcp::server` 不是抽象概念，而是这个仓库里服务端能力真正汇聚的中心类。

## 主调用链

先看 client 侧。无论是 `examples/sse_client_example.cpp`、`examples/stdio_client_example.cpp` 还是 `examples/streamable_http_client_example.cpp`，主流程都差不多：

1. 构造一个具体 client。
2. 可选地调用 `set_capabilities(...)`，声明这个 client 想告诉 server 的能力。
3. 调用 `initialize(...)` 建立 MCP 会话。
4. 之后通过 `get_tools()`、`call_tool(...)`、`list_resources()`、`read_resource(...)` 等接口驱动协议交互。

这条链上真正“扮演 Host”的，不是某个统一的 `host` 类，而是外层应用代码本身。也就是这些 example 里的 `main()`，或者未来你自己的程序：谁持有 `mcp::client` 对象，谁决定何时初始化、何时发请求、何时把结果继续送入对话/编排逻辑，谁就是这个仓库语境下的 Host。

再看 server 侧。`examples/server_example.cpp` 的流程是：

1. 构造 `mcp::server`。
2. 用 `set_server_info(...)`、`set_capabilities(...)` 配置服务端元信息。
3. 通过 `register_tool(...)` 等接口把能力挂进去。
4. 最后 `start(true)` 开始监听。

运行起来以后，client 发来的 `initialize`、`tools/list`、`tools/call` 等请求会进入 `mcp::server` 的 HTTP 入口，再由它分发到对应 handler。也就是说，server 不是“你写一个函数就算了”，而是完整承接连接、会话、协议方法路由的核心对象。

## 关键实现细节

最关键的一点必须说清：Host 在这个仓库里没有被建成一个统一类。

这不是遗漏，而是当前实现方式的直接结果。仓库真正提供的是：

- 一组 client 抽象和三种 transport 实现。
- 一个 server 核心类。
- 若干 example 程序把它们串起来。

因此 Host 体现在“谁持有 client、谁驱动对话与工具调用”。例如：

- `examples/sse_client_example.cpp` 里，`main()` 创建 `mcp::sse_client client(...)`，然后顺序调用 `initialize`、`ping`、`get_tools`、`call_tool`。这里 `main()` 就是 Host 逻辑。
- `examples/stdio_client_example.cpp` 里，`main()` 持有 `mcp::stdio_client`，负责启动远端 server 进程、初始化连接、读取 tools/resources。这里 Host 仍然是外层程序，而不是库中一个统一对象。
- `examples/streamable_http_client_example.cpp` 里，`main()` 持有 `mcp::streamable_http_client`，还额外设置通知处理器并启动 SSE stream。Host 仍然表现为“调用者代码”。

`mcp::client` 这一层故意只保留协议动作接口，不绑定传输：

- `initialize(...)` 负责启动 MCP 初始化流程。
- `send_request(...)` / `send_notification(...)` 是通用 JSON-RPC 出口。
- `call_tool(...)`、`get_tools()`、`list_resources()`、`read_resource(...)` 是更高一层的便捷包装。

真正的传输细节分别在具体实现里：

- `sse_client` 负责 legacy HTTP+SSE。
- `stdio_client` 负责通过子进程标准输入输出通信。
- `streamable_http_client` 负责新版 Streamable HTTP，会在 `initialize` 后持有 `Mcp-Session-Id`，并可额外拉起 SSE 通知流。

而 `mcp::server` 则把服务端的“入口”和“能力注册表”合并在一起：它内部保存 method handlers、tool 注册表、resource 注册表、session 状态，并在收到请求时统一调度。

## 和 mcp.pdf 的对应关系

从协议层视角看，Host/Client/Server 是职责划分；从这个仓库的实现层看，则更像“两头实、中间一层虚”：

- Server 在仓库里是实的，对应 `mcp::server`。
- Client 在仓库里也是实的，但先抽象成 `mcp::client`，再落成多个 transport 实现。
- Host 在仓库里没有被单独建模成统一类，而是散落在应用入口代码里。

这和 `mcp.pdf` 并不冲突。协议要求的是角色关系，不要求必须出现一个名为 `host` 的类。很多真实工程都会这么做：把 Host 留给上层应用，把 Client/Server 做成可复用库。

对看实现的人来说，这个区别非常重要。否则你会一直在仓库里找一个并不存在的 “Host class”，结果越找越乱。

## 当前实现边界或问题

第一，Host 没有统一抽象，意味着仓库没有替你定义“对话编排层”。工具调用前后如何与 LLM 对话结合、如何维护多轮上下文、如何做策略决策，都留给库使用者自己写。

第二，`mcp::client` 虽然把基础能力抽象出来了，但它更偏“协议客户端”而不是“完整宿主 SDK”。它提供了初始化、请求发送、tools/resources 访问，但没有把完整的 Host 生命周期封装成更高阶对象。

第三，examples 的角色必须分清：

- `examples/server_example.cpp` 是“使用 `mcp::server` 的人”，也就是服务提供方示例。
- `examples/sse_client_example.cpp`、`examples/stdio_client_example.cpp`、`examples/streamable_http_client_example.cpp` 是“使用 `mcp::client` 具体实现的人”，也就是宿主/接入方示例。

如果把这些 example 误看成“协议三种角色各有一个类”，会误读整个仓库结构。

## 下一篇看什么

下一篇：`03-server-primitives-tools-resources-and-what-is-missing.md`

有了 Host / Client / Server 的对应关系，下一步最值得看的是 `mcp::server` 到底提供了哪些服务端原语：`tool` 怎么定义，`server.register_tool(...)` 怎么映射到 `tools/list` / `tools/call`，`resource` 又是怎么挂到 `resources/list` / `resources/read` 上的，以及哪些协议项目前还基本缺位。
