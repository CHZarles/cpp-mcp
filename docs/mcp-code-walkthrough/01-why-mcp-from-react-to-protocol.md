# 为什么需要 MCP：从 React 接入到协议抽象

## 这一篇回答什么问题

这一篇先不急着讲某个函数怎么写，而是回答一个更根本的问题：为什么一个表面上像“让模型会调用外部工具”的需求，最后会在这个仓库里长成 `client`、`server`、`tool`、`resource`、`message` 这几层分工明确的对象。

如果只看 `examples/agent_example.cpp`，你会觉得事情很简单：模型输出一个 `tool_calls`，程序收到后调用 `client.call_tool(...)`，再把结果回填给模型就行。但一旦把“演示一下”提升为“不同 server 可以被不同 host 稳定接入”，你就必须回答能力如何发现、参数如何描述、返回值如何统一、通信怎么承载、初始化和 session 如何管理。这些问题一起出现时，协议抽象就不可避免。

## 最小前置知识

ReAct 的基本循环可以概括成 Thought / Action / Observation。模型先基于当前上下文形成内部推理方向，也就是 Thought；接着选择一个动作，例如搜索、调用计算器、读取文件，这一步是 Action；环境把动作结果返回给模型，形成新的 Observation。然后模型再基于新 Observation 进入下一轮 Thought。即便你在工程里不显式打印 Thought，这个“先判断要不要做外部动作，再消费返回结果”的闭环仍然存在。

问题在于，Action 集合不能总是硬编码在 prompt 里。单个 demo 当然可以把几个函数签名直接塞进 prompt，但一旦工具数量变多、来源变多、权限边界变复杂，或者需要让同一个 host 连接不同 server，硬编码 prompt 就会变成一套难以维护的私有约定。模型不仅要“知道有哪个动作”，还要“知道这些动作的名字、输入 schema、返回形状、何时可用，以及该通过什么通道调用”。

这也是 MCP 要解决的问题。它不是替模型做推理，也不是直接替你实现 agent runtime；它解决的是“工具和数据能力如何被标准化暴露，以及 host/client/server 三方如何围绕这些能力协作”。换句话说，MCP 把本来容易散落在 prompt、临时 JSON 格式、私有 HTTP API 和自定义 glue code 里的约定，收束成一组可以发现、可以调用、可以组合的协议对象。

在这个仓库里，这种收束最终体现在几类核心抽象上：`mcp_message.h` 负责请求/响应这种最底层协议数据；`mcp_tool.h` 和 `mcp_resource.h` 负责表达“可调用能力”和“可读取数据”；`mcp_client.h` 负责站在使用者一侧消费这些能力；`mcp_server.h` 则负责把这些能力通过具体 transport 暴露出去。你可以把它理解成：ReAct 提出“模型需要 Action/Observation 回路”，MCP 进一步回答“这个回路如何被工程化、可互操作地落地”。

## 代码地图

这一篇建议配合下面这些文件一起看：

- `README.md`：先看项目作者自己怎样描述“core features”和主要组件，尤其是 `Client Interface`、`Message Processing`、`Tool Management`、`Resource Management`、`Server` 那几段。
- `include/mcp_client.h`：定义 `mcp::client` 抽象接口，可以直接看出 client 这一侧被要求支持 `initialize`、`send_request`、`call_tool`、`list_resources`、`read_resource` 等协议动作。
- `include/mcp_server.h`：定义 `mcp::server`、`tool_handler`、`notification_handler`、session 相关接口，以及 `register_tool`、`register_resource` 这类能力暴露入口。
- `include/mcp_tool.h`：定义 `mcp::tool` 结构和 `tool_builder`，表示“模型可调用的动作”在仓库里的数据形状。
- `include/mcp_resource.h`：定义 `mcp::resource` 及其派生类，表示“模型可读取或订阅的数据对象”在仓库里的抽象边界。
- `include/mcp_message.h`：定义 `mcp::json`、`mcp::request`、`mcp::response`、`mcp_exception`，是所有上层抽象共用的数据地基。
- `examples/agent_example.cpp`：展示一个最小 agent-like 演示，能帮助你把抽象对象重新放回“模型要用工具”的场景里。

## 主调用链

把抽象到具体的路径写直白一点，大致是下面这条链：

1. 模型需要外部动作。单靠参数记忆和上下文补全，模型无法稳定完成计算、查文件、读外部系统状态这类任务。
2. 外部动作需要统一描述。否则每个工具都是一套私有 JSON 格式，模型和宿主程序都没法稳定发现和调用。
3. 统一描述需要协议对象。你至少需要一种统一的 request/response 表达方式，以及对能力、参数、错误、通知的共同语言。
4. 协议对象在本仓库里被拆成多层抽象：`include/mcp_message.h` 负责消息，`include/mcp_tool.h` 负责动作能力，`include/mcp_resource.h` 负责数据能力，`include/mcp_server.h` 负责提供侧，`include/mcp_client.h` 负责消费侧。
5. transport 只是承载这些对象的通信方式。无论是 SSE、stdio 还是 Streamable HTTP，本质上都只是把同一批协议对象传过去，而不是重新定义工具或资源本身。

