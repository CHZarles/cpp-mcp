# 已知问题与阅读风险提示

## 这一篇回答什么问题

前面几篇已经把主要代码结构讲完了。最后这一篇不再新增新的协议对象，而是把“阅读这个仓库时最容易误判的地方”集中列出来，避免你把某些 capability 名字、README 说法或示例程序误当成已经成熟实现的事实。

换句话说，这是一份面向读者的风险清单。它的目的不是评价项目好坏，而是帮助你建立更准确的阅读姿势：哪些东西可以当作当前主线实现，哪些只能当作半成品、旧注释残留或局部实验。

## 最小前置知识

这一篇默认你已经读过前面的主线章节，至少知道：

- `mcp::message`、`mcp::tool`、`mcp::resource`、`mcp::client`、`mcp::server` 分别是什么；
- 仓库同时存在 legacy SSE、stdio、streamable HTTP 三条主要 transport 路径；
- `examples/`、`test/`、`ext/server/` 分别承担演示、验证、二次封装的角色。

如果你还没读前文，也可以把这里当成“进入源码前的风险预警”。但更推荐的方式是：先按章节顺序读一遍，再回来看这份清单，会更容易理解每条风险为什么重要。

## 代码地图

- `README.md`
- `include/mcp_message.h`
- `include/mcp_client.h`
- `include/mcp_server.h`
- `include/mcp_tool.h`
- `src/mcp_server.cpp`
- `src/mcp_sse_client.cpp`
- `src/mcp_stdio_client.cpp`
- `src/mcp_streamable_http_client.cpp`
- `examples/server_example.cpp`
- `examples/sse_client_example.cpp`
- `examples/streamable_http_client_example.cpp`
- `test/mcp_test.cpp`
- `test/streamable_http_test.cpp`
- `test/CMakeLists.txt`
- `ext/server/src/main.cpp`
- `ext/server/src/plugin_loader.cpp`
- `ext/server/plugins/tool_api.h`

## 主调用链

这一篇不再按单条运行时链路组织，而是按“最容易误读的层次”收束：

1. 先看文档与注释层的漂移，也就是 `README.md`、example 文件头注释、仓库实际实现之间的版本和能力表述差异。
2. 再看协议能力层的缺口，主要是 prompts、roots、sampling、elicitation 这类“名字存在但实现不完整”的区域。
3. 再看验证层的缺口，也就是测试覆盖、默认测试接线和固定端口依赖问题。
4. 最后看扩展层，也就是 `ext/server` 这种二次封装里有哪些实现折损或平台限制。

这种收束方式的好处是：你在继续深入源码时，可以先判断“我现在看到的是哪一类风险”，而不是每次都重新梳理整个仓库。

## 关键实现细节

### 1. README、注释和当前协议版本表述存在漂移

仓库根 `README.md` 开头写的是“conforming to the 2025-03-26 basic protocol specification”，这是当前主线最接近真实代码的说法；但同一个 `README.md` 里又链接到 `2024-11-05` 的规范地址，`examples/server_example.cpp`、`examples/sse_client_example.cpp` 等文件头注释也还写着 “Follows the 2024-11-05 basic protocol specification”。

这意味着阅读时不能只凭 README 或示例注释做版本判断。更可靠的做法是回到代码本身，尤其是 `include/mcp_message.h`、`src/mcp_server.cpp`、`src/mcp_streamable_http_client.cpp`，看它们到底围绕哪条 transport 和哪些字段在工作。

### 2. prompts 没有形成完整的一等实现

从 capabilities 命名、测试数据或协议背景里，你会多次看到 `prompts` 这个词，但当前仓库并没有像 tools/resources 那样形成清晰、成体系的一等抽象：

- 没有与 `mcp::tool`、`mcp::resource` 对称的完整 prompts 数据结构和操作面；
- 没有一条像 `tools/list` / `tools/call` 那样可直接对照阅读的成熟主链；
- 在 `ext/server/README.md` 的未来扩展项里，prompts 甚至仍被列为“未来可添加”。

所以看到 `prompts` capability 或测试里出现相关字段时，不能自动理解成“仓库已经完整实现 prompts feature”。

### 3. client features 大多还停留在 capability 概念层

`roots`、`sampling`、`elicitation` 是最容易让人误判的三类能力。client example 和测试环境确实会把 `roots` 或 `sampling` 放进 `capabilities`，但这只说明：

- initialize 请求里可以携带这些声明；
- server 可以在 capabilities 层看到这些名字。

它不说明仓库已经提供：

- roots 的完整访问与变更处理接口；
- 一条可工作的 `sampling/createMessage` 请求-响应闭环；
- 一条可工作的 `elicitation/create` 宿主补充信息闭环。

换句话说，这些能力目前更多停留在“协议概念被 JSON 声明出来了”，而不是“库已经把它们实现成完整 API 和执行链了”。

### 4. `test/streamable_http_test.cpp` 存在，但未接入默认测试目标

