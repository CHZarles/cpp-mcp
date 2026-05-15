# 客户端能力与当前缺口

## 这一篇回答什么问题

前两篇看完以后，很容易产生一个错觉：既然 server 侧已经有 `tools/*`、`resources/*` 的落地，那 client 侧是不是也已经把协议里的 client-provided capabilities 全做完了？

这一篇的任务就是把这个错觉拆开。重点看 `roots`、`sampling`、`elicitation` 在 MCP 语义中的位置，以及它们在这个仓库里到底落到了哪里，哪些只是 capability 声明，哪些并没有形成真正可调用的执行链。

## 最小前置知识

在 MCP 语义里，client 不只是“会发请求的人”，它还可能向 server 提供某些由宿主环境掌握的能力。这类能力典型包括：

- `roots`：告诉 server，宿主允许访问或关注哪些根路径/根上下文。
- `sampling`：允许 server 反过来请求 client/host 发起一次模型采样，也就是常见的 `sampling/createMessage` 语义。
- `elicitation`：允许 server 向 client/host 发起补充信息请求，也就是让宿主去收集更多用户输入或上下文。

这三者在 `mcp.pdf` 里的位置都属于 client-provided capabilities。也就是说，协议上它们不是 server 自己凭空拥有的，而是 server 只能在 client 声称支持、且宿主真的实现了时才有资格调用。

所以要区分两层：

- 协议层：client 可以声明自己支持这些能力。
- 实现层：仓库是否真的把这些能力建成了可调用、可处理、可返回结果的一整套代码路径。

## 代码地图

看这几个文件就够了：

- `include/mcp_client.h`：定义 `mcp::client` 的抽象接口。这里能看到当前抽象真正暴露了哪些客户端动作，也能看出哪些东西根本没有进入接口面。
- `src/mcp_sse_client.cpp`：SSE transport 下的 client 实现，能看到 `initialize`、`tools/list`、`tools/call`、`resources/list`、`resources/read` 如何被发送。
- `src/mcp_stdio_client.cpp`：stdio transport 下的 client 实现。
- `src/mcp_streamable_http_client.cpp`：Streamable HTTP transport 下的 client 实现。
- `src/mcp_server.cpp`：虽然这是 server 文件，但要反向确认它是否会主动发起某些针对 client capability 的调用，尤其是有没有完整的 `sampling` / `elicitation` 方法链。

这篇之所以必须把 `src/mcp_server.cpp` 也带上，是因为“client capability 有没有落地”不是只看 client 头文件就够了，还得看 server 端是否存在对这些能力的实际调用路径。

## 主调用链

当前仓库里，三个 client 实现的共通主链非常清楚：

1. 先在本地保存一份 `capabilities_`。
2. `initialize(...)` 时把这份 `capabilities_` 放进请求体发给 server。
3. server 返回 `capabilities` 后，client 保存 `server_capabilities_`。
4. 后续主要围绕 `ping`、`tools/list`、`tools/call`、`resources/list`、`resources/read`、`resources/subscribe` 这些已实现的方法工作。

这条链说明了一个关键事实：`initialize` / `capabilities` 只提供了声明接口，不等于能力真的落地。

例如在 `examples/sse_client_example.cpp` 和 `examples/streamable_http_client_example.cpp` 里，都有类似：

- `client.set_capabilities({{"roots", {{"listChanged", true}}}});`

这表示 client 在协议上声明了一个 `roots` capability 信息块。但从当前抽象和实现来看，这更像“把 JSON 原样塞进 initialize 请求”，而不是“库已经提供 roots 的完整访问、变更通知和服务端回调处理机制”。

## 关键实现细节

先看 `include/mcp_client.h`。`mcp::client` 抽象接口里真正有的方法主要是：

- `initialize(...)`
- `set_capabilities(...)`
- `send_request(...)`
- `send_notification(...)`
- `get_server_capabilities()`
- `call_tool(...)`
- `get_tools()`
- `list_resources(...)`
- `read_resource(...)`
- `subscribe_to_resource(...)`
- `list_resource_templates()`

这里最重要的不是“有什么”，而是“没有什么”：

- 没有专门的 roots API，例如 `list_roots()`、`watch_roots()` 之类。
- 没有 sampling API，例如 `create_message(...)` 或对 `sampling/createMessage` 的封装。
- 没有 elicitation API，例如对 `elicitation/create` 的请求处理接口。

