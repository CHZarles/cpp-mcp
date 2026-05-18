# 各类客户端传输：SSE、stdio 与 Streamable HTTP

## 这一篇回答什么问题

这一篇看同一个 `mcp::client` 抽象，在当前仓库里是如何被三种不同 transport 具体实现的：

- `sse_client`
- `stdio_client`
- `streamable_http_client`

如果你已经看完 server 侧，这一篇的重点就是反向理解：client 端为了完成同样的 `initialize`、`send_request`、`send_notification`，在连接建立、响应等待、错误传播上各自付出了什么实现代价。

## 最小前置知识

三种 client 都实现了同一个接口：`include/mcp_client.h` 里的 `mcp::client`。也就是说，从调用者视角看，它们都要提供：

- `initialize`
- `ping`
- `send_request`
- `send_notification`
- `get_server_capabilities`
- `call_tool`
- `get_tools`
- `is_running`

协议层上，这三者都在说 JSON-RPC over MCP；差异只在 transport：

- `sse_client`：面向 legacy HTTP+SSE server。
- `stdio_client`：面向本地子进程，通过标准输入输出搬运 JSON-RPC。
- `streamable_http_client`：面向 `/mcp` + `Mcp-Session-Id` 的新 transport。

因此读这一篇时要一直分清三层：

- 协议层：方法名和消息格式一致。
- 当前仓库实现层：连接建立、等待机制、异常映射不同。
- 当前缺口：重试、超时、并发安全、流式行为成熟度并不一致。

## 代码地图

- `include/mcp_sse_client.h`
- `src/mcp_sse_client.cpp`
- `include/mcp_stdio_client.h`
- `src/mcp_stdio_client.cpp`
- `include/mcp_streamable_http_client.h`
- `src/mcp_streamable_http_client.cpp`

此外还应配合看：

- `include/mcp_client.h`：三者共同实现的抽象接口
- `include/mcp_message.h`：`request` / `response` / `mcp_exception`

## 主调用链

### `sse_client`

`sse_client` 的初始化链是三者里最绕的一条：

1. `initialize()` 先构造 `request::create("initialize", ...)`
2. 调 `open_sse_connection()`
3. 等待 SSE 流里出现 `event: endpoint`
4. 从该事件拿到真正的 `msg_endpoint_`
5. 再通过 `send_jsonrpc(req)` 向这个 message endpoint 发 `initialize`
6. 成功后再发 `request::create_notification("initialized")`

这说明 `sse_client` 依赖“先打开 SSE，再通过 endpoint 事件拿到 message endpoint”。如果没有拿到 `endpoint` 事件，它甚至连 initialize 都发不出去。

### `stdio_client`

`stdio_client` 的初始化链更像本地 RPC：

1. `initialize()` 先确保 server 子进程已经启动
2. 通过管道向子进程 stdin 写入一行 JSON-RPC
3. `read_thread_func()` 持续从 stdout 读回 JSON
4. 收到带 `id` 的响应后，填充 `pending_requests_` 里对应 promise
5. initialize 成功后，再发 `request::create_notification("initialized")`

这里 transport 完全不依赖网络；所有同步等待都建立在“一个后台读线程 + 一个 pending request map”之上。

### `streamable_http_client`

`streamable_http_client` 的主链最接近第 06 篇的 server 设计：

1. `initialize()` 直接 `POST /mcp`
2. 从响应头提取 `Mcp-Session-Id`
3. 保存到 `session_id_`，并设 `session_active_ = true`
4. 再发送 `request::create_notification("initialized")`
5. 后续普通请求统一走 `send_post()`
6. 如需服务端主动通知，再调用 `start_sse_stream()`，它会 `GET /mcp` 并附带 `Mcp-Session-Id`

所以它是唯一一个把 session 显式建模成成员变量的网络 client。

## 关键实现细节

### 三种 client 都实现 `mcp::client`

这是先要钉死的一点：

- `class sse_client : public client`
- `class stdio_client : public client`
- `class streamable_http_client : public client`

