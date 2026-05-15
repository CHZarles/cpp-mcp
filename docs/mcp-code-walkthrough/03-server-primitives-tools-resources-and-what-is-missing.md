# 服务端基础能力：Tools、Resources 与当前缺失项

## 这一篇回答什么问题

这一篇只看服务端原语，也就是 `mcp::server` 真正能拿出来给 client 用的那些东西：tools 怎么定义、resources 怎么定义、它们如何挂到协议方法上，以及哪些看起来像“协议里应该有”的东西在这个仓库里其实还没有形成完整实现。

如果你想从“能跑起来的 server”继续往下读实现，这一篇就是 `include/mcp_server.h`、`include/mcp_tool.h`、`include/mcp_resource.h` 和 `src/mcp_server.cpp` 之间的总导航。

## 最小前置知识

协议层里，server 侧最核心的两类暴露物是：

- tools：让 client 以 `tools/list` 发现可用工具，再用 `tools/call` 触发执行。
- resources：让 client 以 `resources/list` 枚举资源，再用 `resources/read` 读取内容，部分实现还支持 `resources/subscribe` 订阅变化。

这个仓库对这两类都做了落地，而且不是停留在“描述对象”，而是已经形成了从 C++ 注册到 JSON-RPC 方法分发的闭环。

但也要先记住一个边界：prompts 在这个仓库里基本缺位。后面会专门说，不能把 capability 名字或者协议概念误认为已经有对应实现。

## 代码地图

建议从这几处看起：

- `include/mcp_tool.h`：定义 `tool` 结构和 `tool_builder`。
- `src/mcp_tool.cpp`：实现 `tool_builder`，把 fluent API 组装成最终的 `inputSchema`。
- `include/mcp_resource.h`：定义 `resource` 抽象基类，以及 `text_resource`、`binary_resource`、`file_resource`。
- `src/mcp_resource.cpp`：实现各种 resource 的 `read()`、`get_metadata()`、`is_modified()` 等行为。
- `include/mcp_server.h`：声明 `register_tool(...)`、`register_resource(...)`、`register_resource_template(...)`，以及 server 内部持有的注册表。
- `src/mcp_server.cpp`：真正把 `server.register_tool(...)` 挂到 `tools/list` / `tools/call`，把 `server.register_resource(...)` 和 `server.register_resource_template(...)` 挂到 `resources/list` / `resources/read` / `resources/subscribe` / `resources/templates/list`。

如果你只看头文件，会知道有哪些 API；如果你只看 `src/mcp_server.cpp`，会知道实际分发路径；两边必须对着读。

## 主调用链

先看 tools。

在使用方代码里，典型流程和 `examples/server_example.cpp` 一样：

1. 用 `tool_builder` 构造一个 `tool`，主要填 `name`、`description`、参数 schema。
2. 调用 `server.register_tool(tool, handler)`。
3. client 之后调用 `tools/list` 时，server 把已注册 `tool` 的元数据导出。
4. client 调用 `tools/call` 时，server 根据 `name` 找到对应 handler 并执行。

这条链的关键点在于：`tool` 只是声明，真正可执行的是 `register_tool(...)` 时一并传入的 handler。

再看 resources。

静态资源路径是：

1. 构造某个 `resource` 子类实例，例如 `text_resource`、`binary_resource`、`file_resource`。
2. 调用 `server.register_resource(path, resource_ptr)`。
3. client 调 `resources/list` 时拿到 metadata。
4. client 调 `resources/read` 并带上 `uri` 时，server 找到对应 resource 并执行 `read()`。
5. client 调 `resources/subscribe` 时，server 至少会校验目标 `uri` 是否存在。

模板资源路径是：

1. 调用 `server.register_resource_template(uri_template, name, mime_type, description, handler)`。
2. client 先用 `resources/templates/list` 看到模板定义。
3. 当 client 用某个具体 `uri` 调 `resources/read` 时，server 先查静态 resources，找不到再尝试匹配 `resource_templates_`，匹配成功后调用模板 handler 动态生成内容。

所以这里实际上有两层资源来源：一层是静态注册表，一层是 URI template 驱动的动态生成。

## 关键实现细节

`tool` 结构本身很薄，定义在 `include/mcp_tool.h`，主要字段是：

- `name`
- `description`
- `parameters_schema`
- `annotations`

其中真正暴露给协议层的是 `to_json()`，它会输出 `name`、`description`、`inputSchema`，必要时再带上 `annotations`。也就是说，client 在 `tools/list` 看到的 schema，最终来自这里。

`tool_builder` 则是给 C++ 侧注册工具时省手工 JSON 的辅助器。`src/mcp_tool.cpp` 里可以看到：

- `with_string_param(...)`
- `with_number_param(...)`
- `with_boolean_param(...)`
- `with_array_param(...)`
- `with_object_param(...)`

本质上都在往 `parameters_["properties"]` 里填 schema，再在 `build()` 时补一个顶层 `"type": "object"`，并把 `required_params_` 收进 `"required"`。因此它做的不是运行时校验器，而是“生成给 client 看的 input schema”。