这就说明，本仓库的 `client` 抽象里没有把这些能力完整建模成一套可调用实现。

再看三个 transport 实现，模式完全一致：

- `src/mcp_sse_client.cpp` 的 `initialize(...)` 会把 `capabilities_` 放入 `"capabilities"` 字段。
- `src/mcp_stdio_client.cpp` 的 `initialize(...)` 也是同样做法。
- `src/mcp_streamable_http_client.cpp` 的 `initialize(...)` 也只是把 `capabilities_` 连同 `clientInfo`、`protocolVersion` 一起发给 server。

换句话说，这三份实现都支持“声明 capability”，但没有进一步把 `roots`、`sampling`、`elicitation` 建成独立的高层 API。

再反过来看 `src/mcp_server.cpp`。如果这些 client-provided capabilities 真正落地，通常你应该能在 server 侧看到一条明确执行链：server 何时发 `sampling/createMessage`，如何等待 client 返回，如何把结果继续用于自己的逻辑；或者 server 如何发 `elicitation/create` 并消费用户补充信息。

但当前核心文件里看不到这样一条完整链路。能清楚看到的是：

- `handle_initialize(...)` 会把 server 自己的 capabilities 返回给 client。
- 常规 method handlers 已覆盖 `tools/*`、`resources/*`。
- 没有形成一套与 `sampling/createMessage` 或 `elicitation/create` 对称的服务端主动调用和客户端回包处理闭环。

所以对这类能力，当前实现更接近“协议外壳已经有地方塞声明 JSON”，而不是“库级功能已经真正可用”。

## 和 mcp.pdf 的对应关系

从协议层看，`roots`、`sampling`、`elicitation` 的共同点是：它们都属于 client-provided capabilities。原因也一致，因为这些能力依赖的是宿主环境，而不是 server 自己：

- `roots` 依赖宿主掌握的可访问工作区、目录边界或上下文根。
- `sampling` 依赖宿主侧的模型接入能力、权限和交互策略。
- `elicitation` 依赖宿主能不能继续向用户追问、弹交互、收集补充输入。

因此 server 最多只能“请求使用”，不能在没有 client/host 配合的前提下自己完成。

从当前仓库实现层看，三者的状态比较一致：

- capability JSON 可以被放进 `initialize`。
- 但没有看到完整协议方法执行链。
- 因此它们更多停留在概念层和声明层，而不是像 `tools/call` 那样已经成为库里的一等公民。

这正是协议层与实现层必须分开的原因。规范说“这个能力存在”，不等于库已经把它做成可直接调用的 API。

## 当前实现边界或问题

这里要明确写结论，而不是模糊措辞：

- 这些能力更多停留在协议概念和 capability 声明层。
- 仓库没有形成一条完整的 `sampling/createMessage` 或 `elicitation/create` 执行链。

进一步展开就是：

第一，`initialize` / `capabilities` 只提供了声明接口，不等于能力真的落地。把 `roots`、`sampling`、`elicitation` 写进 `capabilities_`，当前主要效果是把 JSON 带给对端，而不是自动获得对应运行时行为。

第二，本仓库的 `client` 抽象没有把这些能力完整建模成专门 API。对比之下，tools/resources 已经有 `call_tool(...)`、`get_tools()`、`read_resource(...)` 这样的明确入口；而 `roots`、`sampling`、`elicitation` 没有对应层级的接口。

第三，server 侧也没有形成完整的反向调用链。至少从 `src/mcp_server.cpp` 当前主干可见的方法处理来看，没有一条从 server 发起、client 处理、结果回传、再回到 server 逻辑的 `sampling/createMessage` 或 `elicitation/create` 闭环。

第四，`roots` 目前更像 capability 声明中的一个 JSON 片段，而不是库内部有统一模型、缓存、订阅和变更通知机制的能力模块。

如果你接下来要在这个仓库上继续补协议能力，这一章指出的就是优先缺口：不是再加一个 capability 字段，而是补完整的接口、method、回调和运行时状态机。

## 下一篇看什么

下一篇：`05-data-layer-json-rpc-to-cpp-types.md`

既然已经看清哪些能力只是“声明”，哪些能力已经形成真实调用链，下一步就该往更底层走：JSON-RPC 消息在这个仓库里如何映射到 C++ 类型，`request` / `response` / `json` 在传输层和业务层之间怎么流动。这部分放在 `05-data-layer-json-rpc-to-cpp-types.md`。
