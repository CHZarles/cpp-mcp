# 数据层：从 JSON-RPC 到 C++ 类型

## 这一篇回答什么问题

这一篇看最底层的数据模型：一条 MCP / JSON-RPC 消息，在这个仓库里如何被表示成 `request`、`response`、`mcp_exception` 这些 C++ 类型，以及这些类型怎样把协议字段压平为便于调用的构造函数。

如果你已经会 C++，真正关心的是“后面的 server / client 为什么能直接传 `request` 和 `response`”，那答案基本都在这里：`include/mcp_message.h` 把 JSON-RPC 的共通语义收口成几个非常薄的值类型，后续 transport 层只是负责搬运和调度。

## 最小前置知识

协议层先记住四个概念即可。

- `jsonrpc` 是 JSON-RPC 版本字段，这个仓库固定写成 `"2.0"`。它不是 MCP 版本；MCP 版本由 `initialize` 里的 `protocolVersion` 协商。
- `id` 用来把请求和响应配对。带 `id` 的消息是 request，响应里必须带回同一个 `id`。不带 `id` 的是 notification。
- notification 表示“只送达，不等返回值”。在 JSON-RPC 语义里，它没有响应；在这个仓库里通常也不会进入“等待 future / promise 返回”的那条路径。
- `params` 是方法参数容器，具体结构由方法自己定义，例如 `initialize` 会把 `protocolVersion`、`capabilities`、`clientInfo` 都放进去。

仓库实现层在这之上只做了一个很直接的映射：`request`/`response` 持有 `nlohmann::ordered_json`，不强行把每个方法的参数再拆成大量专用 struct。这样做的代价是静态类型约束弱一些，但好处是协议扩展成本低。

## 代码地图

- `include/mcp_message.h`：核心文件，定义 `error_code`、`mcp_exception`、`request`、`response`，绝大多数逻辑都在这里内联实现。
- `src/mcp_message.cpp`：几乎是空壳，只 `#include "mcp_message.h"`，没有实质性协议逻辑。

如果只想抓主线，先看 `request::create` / `request::create_notification`，再看 `response::create_success` / `response::create_error`，最后看 `request::to_json`、`request::from_json`、`response::to_json`、`response::from_json`。

## 主调用链

从“业务代码要发一条请求”到“网络上出现 JSON”大致是这一条链：

1. 上层代码调用 `request::create(method, params)` 生成 request 对象。
2. transport 层把 `request::to_json()` 的结果序列化成字符串发送出去。
3. 对端收到后，通常用 `request::from_json()` 或手工填充 `request`。
4. 方法执行成功时走 `response::create_success(req.id, result)`。
5. 方法执行失败时，如果抛出 `mcp_exception`，会被上层捕获并转换成 `response::create_error(req.id, e.code(), e.what())`。
6. 最终 `response::to_json()` 再被发回客户端。

这个链条的关键点是：`id` 始终作为 request/response 的关联键贯穿全程；notification 则因为 `id == null`，天然绕开“回包等待”逻辑。

## 关键实现细节

### `request::create`

`request::create` 是最常用的请求构造入口，做了四件事：

- 把 `jsonrpc` 固定设为 `"2.0"`。
- 通过私有 `generate_id()` 自动生成一个 `id`。
- 原样写入 `method`。
- 把传入的 `params` 放到 `req.params`。

这里的 `generate_id()` 目前是 header 内一个 `static int next_id = 1; return json(next_id++);`。也就是说：

- 当前实现层默认把 `id` 生成为递增整数；
- 返回类型仍然是 `json`，因此协议上仍兼容 JSON-RPC 允许的多种 `id` 表示；
- 但当前缺口也很明显：这个自增计数没有并发保护。如果多个线程同时直接构造 request，`id` 生成并不是严格线程安全的。

### `request::create_notification`

`request::create_notification` 刻意把 notification 和普通 request 区分开：

- `req.id = nullptr`，这就是“没有响应”的协议标志；
- `req.method = "notifications/" + method`，也就是仓库内部默认把通知方法名规范化为 `notifications/...`；
- `params` 仍然照常携带。

这也是为什么很多上层代码写的是 `request::create_notification("initialized")`，实际发出去的是 `notifications/initialized`。这个行为不是调用方自己拼字符串，而是数据层直接帮你做掉了。

### `response::create_success`

`response::create_success` 的结构非常薄：

- `jsonrpc = "2.0"`
- `id = req_id`
- `result = result_data`

它不做任何业务判断，只负责保证响应外壳正确。比如 `src/mcp_server.cpp` 里的 `process_request()`，在方法执行成功后直接 `return response::create_success(req.id, result).to_json();`。

### `response::create_error`

`response::create_error` 则负责构造 JSON-RPC 标准错误对象：

- `jsonrpc = "2.0"`
- `id = req_id`
- `error.code = static_cast<int>(code)`
- `error.message = message`
- 如果 `data` 非空，再附加 `error.data`

