# 服务端传输层与会话生命周期

## 这一篇回答什么问题

这一篇看 `mcp::server` 怎么把协议对象真正挂到 HTTP 端口上，以及一个 client 从连上来、完成 `initialize`、进入可用状态，到超时或主动关闭为止，服务端内部到底维护了哪些状态。

如果第 05 篇回答的是“消息长什么样”，这一篇回答的就是“消息通过哪条路走、谁在负责会话、谁在负责并发执行”。

## 最小前置知识

先把协议层和 transport 层分开：

- 协议层关心的是 `initialize`、`notifications/initialized`、`tools/list`、`resources/read` 这些 JSON-RPC 方法语义。
- transport 层关心的是请求到底从 `/message` 还是 `/mcp` 进来，响应是内联 HTTP body 返回，还是走 SSE 事件流返回。

这个仓库同时支持两套 transport：

- legacy SSE transport：`GET /sse` 建立事件流，服务端先推一个 `endpoint` 事件，client 再向 `/message?session_id=...` 发 POST。
- streamable HTTP transport：统一走 `/mcp`，其中 `POST /mcp` 发请求，`GET /mcp` 建 SSE 通知流，`DELETE /mcp` 关 session，并靠 `Mcp-Session-Id` 维持会话。

两套 transport 共享同一批协议处理逻辑，尤其都会落到 `process_request()` 和 `handle_initialize()`。

## 代码地图

- `include/mcp_server.h`：`server` 类声明、`event_dispatcher`、session 相关容器、各个 handler 原型。
- `src/mcp_server.cpp`：路由挂接、`handle_sse` / `handle_jsonrpc` / `handle_mcp_post` / `handle_mcp_get` / `handle_mcp_delete` 的完整实现。
- `include/mcp_thread_pool.h`：服务端异步执行请求使用的动态 `thread_pool`。

建议阅读顺序：

1. `server::start`
2. `event_dispatcher`
3. 五个 transport handler
4. `process_request` / `handle_initialize`
5. `check_inactive_sessions` / `close_session`

## 主调用链

### 1. `server::start` 如何挂接 `/sse`、`/message`、`/mcp`

`src/mcp_server.cpp` 里的 `server::start` 做了最关键的路由绑定：

- `http_server_->Post(msg_endpoint_.c_str(), ...)` 挂 `POST /message` 到 `handle_jsonrpc`
- `http_server_->Get(sse_endpoint_.c_str(), ...)` 挂 `GET /sse` 到 `handle_sse`
- `http_server_->Post(mcp_endpoint_.c_str(), ...)` 挂 `POST /mcp` 到 `handle_mcp_post`
- `http_server_->Get(mcp_endpoint_.c_str(), ...)` 挂 `GET /mcp` 到 `handle_mcp_get`
- `http_server_->Delete(mcp_endpoint_.c_str(), ...)` 挂 `DELETE /mcp` 到 `handle_mcp_delete`

再往前看构造函数会发现，这三个 endpoint 默认来自 `server::configuration`：

- `sse_endpoint{ "/sse" }`
- `msg_endpoint{ "/message" }`
- `mcp_endpoint{ "/mcp" }`

所以这份实现不是“server 自动识别 transport”，而是显式把两套 transport 并排暴露在同一个 server 实例上。

### 2. legacy SSE transport 主链

legacy 链路大致是：

1. client `GET /sse`，进入 `handle_sse`。
2. server 生成 `session_id`，创建该 session 的 `event_dispatcher`。
3. server 通过 SSE 推一个 `event: endpoint`，数据内容是 `/message?session_id=...`。
4. client 拿着这个 endpoint，对 `/message` 发 POST，请求进入 `handle_jsonrpc`。
5. notification 直接异步执行；带 `id` 的 request 则丢进 `thread_pool_`，处理完后经 SSE `event: message` 回推。

这条链的特点是“请求通道”和“响应通道”分离：POST 只负责把消息送进去，真正的结果从先前那条 SSE 连接回来。

### 3. streamable HTTP transport 主链

streamable HTTP 则是另一条路：

1. client 首先 `POST /mcp` 发 `initialize`，进入 `handle_mcp_post`。
2. server 为这次初始化创建 `session_id` 和 `event_dispatcher`，并在 HTTP 响应头写回 `Mcp-Session-Id`。
3. client 发送 `notifications/initialized`，server 在 `process_request()` 里把该 session 标记为 initialized。
4. 后续普通请求继续 `POST /mcp`，非流式场景通常直接在 HTTP body 里返回 JSON。
5. 如果 client 需要接收服务端主动通知，可额外 `GET /mcp` 建立 SSE 流，由 `handle_mcp_get` 负责。
6. 结束时 `DELETE /mcp`，由 `handle_mcp_delete` 清理 session。

这条链的特点是：session 生命周期由 header 里的 `Mcp-Session-Id` 显式串起来，而不是像 legacy SSE 那样把 session_id 藏在 `/message?session_id=...` 查询串中。

