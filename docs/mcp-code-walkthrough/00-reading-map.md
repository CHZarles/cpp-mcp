# 这组代码导读该怎么读：整体阅读地图

## 这一篇回答什么问题

这篇不是讲某个类怎么实现，而是先告诉你整组导读应该怎么读。目标读者是假定已经会看 C++，也愿意顺着头文件、实现文件和示例程序往下追的人，所以这里先把阅读顺序、仓库分层、以及 `mcp.pdf` 里的概念如何落到当前仓库说明清楚。

更具体地说，这一组文档想回答两个问题。第一，`mcp.pdf` 讲的是 MCP 为什么存在、有哪些角色和能力，但仓库里的代码不会按幻灯片顺序排好；你需要一张“概念到文件”的映射图。第二，这个仓库实现的是一个可用但并不完整的 MCP C++ 框架，读代码时必须始终区分“协议本来怎么定义”和“当前代码实际做到什么”。

## 最小前置知识

读这组文档时，建议一直把三层区分放在脑子里。第一层是协议层，也就是 MCP 和 JSON-RPC 想表达的抽象对象，例如 `initialize`、`tools/list`、`resources/read`、session、notification 这些语义。第二层是实现层，也就是这个仓库如何把这些语义落成 C++ 结构和类接口，例如 `mcp::request`、`mcp::response`、`mcp::tool`、`mcp::resource`、`mcp::client`、`mcp::server`。第三层是边界层，也就是仓库里没有实现、只实现了局部、或者实现方式与文档版本不完全对齐的部分。

如果你以前已经看过 MCP 规范，可以把这组文档当作“实现核对表”；如果没有系统读过规范，也没关系，直接从仓库对象反推协议结构也能看懂。本文不会要求你先逐页读完 `mcp.pdf`，但会不断提醒哪些地方是协议概念，哪些地方只是这个仓库当前的工程选择。

还要提前说明一点：这组文档不是逐页复述 `mcp.pdf`。它保留的是主题推进顺序，而不是照着幻灯片做翻译。你看到的每一篇都会优先回答“这个概念在仓库里具体在哪，怎么串起来，哪里还没补齐”。

## 代码地图

先把阅读入口固定下来，后面各章都会反复回到这些位置：

- `mcp.pdf`：提供整组导读的主题顺序，告诉你为什么先讲动机，再讲角色，再讲 server primitives、data layer、transport 和 examples。
- `README.md`：仓库作者自己给出的组件概览，能快速确认当前项目宣称支持哪些 transport、哪些 example、以及它把 `mcp_client.h`、`mcp_server.h`、`mcp_message.h` 等文件视为核心组件。
- `include/`：最适合先读的地方。这里放着抽象接口和主要数据结构，像 `include/mcp_client.h`、`include/mcp_server.h`、`include/mcp_tool.h`、`include/mcp_resource.h`、`include/mcp_message.h` 会直接决定你后面理解源码的方式。
- `src/`：实现细节集中在这里。比如 `src/mcp_server.cpp` 负责把 method handler、session、SSE、Streamable HTTP 接起来；`src/mcp_sse_client.cpp`、`src/mcp_stdio_client.cpp`、`src/mcp_streamable_http_client.cpp` 则对应不同 transport 的 concrete client。
- `examples/`：这里不是“权威实现”，而是最便于快速建立直觉的样例。`examples/server_example.cpp` 展示 server 如何注册工具；`examples/agent_example.cpp` 则展示一个把 LLM 调用和 MCP client 拼起来的最小演示。
- `test/`：用于确认当前仓库作者实际验证了哪些行为。比如 `test/mcp_test.cpp`、`test/streamable_http_test.cpp` 可以帮助你辨认哪些调用链被覆盖了，哪些地方只是头文件里声明了能力。
- `ext/server/`：这里是一个扩展 server 程序，不属于核心库接口本身，但很适合用来观察“库怎样被二次封装成可执行服务端”。

建议的读法是：先用 `README.md` 和 `include/` 建立对象图，再用 `src/` 看调用链，最后用 `examples/`、`test/`、`ext/server/` 对照真实用法与边界。

## 主调用链

这组文档的推荐阅读顺序就是主调用链本身，从“为什么会需要 MCP”一路走到“这份实现哪里还不完整”：

1. `01-why-mcp-from-react-to-protocol.md`：先看动机，理解为什么“让模型会用工具”最后会拆成协议对象。
2. `02-entities-host-client-server-in-this-repo.md`：再看角色，在本仓库语境里 Host、Client、Server 分别由哪些类和调用关系承载。
3. `03-server-primitives-tools-resources-and-what-is-missing.md`：进入 server 侧最重要的 primitives，先看 `tool` 和 `resource`。
4. `04-client-features-and-current-gaps.md`：回到 client 侧，确认这个仓库给 client 暴露了哪些协议能力、哪些还只是薄封装。
5. `05-data-layer-json-rpc-to-cpp-types.md`：下钻到数据层，理解 `mcp_message.h` 怎样把 JSON-RPC 映射成 C++ 类型。
6. `06-server-transport-and-session-lifecycle.md`：顺着 `src/mcp_server.cpp` 看 transport、session、初始化流程和 request 分发。
7. `07-clients-sse-stdio-streamable-http.md`：对比几个 concrete client，理解 transport 差异究竟落在哪些实现点上。
8. `08-examples-tests-and-extension-server.md`：把前面抽象过的对象放回样例、测试和 `ext/server/`，看这套库怎样被实际使用。
9. `09-known-issues-and-reading-notes.md`：最后集中看已知问题、版本漂移和阅读时容易误判的点。