这意味着上层如果只依赖 `mcp::client` 抽象，就能在不改业务调用代码的前提下切换 transport。但“接口统一”不等于“行为一致”，下面的差异才是实现阅读的重点。

### `sse_client`：先建 SSE，再拿 endpoint

`sse_client` 最特别的地方是初始化依赖两阶段网络动作：

1. `open_sse_connection()` 用单独的 `httplib::Client` 长连 `GET /sse`
2. `parse_sse_data()` 解析到 `event: endpoint` 时，把 `data` 写入 `msg_endpoint_`
3. `initialize()` 用 `endpoint_cv_` 等待 `msg_endpoint_` 可用
4. 之后 `send_jsonrpc()` 才能对 `msg_endpoint_` 发 POST

它的 request/response 匹配方式也比较特殊：

- POST 发送请求后，并不直接从 HTTP response body 拿业务结果
- 真正的结果由 SSE `event: message` 推回来
- `parse_sse_data()` 把回包里的 `id` 序列化成字符串，去匹配 `pending_requests_`

这套实现很贴合 legacy transport，但也意味着初始化链路对时序特别敏感：SSE 没连上、`endpoint` 事件没来、或者 `msg_endpoint_` 被提前清空，后面都会失败。

### `stdio_client`：通过子进程和管道通信

`stdio_client` 的 transport 是纯本地的。它通过子进程和管道通信，核心动作有三类：

- `start_server_process()`：在 Windows 或 POSIX 下创建子进程，并把子进程的 stdin/stdout 重定向到本地管道
- `send_jsonrpc()`：向 `stdin_pipe_[1]` 写一行 JSON
- `read_thread_func()`：后台持续从 `stdout_pipe_[0]` 读数据，解析出 JSON-RPC 消息

同步等待逻辑和 `sse_client` 很像，但信道换成了管道：

- request 发出后，把 promise 放进 `pending_requests_`
- 读线程看到同 `id` 的 response，就设置 promise
- 调用线程等待 future 超时或就绪

它的优点是部署简单、非常适合本机工具集成；缺点是进程生命周期、管道阻塞、子进程异常退出都要 client 自己承担。

### `streamable_http_client`：通过 `Mcp-Session-Id` 维护 session

`streamable_http_client` 最核心的实现事实就是：它通过 `Mcp-Session-Id` 维护 session。

具体表现为：

- `initialize()` 成功后，必须从 HTTP 响应头拿到 `Mcp-Session-Id`
- `send_post()` 对除 `initialize` 以外的请求都会自动附带这个 header
- `start_sse_stream()` 在 `GET /mcp` 时也附带同一个 header
- `close_session()` 最终 `DELETE /mcp` 也附带它

换句话说，`session_id_` 是这个 client 在 transport 层最重要的状态变量。少了它，请求就不是“当前会话里的请求”，而只是一个无上下文的 HTTP POST。

### 初始化差异

三者都遵循“先 `initialize`，再 `notifications/initialized`”的协议节奏，但关键动作不同：

| client | transport | 初始化关键动作 | 典型风险 |
| --- | --- | --- | --- |
| `sse_client` | legacy HTTP+SSE | 先 `GET /sse`，等待 `endpoint` 事件，再向 `/message?session_id=...` 发 `initialize` | SSE 建连失败、收不到 `endpoint`、响应回推超时 |
| `stdio_client` | 子进程 stdio / pipe | 先拉起 server 子进程，再通过 stdin/stdout 管道完成 `initialize` | 子进程启动失败、管道写失败、读线程失步或阻塞 |
| `streamable_http_client` | Streamable HTTP | 直接 `POST /mcp`，从响应头提取 `Mcp-Session-Id`，再发 `initialized` | 服务端不返回 `Mcp-Session-Id`、session 过期、GET SSE 流与 POST 状态不一致 |

这个表本质上说明了一件事：协议初始化是一回事，transport 把“可发 initialize”这一步准备好是另一回事。

### 错误处理差异

三者都会把协议错误重新抛成 `mcp_exception`，但错误来源不一样：