## 关键实现细节

### `event_dispatcher` 在做什么

`event_dispatcher` 是这一层最关键的基础设施。它本质上不是“协议对象”，而是“把后台线程生成的文本事件安全地推给 SSE 连接”的同步器。

它提供三个核心动作：

- `send_event(message)`：生产一条待发送事件，唤醒等待方。
- `wait_event(sink, timeout)`：阻塞等待事件，然后把内容写入 `httplib::DataSink`。
- `close()`：关闭 dispatcher，唤醒等待线程，让 SSE 流尽快退出。

从实现上看，`event_dispatcher` 还顺手承担了 session 活跃时间记录：

- `update_activity()`
- `last_activity()`

这就是后面 session timeout 能工作的基础。

### `handle_sse`

`handle_sse` 对应 legacy `GET /sse`。它做的事可以拆成三段：

1. 检查 `max_sessions_`，超限直接拒绝。
2. 生成 `session_id`，创建 `event_dispatcher`，放入 `session_dispatchers_`。
3. 启一个 session thread，先发 `endpoint` 事件，再周期性发 `heartbeat`；与此同时把 HTTP 响应设置为 `chunked_content_provider`，持续从 dispatcher 取事件往外写。

这里可以看出 legacy 设计的一个明显特点：每个 SSE session 除了 HTTP 框架线程外，还显式持有一条 `std::thread` 负责 heartbeat 和生命周期收尾。

### `handle_jsonrpc`

`handle_jsonrpc` 对应 legacy `POST /message`。它的职责是：

- 从 query string 里取 `session_id`
- 解析 body 为 JSON，再组装成 `request`
- 如果是 notification，则丢进 `thread_pool_` 异步执行并立即返回 `202`
- 如果带 `id`，则同样丢给 `thread_pool_`，但执行完成后用 `dispatcher->send_event()` 把 `response` 通过 SSE 发回客户端

所以 legacy transport 的一个关键事实是：HTTP POST 并不直接携带响应 body，真正的 response 要经过 SSE 回推。

### `handle_mcp_post`

`handle_mcp_post` 是 streamable HTTP 的主入口，逻辑比 `handle_jsonrpc` 更重，因为它同时要处理初始化、session 校验、batch、以及响应格式选择。

主要分支如下：

- 先解析 JSON；失败时返回 `response::create_error(nullptr, error_code::parse_error, ...)`
- 判断是不是 `initialize`
- 对非 `initialize` 请求要求必须携带 `Mcp-Session-Id`
- 初始化请求会创建新 `session_id` 和 `event_dispatcher`，并把 `Mcp-Session-Id` 写到响应头
- 纯 notification/batch notification 走 fire-and-forget，返回 `202`
- 普通 request 则同步调用 `process_request()`，默认直接把 JSON 写回 HTTP body

代码里还保留了一个“如果客户端接受 `text/event-stream`，且有多个响应，就把 POST 响应拼成 SSE body”的分支。不过注释也写得很直白：当前实现仍以 inline JSON 为主，POST 上真正的流式长任务处理还比较初级。

### `handle_mcp_get`

`handle_mcp_get` 对应 `GET /mcp`，专门给 streamable HTTP transport 提供“服务端主动通知”的 SSE 流。

进入条件有两个：

- 请求头必须有 `Mcp-Session-Id`
- `is_session_initialized(session_id)` 必须为 true

通过校验后，它和 legacy 的 SSE 输出模式基本相同：把响应设成 `text/event-stream`，再用 `chunked_content_provider` 持续从该 session 的 `event_dispatcher` 拉取待发送事件。

换句话说，streamable HTTP 并没有废掉 `event_dispatcher`；它只是把“请求走 POST、服务端推送走 GET SSE”重新组织到了同一个 `/mcp` endpoint 名下。

### `handle_mcp_delete`

`handle_mcp_delete` 很直接：

- 从 `Mcp-Session-Id` 取 session
- 找不到返回 `404`
- 找到就 `close_session(session_id)`

这就是 streamable HTTP transport 的显式 session close 入口。

### `initialize` 与 `notifications/initialized`

这一点必须单独看，因为它决定“会话建立”和“会话可用”不是同一个时刻。

在协议层：

- `initialize` 用来协商协议版本、能力集、serverInfo
- `notifications/initialized` 表示 client 告诉 server：“初始化握手完成，可以进入正常消息阶段了”

在当前仓库实现层：

- `handle_initialize()` 只生成 initialize 的 success response，并不会立刻把 session 标记为 initialized
- `process_request()` 只有在收到 `req.method == "notifications/initialized"` 时，才调用 `set_session_initialized(session_id, true)`
- 对除 `initialize` / `ping` 以外的方法，`process_request()` 会先检查 `is_session_initialized(session_id)`，没完成就返回 `error_code::invalid_request`