这点必须单独记住，因为它非常容易被忽略。仓库里确实有 `test/streamable_http_test.cpp`，而且内容不是空壳，它已经在做：

- server 启动
- `streamable_http_client.initialize`
- `ping`
- `get_tools`
- `call_tool`
- `start_sse_stream` / `stop_sse_stream`
- 多次顺序请求

但 `test/CMakeLists.txt` 的 `TEST_SOURCES` 只有 `mcp_test.cpp`。也就是说，默认测试构建和 `ctest` 路径不会自动把 streamable HTTP 这组验证一起跑掉。你要评估这个 transport 的稳定性，不能只凭“仓库里有测试文件”下结论。

### 5. 测试依赖固定端口，稳定性有限

`test/mcp_test.cpp` 中不同测试环境直接使用固定端口，例如 `8080`、`8081`、`8082`、`8083`。这类写法的现实代价是：

- 端口占用会导致测试失败；
- 并行运行测试时更容易冲突；
- 连接关闭、线程回收和下一组测试启动之间需要依赖 `sleep_for` 做时间缓冲。

从阅读角度看，这说明测试更像“验证主路径能跑通”，而不是一套对时序、端口竞争和生命周期都很强健的测试体系。

### 6. `ext/server` 的插件 schema 传播不完整

`ext/server/plugins/tool_api.h` 明确定义了插件工具元数据里的 `inputSchema` 字段，但 `ext/server/src/main.cpp` 在把插件转成 `mcp::tool` 时只调用了：

- `mcp::tool_builder(tool_def.name)`
- `.with_description(tool_def.description)`
- `.build()`

这里没有把 `inputSchema` 写回 `tool.parameters_schema`。结果就是：插件自己知道输入 schema，但通过 MCP 暴露给 client 的工具元数据不一定保留这份 schema。这会直接影响 tool discovery 的质量，也会削弱上层根据 schema 自动构造参数的能力。

### 7. 插件层和若干生命周期路径都存在实现边界

`ext/server/src/plugin_loader.cpp` 只认 `lib*.so`，而且依赖 `dlopen` / `dlsym`。这说明当前插件层：

- 明显偏 Linux/Unix；
- 文件命名约束写死；
- 只覆盖 tools，不覆盖 resources/prompts。

再结合前面的 tests 和 examples，可以看到另一个普遍边界：许多生命周期路径依赖固定启动顺序、显式 `sleep_for`、手工停止或析构时清理。它们可以工作，但还谈不上“对异常时序足够强健”。

## 和 mcp.pdf 的对应关系

`mcp.pdf` 更强调 MCP 架构和能力模型本身；这一篇做的是反向工作，把这些抽象投影到当前仓库后留下的空缺、不一致和项目特有折中全部挑出来。

这也是实现导读里很必要的一步。因为如果只讲“协议应该怎样”，你会高估仓库的完整度；如果只讲“代码目前怎样”，又容易忽略哪些只是阶段性缺口。把两者并排看，才不会误读。

## 当前实现边界或问题

下面给出一份最值得记住的阅读风险清单：

- `README.md`、示例文件头注释和当前代码实现之间存在协议版本表述漂移。
- prompts 没有形成像 tools/resources 那样完整的一等实现。
- client features 里的 `roots`、`sampling`、`elicitation` 大多停留在 capability 概念层，没有形成完整执行链。
- `test/streamable_http_test.cpp` 未接入 `test/CMakeLists.txt`，默认测试流程不会自动覆盖它。
- 多组测试依赖固定端口和 `sleep_for`，稳定性有限，容易受运行环境影响。
- `ext/server` 的插件 schema 传播不完整，`inputSchema` 没有被完整带入 MCP tool 元数据。
- 插件层只覆盖 tools，且当前实现明显偏 Linux/Unix 平台。
- examples 能帮助理解调用方式，但不能被当作成熟度证明。

如果你接下来还要继续深入源码，推荐从下面几处开始复读，收益最高：

- `include/mcp_message.h`：先把 request/response/error 的数据层打牢。
- `src/mcp_server.cpp`：这是协议调度、transport 和 session 真正汇合的地方。
- `include/mcp_client.h` 对照 `src/mcp_sse_client.cpp`、`src/mcp_stdio_client.cpp`、`src/mcp_streamable_http_client.cpp`：最适合建立“统一接口，不同 transport 实现”的直觉。
- `examples/agent_example.cpp`：帮助你把 MCP 接入层放回一个真实的 agent-like 使用场景里。
- `ext/server/src/main.cpp` 与 `ext/server/src/plugin_loader.cpp`：帮助你辨认哪些是核心库能力，哪些已经是项目层二次封装。

## 下一篇看什么

这是最后一篇。读到这里，整组导读已经闭环；如果要重新进入源码，建议回到第 `05`、`06`、`07` 篇，再结合本篇风险清单复读实现细节。