`server.register_tool(...)` 的挂载逻辑在 `src/mcp_server.cpp` 很直接：

- 先把 `{tool, handler}` 存进 `tools_`，key 是 `tool.name`。
- 如果 `method_handlers_` 里还没有 `tools/list`，就注册一个 lambda，把 `tools_` 中所有 `tool_pair.first.to_json()` 拼成返回值。
- 如果 `method_handlers_` 里还没有 `tools/call`，就注册另一个 lambda：先读 `params["name"]`，再在 `tools_` 里找 handler，最后把 handler 的结果包成 `{"isError": false, "content": ...}`；若抛异常，则改成 `isError = true` 并返回文本错误。

这就是 `server.register_tool(...)` 如何挂到 `tools/list` / `tools/call` 的完整路径。不是别处有某个神秘 dispatcher，而是注册时就把 method handler 塞好了。

resources 这一侧分成四个类型层次：

- `resource`：抽象基类，只定义 `get_metadata()`、`read()`、`is_modified()`、`get_uri()`。
- `text_resource`：保存文本内容，`read()` 返回 `{"uri", "mimeType", "text"}`。
- `binary_resource`：保存二进制内容，`read()` 返回 `{"uri", "mimeType", "blob"}`，其中 `blob` 是 base64 编码。
- `file_resource`：基于文件系统的文本资源，继承自 `text_resource`，`read()` 时会实际打开文件、读内容、更新最后修改时间。

`server.register_resource(...)` 的处理路径也在 `src/mcp_server.cpp` 里一次性挂好：

- `resources/read`：先检查 `params["uri"]`，再先查 `resources_` 静态表；找不到再匹配 `resource_templates_`；仍找不到就报错。
- `resources/list`：遍历 `resources_`，把每个 resource 的 `get_metadata()` 放进 `resources` 数组。如果传了 `cursor`，目前只是返回空的 `nextCursor`。
- `resources/subscribe`：只校验 `uri` 是否存在于静态 `resources_`，存在就返回空对象。
- `resources/templates/list`：遍历 `resource_templates_`，导出 `uriTemplate`、`name`、`description`、`mimeType`。

`server.register_resource_template(...)` 的作用不是注册一个现成对象，而是注册一条 URI 模板和一个 handler。等 `resources/read` 收到具体 `uri` 时，再通过模板匹配提取参数并动态生成内容。这个设计对“按 id 取文档”“按路径映射虚拟内容”这类场景更合适。

## 和 mcp.pdf 的对应关系

从协议层看，这一章对应的是 server-provided primitives，也就是 server 向 client 暴露的可发现能力。

从当前仓库实现层看：

- tools 已经有比较完整的声明和调用链。
- resources 也已经有静态资源、模板资源、读取入口和基础订阅入口。
- prompts 则没有形成同等级实现。

换句话说，这个仓库最像“一个 tool/resource 导向的 MCP server 库”，而不是“把规范里所有 server 能力都完整铺平”的实现。

这一点读代码时很重要。你会看到 `tools/list`、`tools/call`、`resources/list`、`resources/read` 都是实打实有分发代码的；但 prompts 不能按同样期待去找。

## 当前实现边界或问题

第一，prompts 在仓库中基本缺位。这里不能假装它已经实现。至少从这次涉及的核心文件看，没有形成与 `tools/*`、`resources/*` 对称的一组 prompts 注册 API 和处理链。

第二，`resources/subscribe` 目前只做了很薄的一层存在性校验并返回空对象。`src/mcp_resource.cpp` 虽然有 `resource_manager::subscribe(...)`、`notify_resource_changed(...)` 这类能力，但 `mcp::server` 当前这条协议处理链并没有把完整订阅分发、事件推送、退订语义都串起来。

第三，`resources/list` 目前只列出静态 `resources_`，不会把 `resource_templates_` 直接混进同一个列表，而是另走 `resources/templates/list`。这不是错误，但要意识到“可枚举静态资源”和“可匹配模板资源”是两套发现路径。

第四，tool schema 主要用于协议描述，不等于 server 在 `tools/call` 时自动按 JSON Schema 做严格参数校验。真正的参数检查更多仍由具体 handler 自己负责，比如 `examples/server_example.cpp` 里的 `calculator_handler(...)` 会手工检查 `operation`、`a`、`b`。

第五，当前文件里看不到完整 prompts 支持，这意味着如果你要扩展到更完整的 MCP server 语义，需要自己补出 prompts 的数据结构、注册接口、method handler 和 capability 对齐。

## 下一篇看什么

下一篇：`04-client-features-and-current-gaps.md`

服务端原语看完以后，最自然的问题就是：client 这边到底接住了多少协议能力。特别是 `roots`、`sampling`、`elicitation` 这些按协议属于 client-provided capabilities 的东西，在这个仓库里到底是已经能调用，还是只是出现在 `initialize` / `capabilities` 声明里。下一篇就专门拆这个问题。