所以真正的时序是：

1. `initialize`
2. 服务端返回 success
3. client 再发 `notifications/initialized`
4. session 才变为“可正常调用方法”

这点和很多只看协议文字、不看实现的人直觉不一样，但在这里是明确落地了的。

### session id、session timeout、maintenance thread

session id 由 `generate_session_id()` 生成，看起来是 UUID 风格的十六进制字符串，随后被用于：

- legacy SSE：拼进 `/message?session_id=...`
- streamable HTTP：放进 `Mcp-Session-Id`
- 服务端内部：作为 `session_dispatchers_`、`session_initialized_`、`sse_threads_` 等 map 的 key

session timeout 由 `server::configuration::session_timeout` 控制，对应成员 `session_timeout_`。真正执行超时清理的是：

- `check_inactive_sessions()`：遍历 `session_dispatchers_`，比较 `now - dispatcher->last_activity()` 是否超出阈值
- `maintenance_thread_`：在 `start(false)` 的非阻塞模式下每 10 秒唤醒一次，调用 `check_inactive_sessions()`

也就是说，当前实现的 session 维护不是由 HTTP 框架自动托管，而是 server 自己额外开了一条 maintenance thread 去做空闲回收。

### `thread_pool`

`thread_pool` 定义在 `include/mcp_thread_pool.h`，是一个会自动扩缩容的动态线程池：

- 构造时先启动 `min_threads`
- `enqueue()` 时如果没有空闲线程且还没到 `max_threads`，就 `spawn_thread()`
- worker 空闲超过 `idle_timeout_`，并且当前线程数大于 `min_threads_` 时，会自行退出

在当前仓库里，`server` 构造时用 `thread_pool_(conf.threadpool_size)` 初始化它。虽然只显式传了一个参数，但由于 `thread_pool` 构造函数第一个参数是 `min_threads`，第二个参数 `max_threads` 会自动按硬件并发数倍数取默认值，因此它仍是“核心线程数固定、峰值线程数动态增长”的行为。

它主要被用于：

- legacy `handle_jsonrpc` 中异步处理 notification
- legacy `handle_jsonrpc` 中异步处理有 `id` 的 request，再异步回推 SSE response

反过来说，`handle_mcp_post` 当前更多是同步处理并直接返回 inline response，和 legacy transport 的并发模型并不完全一致。

## 和 mcp.pdf 的对应关系

协议层对应关系可以这样理解：

- legacy SSE transport 对应较早的 HTTP+SSE 组织方式：先建 SSE，再拿单独 message endpoint 发消息。
- streamable HTTP transport 对应较新的 `/mcp` 统一入口模型：POST/GET/DELETE 共用一个资源路径，session 由 `Mcp-Session-Id` 标识。
- `initialize` 与 `notifications/initialized` 的二阶段握手，在当前实现里被严格落实为“先协商，再解锁普通方法调用”。

但实现层也有自己的取舍：

- `process_request()` 同时承担协议状态检查和方法分发。
- `handle_mcp_post()` 不只是 transport handler，还直接处理 batch、初始化分支和部分错误封装。
- `event_dispatcher` 不是协议要求的抽象，而是这个仓库为了把 SSE 推送和 session 活跃时间统一起来而引入的基础设施。

## 当前实现边界或问题

- server 代码同时承担协议和 transport 逻辑，职责较重。`src/mcp_server.cpp` 既做路由挂接、CORS、SSE 输出、session 管理，也做 `initialize` 状态机和 JSON-RPC 错误封装，单文件认知负担比较高。
- 测试与运行日志已经暴露出端口和生命周期上的脆弱性。这里能直接看到不少与连接关闭、heartbeat、session thread、maintenance thread、server start/stop 相关的日志和分支，说明端口占用、连接未及时回收、线程 join/detach 边界都是真实风险面。
- legacy transport 和 streamable HTTP transport 共存于同一个 `server` 实现，复用度高，但也意味着行为分叉多，修改一个 handler 时要反复确认另一套 transport 是否被间接影响。
- `close_session()` 里对 `session_cleanup_handler_` 的调用方式是 `handler(key)`，而不是显然更直观的 `handler(session_id)`。这未必是 bug，但至少说明这块生命周期回调接口需要非常仔细地复核调用约定。
- 只有非阻塞 `start(false)` 会启动 `maintenance_thread_`。如果使用阻塞模式，session timeout 的清理路径是否仍满足预期，需要额外测试确认。

## 下一篇看什么

下一篇看 client 侧如何消费这些 transport：[07-clients-sse-stdio-streamable-http.md](/home/charles/cpp-mcp/docs/mcp-code-walkthrough/07-clients-sse-stdio-streamable-http.md)。重点会转到 `sse_client`、`stdio_client`、`streamable_http_client` 分别怎样初始化、等待响应、处理错误，以及各自适合什么场景。