如果你想从演示代码走到协议实现，可以沿着 `examples/agent_example.cpp` 里的“模型决定调用工具”往回追。这个例子先在本地构建一个 `mcp::server` 并用 `server.register_tool(...)` 注册 `calculator`，然后创建 `mcp::sse_client client("http://localhost:8889")`，执行 `client.initialize(...)`，接着在对话循环中把模型返回的 `tool_calls` 转成 `client.call_tool(tool_name, args)`。这条链说明：模型层面看到的是“我要执行一个动作”，而仓库实现层面必须把它分解成初始化、能力发现、消息封装、server 分发和结果回传几个步骤。

## 关键实现细节

第一个关键点是，为什么 `mcp_message.h` 是数据层基础。因为无论你最后暴露的是 tool、resource 还是 initialize 流程，落到线上都要表现成 JSON-RPC 风格的 request、response 和 error。`include/mcp_message.h` 里定义了 `using json = nlohmann::ordered_json;`、`struct request`、`struct response`、`enum class error_code` 和 `mcp_exception`，这意味着上层抽象共享同一套消息骨架。后面 `client` 发请求、`server` 回结果、tool handler 抛异常转错误，本质都建立在这一层。

第二个关键点是，为什么 `tool` 和 `resource` 要分开抽象。`include/mcp_tool.h` 里的 `mcp::tool` 强调的是“一个可被调用的动作”，核心字段是 `name`、`description`、`parameters_schema`、`annotations`，天然对应的是 `tools/list` 和 `tools/call` 这种调用语义。`include/mcp_resource.h` 里的 `mcp::resource` 则强调“一个按 URI 标识、可以读取、可能会变化的数据对象”，核心接口是 `get_metadata()`、`read()`、`is_modified()`、`get_uri()`。前者关注调用和参数校验，后者关注内容表示与变更感知；如果把二者混成一种抽象，协议语义和实现边界都会变得模糊。

第三个关键点是，为什么 `client` 是抽象接口。`include/mcp_client.h` 把 `initialize`、`ping`、`send_request`、`send_notification`、`call_tool`、`list_resources`、`read_resource` 等操作定义成纯虚函数，说明仓库作者想把“协议能力”与“传输实现”分开。这样 `src/mcp_sse_client.cpp`、`src/mcp_stdio_client.cpp`、`src/mcp_streamable_http_client.cpp` 可以各自处理连接建立、会话保持、请求发送的差异，但对使用者暴露出尽量一致的调用面。对会看实现的人来说，这也是推荐的阅读顺序：先读抽象接口，再对比不同 concrete client 如何把同一接口落在不同 transport 上。

第四个关键点是，为什么 `server` 既处理协议又处理 transport 接入。理想分层里，你可能会希望“纯协议调度”和“HTTP/SSE/stdio 接入”完全拆开；但当前仓库的 `include/mcp_server.h` 和 `src/mcp_server.cpp` 选择把二者编织在一起。`server::start()` 直接注册 `msg_endpoint_`、`sse_endpoint_`、`mcp_endpoint_` 的 HTTP 路由，`handle_jsonrpc`、`handle_mcp_post`、`handle_mcp_get`、`handle_mcp_delete` 既解析 transport 请求，又决定 session 是否存在、是否已初始化、该调用哪个 method handler。这样做的好处是库的使用门槛较低，`server.register_tool(...)` 后可以直接对外服务；代价是 server 类承担了较多职责，后面读代码时你会频繁在“协议分发逻辑”和“网络接入逻辑”之间切换。

## 和 mcp.pdf 的对应关系

本篇对应 `mcp.pdf` 中最前面的两类内容：一类是 ReAct 背景，也就是为什么模型需要一个稳定的 Thought / Action / Observation 外部回路；另一类是“MCP 为什么出现”，也就是为什么这种回路一旦跨越不同工具、不同数据源、不同宿主程序，就需要被协议化。

但这里的写法仍然不是照着 `mcp.pdf` 逐页解释。我们关心的是这些概念在当前仓库里分别变成了哪些头文件、哪些对象和哪些调用链。

## 当前实现边界或问题

`examples/agent_example.cpp` 必须看，但也必须降权看。它只是一个演示程序，用来说明“一个 LLM 输出 tool call 后，可以怎样通过 `mcp::sse_client` 去调用本地起起来的 `mcp::server`”。它里面包含命令行参数解析、向 chat completions 端点发 HTTP 请求、解析 `tool_calls`、把结果再喂回模型这些逻辑，足以帮助你建立直觉，但它不代表完整的 agent runtime，更没有覆盖长期记忆、规划器、权限控制、并发任务调度、复杂恢复策略这些真正 agent 框架会关心的问题。

同样地，这个仓库的主轴也不是“完整 agent 框架”，而是 MCP 通信层和工具/资源承载层。你可以从 `README.md` 的组件列表直接看出来，核心文件都围绕 `mcp_client.h`、`mcp_message.h`、`mcp_tool.h`、`mcp_resource.h`、`mcp_server.h` 展开。因此，后面读代码时最好把它当成“一个为 agent 或 host 提供 MCP 接入能力的 C++ 库”，而不是把 `examples/agent_example.cpp` 当成项目的真正中心。

## 下一篇看什么

下一篇看 [`02-entities-host-client-server-in-this-repo.md`](./02-entities-host-client-server-in-this-repo.md)。在理解“为什么需要协议抽象”之后，接下来就该把协议里的角色压回仓库：Host、Client、Server 在这里分别是谁，由哪些类和调用关系承载。