这里值得注意的是，`req_id` 即便为 `nullptr` 也能传进去。对于解析失败、非法请求这类场景，服务端可以据此生成没有正常 request id 的错误响应。

### `id`、notification、`jsonrpc`、`params` 在仓库里的真实含义

- `id`：不是“数据库主键”，只是一次 RPC 往返的相关性标识。`sse_client` 用 `response["id"].dump()` 做 map key，`stdio_client` 直接用 `req.id` 查 `pending_requests_`，都说明它的唯一职责是匹配响应。
- notification：在协议层表示“不期望 response”；在实现层直接影响等待逻辑。比如 `src/mcp_sse_client.cpp` 和 `src/mcp_stdio_client.cpp` 的 `send_jsonrpc` 都先判断 `req.is_notification()`，是的话发送完立即返回。
- `jsonrpc`：这里只是 JSON-RPC 2.0 固定头，不参与 MCP 版本协商。不要把它和 `MCP_VERSION` 混为一谈。
- `params`：仓库把它保留为 `json`，因此方法参数检查延后到 handler 内做，而不是在 `request` 类型层面静态建模。

### `mcp_exception` 和 `error_code`

`error_code` 是一个 `enum class`，目前定义了 JSON-RPC 常见错误码：

- `parse_error = -32700`
- `invalid_request = -32600`
- `method_not_found = -32601`
- `invalid_params = -32602`
- `internal_error = -32603`
- `server_error_start = -32000`
- `server_error_end = -32099`

`mcp_exception` 继承自 `std::runtime_error`，额外持有一个 `error_code code_`。这意味着业务或 transport 代码可以像抛普通异常一样抛它，但上层在 catch 时还能拿到协议级错误码。

在当前仓库里，这个设计最直接的用法出现在 server/client 两端：

- server 侧 `process_request()` 单独捕获 `const mcp_exception&`，再转成 `response::create_error(...)`。
- client 侧在解析到对端 `error` 对象时，会重新抛出 `mcp_exception`，把协议错误重新映射回 C++ 异常。

于是形成一个对称闭环：本地异常 -> 线上的 JSON-RPC error -> 对端本地异常。

### 为什么 `mcp_message.cpp` 几乎没有实现，而核心逻辑都放在 header 里

这是这个仓库一个很明显的设计取向：`request` 和 `response` 被当作极轻量、近乎 header-only 的值类型使用。

放在 `include/mcp_message.h` 的现实好处有三点：

- 这些函数都很短，而且和类型声明绑定很紧，内联后阅读成本最低。
- 模块间几乎都会用到它们，放 header 可以减少“声明在 `.h`、定义在 `.cpp`、来回跳转”的认知成本。
- `request::create`、`to_json`、`from_json` 这种小函数很适合编译器内联，避免额外的链接边界。

`src/mcp_message.cpp` 几乎为空，说明这里没有需要隐藏的复杂状态，也没有重型算法。它更像是为了保持编译单元结构完整、给未来扩展预留落点，而不是当前逻辑承载者。

当然当前缺口也存在：header 内的 `generate_id()` 使用了函数内静态变量，若以后要改成更严格的线程安全或跨进程唯一 ID，可能就不再适合继续维持这种“全内联极薄实现”的状态。

## 和 mcp.pdf 的对应关系

协议层对应关系很直接：

- JSON-RPC request/notification 的区别，仓库用 `id` 是否为 null 表示。
- JSON-RPC success/error response 的区别，仓库用 `result` 与 `error` 二选一表示。
- MCP 的方法名仍然是字符串，比如 `initialize`、`tools/list`、`resources/read`；数据层并不理解这些方法的业务含义，只负责装载它们。

需要特别区分的是：

- `jsonrpc = "2.0"` 对应的是 JSON-RPC 层；
- `MCP_VERSION = "2025-03-26"` 对应的是 MCP 协议版本；
- `notifications/initialized` 这样的名字约定，是当前仓库实现层对 notification 的组织方式，不是 `request` 类型本身必须知道的全部协议内容。

## 当前实现边界或问题

- 数据层大量使用 `json`，上手快，但缺少编译期约束。调用者只有在运行时才会发现 `params` 结构不合法。
- `request::generate_id()` 是简单的函数内静态自增整数，没有显式同步，严格并发下存在竞态风险。
- `request::from_json()` / `response::from_json()` 基本是宽松反序列化，没做很强的字段合法性校验，更多校验责任被上推到 server/client。
- `src/mcp_message.cpp` 仍保留着旧注释和空实现，说明文件边界与当前真实职责并不完全一致，读代码时不要被 `.cpp` 文件名误导。

## 下一篇看什么

下一篇进入 transport 与生命周期，也就是 [06-server-transport-and-session-lifecycle.md](/home/charles/cpp-mcp/docs/mcp-code-walkthrough/06-server-transport-and-session-lifecycle.md)：看这些 `request` / `response` 如何被 `server::start` 挂到 `/sse`、`/message`、`/mcp` 上，并进一步变成带 session 的服务端行为。