- `sse_client`：既可能失败在 POST，也可能失败在等待 SSE 回包，还可能失败在 SSE 解析。
- `stdio_client`：更多是进程未运行、写管道失败、读管道超时、收到非法 JSON。
- `streamable_http_client`：更多是 HTTP 状态非 2xx、JSON body 解析失败、缺失 `Mcp-Session-Id`、session 已失效。

尤其是 `streamable_http_client::initialize()` 有个很明确的硬失败条件：服务端没返回 `Mcp-Session-Id` 就直接视为初始化失败。这比 `sse_client` 更“状态显式”，但也更依赖服务端严格遵守 header 约定。

### 同步等待差异

同步等待机制是三者实现差别最大的地方之一。

`sse_client`：

- 请求靠 HTTP POST 发出
- 响应通过 SSE 异步回推
- 调用线程等待 `pending_requests_` 对应 future 完成

`stdio_client`：

- 请求写入 stdin pipe
- 响应由后台读线程从 stdout pipe 读回
- 调用线程等待 future，默认超时是 60 秒

`streamable_http_client`：

- 对普通 request，`send_post()` 大多是一次同步 HTTP 往返
- 直接从当前 POST 的 HTTP response body 解析 `result` 或 `error`
- 只有“服务端主动通知”才另外走 `GET /mcp` 的 SSE 流

所以如果你从调用栈角度看：

- `streamable_http_client` 最接近传统同步 RPC；
- `sse_client` 和 `stdio_client` 都是“请求发送线程 + 独立接收通道 + promise/future 汇合”的模式。

### 适用场景差异

- `sse_client` 适合兼容 legacy MCP server，尤其是已经固定暴露 `/sse` + `/message` 的服务。
- `stdio_client` 适合本地 agent、CLI tool、插件进程这类“server 就在本机、最好别走网络”的场景。
- `streamable_http_client` 适合新版本 server 和跨进程/跨主机场景，尤其当你需要明确 session 管理和可选的服务端推送时。

## 和 mcp.pdf 的对应关系

协议层上，三种 client 做的事情并没有变化：

- 都要发 `initialize`
- 都要发 `notifications/initialized`
- 都要在普通方法调用里传 `params`
- 都要把 error response 重新映射成调用侧可处理的异常

但 transport 层上有明确分工：

- legacy SSE 把“请求上传”和“响应返回”拆到不同通道
- stdio 把这两个通道替换为本地进程管道
- streamable HTTP 把 request/response 的主路径统一到 `/mcp`，并把 server-push 作为补充的 GET SSE 流

因此你不能只看 `mcp::client` 接口就假设三者时序完全相同。它们协议一致，生命周期实现并不一致。

## 当前实现边界或问题

- `sse_client` 强依赖先收到 `endpoint` 事件，初始化链条比另外两种更脆弱；一旦 SSE 连接质量不好，失败模式会比较难排查。
- `stdio_client` 要自己管理子进程、管道和读线程，这让它非常实用，但也把进程生命周期问题一并带进了 client 侧。
- `streamable_http_client` 的普通请求路径最简单，但它把 session 正确性强依赖到 `Mcp-Session-Id` header；一旦服务端重启、session 过期或 header 丢失，client 必须重新初始化。
- 三者虽然都实现 `mcp::client`，但超时策略并不统一。`sse_client`、`stdio_client` 都有显式等待 future 的路径，而 `streamable_http_client` 主要依赖 HTTP 客户端超时与同步 POST 行为。
- 三者的“服务端主动发消息”能力也不完全对齐：`sse_client` 天然一直挂着 SSE；`streamable_http_client` 需要额外调用 `start_sse_stream()`；`stdio_client` 则依赖读线程把未匹配消息交给本地逻辑处理，但这里的抽象没有前两者明确。

## 下一篇看什么

下一篇看这些抽象如何在样例和测试里被真正用起来：[08-examples-tests-and-extension-server.md](/home/charles/cpp-mcp/docs/mcp-code-walkthrough/08-examples-tests-and-extension-server.md)。