如果你只想最快进入实现，最少也建议先看 `01`、`05`、`06` 三篇，因为动机、数据层和 server 调度决定了后面绝大多数代码的读法。

## 关键实现细节

下面这张表把 `mcp.pdf` 的主题、仓库里的重点位置，以及本系列对应章节对齐起来。它不是“规范目录”，而是“读代码时该盯哪些对象”的导航表：

| `mcp.pdf` 主题 | 仓库重点位置 | 本系列对应文档 |
| --- | --- | --- |
| ReAct / MCP 动机 | `README.md`、`examples/agent_example.cpp` | `01` |
| Host / Client / Server | `include/mcp_client.h`、`include/mcp_server.h` | `02` |
| Server primitives | `include/mcp_tool.h`、`include/mcp_resource.h`、`src/mcp_server.cpp` | `03` |
| Client features | `include/mcp_client.h`、`src/mcp_sse_client.cpp`、`src/mcp_stdio_client.cpp`、`src/mcp_streamable_http_client.cpp` | `04` |
| JSON-RPC / data layer | `include/mcp_message.h`、`src/mcp_message.cpp` | `05` |
| transport / session | `include/mcp_server.h`、`src/mcp_server.cpp` | `06` |
| concrete clients | `include/mcp_sse_client.h`、`include/mcp_stdio_client.h`、`include/mcp_streamable_http_client.h` | `07` |
| examples / tests / ext | `examples/`、`test/`、`ext/server/` | `08` |

从实现角度看，这个仓库的阅读重心其实很集中：`include/mcp_message.h` 定义协议消息最小单位，`include/mcp_tool.h` 和 `include/mcp_resource.h` 定义 server 提供能力的两类原语，`include/mcp_client.h` 把“消费协议能力”的一侧抽成统一接口，而 `include/mcp_server.h` 与 `src/mcp_server.cpp` 把 method handler、HTTP 路由、session 生命周期以及初始化规则缝合在一起。后面的章节会按这个结构逐步展开。

## 和 mcp.pdf 的对应关系

`mcp.pdf` 在这里的作用，是提供一条从动机到实现的叙事主线，而不是提供逐页讲解模板。本系列会沿用它的主题顺序，比如先讲 ReAct 与工具调用的动机，再讲 Host/Client/Server，再讲数据层和 transport；但每一篇都会优先锚定到当前仓库里真实存在的文件、类、函数和示例程序。

因此，你不应该期待本文档对 `mcp.pdf` 做逐页复述，也不应该把仓库代码默认看成规范的完整镜像。更准确的读法是：`mcp.pdf` 给出“应该讨论什么”，仓库则给出“现在到底实现成了什么样”。

## 当前实现边界或问题

这一点需要在开头就说死，否则后面很容易把“代码存在”误读成“规范完整支持”。首先，本仓库不是完整 MCP 规范实现。`README.md` 宣称自己实现 core functionality，并列出多种 transport，但具体到能力面时，你会发现它主要集中在基础 JSON-RPC 通信、tool 注册调用、resource 暴露和几种 client/server transport 适配，离一个覆盖全部 MCP 语义面的实现还有距离。

其次，文档和代码存在版本漂移，而且漂移不止发生在仓库外部规范与仓库之间，也发生在仓库内部。例如 `README.md` 写的是 conforming to the 2025-03-26 basic protocol specification，`include/mcp_message.h` 和 `include/mcp_client.h` 也写了 2025-03-26，但 `src/mcp_server.cpp` 的文件头注释仍然写着 Follows the 2024-11-05 basic protocol specification，同时实现里又已经加入了 `mcp_endpoint_`、`handle_mcp_post`、`handle_mcp_get`、`handle_mcp_delete` 这样的 Streamable HTTP 路径。读代码时必须接受这种“正在演化中”的状态。

最后，测试覆盖和稳定性都有限。`test/` 里确实有 `mcp_test.cpp` 和 `streamable_http_test.cpp`，但它们更像验证若干关键流程可工作，而不是对所有协议分支、并发边界、异常路径做完备证明。所以后文会把 `test/` 当作“当前作者重点验证过什么”的线索，而不是“已经无歧义证明正确”的依据。

## 下一篇看什么

下一篇看 [`01-why-mcp-from-react-to-protocol.md`](./01-why-mcp-from-react-to-protocol.md)。那一篇先把问题拉回最上游：为什么“模型会用工具”这件事，最后会在这个仓库里落成 `client`、`server`、`tool`、`resource`、`message` 这些分层对象。
