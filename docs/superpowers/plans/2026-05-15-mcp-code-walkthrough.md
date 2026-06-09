# MCP Code Walkthrough Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** 生成一组按 `mcp.pdf` 叙事顺序组织、但以本仓库真实实现为核心的中文代码讲解文档。

**Architecture:** 先创建固定目录和文档骨架，再按章节顺序分批写作。每一批写作后都执行结构检查、占位符检查、交叉链接检查和内容抽样复核，确保文档始终保持“协议意图 / 仓库实现 / 当前边界”三层分离。

**Tech Stack:** Markdown, Git, ripgrep, sed, find, pdfinfo, pdftotext

---

### Task 1: 创建输出目录与文档骨架

**Files:**
- Create: `docs/mcp-code-walkthrough/00-reading-map.md`
- Create: `docs/mcp-code-walkthrough/01-why-mcp-from-react-to-protocol.md`
- Create: `docs/mcp-code-walkthrough/02-entities-host-client-server-in-this-repo.md`
- Create: `docs/mcp-code-walkthrough/03-server-primitives-tools-resources-and-what-is-missing.md`
- Create: `docs/mcp-code-walkthrough/04-client-features-and-current-gaps.md`
- Create: `docs/mcp-code-walkthrough/05-data-layer-json-rpc-to-cpp-types.md`
- Create: `docs/mcp-code-walkthrough/06-server-transport-and-session-lifecycle.md`
- Create: `docs/mcp-code-walkthrough/07-clients-sse-stdio-streamable-http.md`
- Create: `docs/mcp-code-walkthrough/08-examples-tests-and-extension-server.md`
- Create: `docs/mcp-code-walkthrough/09-known-issues-and-reading-notes.md`

- [ ] **Step 1: 创建目录**

Run:

```bash
mkdir -p docs/mcp-code-walkthrough
```

Expected:

```text
命令成功退出，无错误输出
```

- [ ] **Step 2: 为 10 篇文档写入统一骨架**

将下面的模板分别写入每个目标文件，并根据文件名替换标题中的章节主题：

```markdown
# <章节标题>

## 这一篇回答什么问题

## 最小前置知识

## 代码地图

## 主调用链

## 关键实现细节

## 和 mcp.pdf 的对应关系

## 当前实现边界或问题

## 下一篇看什么
```

- [ ] **Step 3: 验证骨架文件是否齐全**

Run:

```bash
find docs/mcp-code-walkthrough -maxdepth 1 -type f | sort
```

Expected:

```text
输出正好 10 个 Markdown 文件，文件名从 00 到 09 连续编号
```

- [ ] **Step 4: 检查每个文件都带有统一章节骨架**

Run:

```bash
rg -n "^## (这一篇回答什么问题|最小前置知识|代码地图|主调用链|关键实现细节|和 mcp.pdf 的对应关系|当前实现边界或问题|下一篇看什么)$" docs/mcp-code-walkthrough
```

Expected:

```text
每个文件都命中 8 个二级标题
```

- [ ] **Step 5: Commit**

```bash
git add docs/mcp-code-walkthrough
git commit -m "docs: scaffold MCP walkthrough chapters"
```

### Task 2: 写 `00` 和 `01`，建立阅读入口与问题背景

**Files:**
- Modify: `docs/mcp-code-walkthrough/00-reading-map.md`
- Modify: `docs/mcp-code-walkthrough/01-why-mcp-from-react-to-protocol.md`

- [ ] **Step 1: 写 `00-reading-map.md`**

将文档写成下面这个结构，并把具体内容填完整：

```markdown
# 阅读地图：这组文档怎么读

## 这一篇回答什么问题

这篇文档说明整套讲解系列的使用方式、章节顺序，以及 `mcp.pdf` 的主题如何映射到当前仓库。

## 最小前置知识

读者默认会看 C++ 代码，不需要先读完 MCP 规范。只需要知道这套文档会同时区分三层：

- 协议层：MCP 或 JSON-RPC 本来想表达什么
- 实现层：这个仓库实际写了什么
- 边界层：这个仓库没有实现、只实现了一部分、或者实现得比较脆弱的部分

## 代码地图

- `mcp.pdf`
- `README.md`
- `include/`
- `src/`
- `examples/`
- `test/`
- `ext/server/`

## 主调用链

先写出推荐阅读顺序：

1. `01-why-mcp-from-react-to-protocol.md`
2. `02-entities-host-client-server-in-this-repo.md`
3. `03-server-primitives-tools-resources-and-what-is-missing.md`
4. `04-client-features-and-current-gaps.md`
5. `05-data-layer-json-rpc-to-cpp-types.md`
6. `06-server-transport-and-session-lifecycle.md`
7. `07-clients-sse-stdio-streamable-http.md`
8. `08-examples-tests-and-extension-server.md`
9. `09-known-issues-and-reading-notes.md`

## 关键实现细节

放一张映射表，至少包含三列：

| `mcp.pdf` 主题 | 仓库重点位置 | 本系列对应文档 |
| --- | --- | --- |
| ReAct / MCP 动机 | `README.md`, `examples/agent_example.cpp` | `01` |
| Host / Client / Server | `include/mcp_client.h`, `include/mcp_server.h` | `02` |
| Server primitives | `include/mcp_tool.h`, `include/mcp_resource.h`, `src/mcp_server.cpp` | `03` |
| Client features | `include/mcp_client.h`, `src/mcp_server.cpp` | `04` |
| JSON-RPC / data layer | `include/mcp_message.h` | `05` |
| transport / session | `src/mcp_server.cpp` | `06` |
| concrete clients | `src/mcp_sse_client.cpp`, `src/mcp_stdio_client.cpp`, `src/mcp_streamable_http_client.cpp` | `07` |
| examples / tests / ext | `examples/`, `test/`, `ext/server/` | `08` |

## 和 mcp.pdf 的对应关系

说明这组文档不是逐页复述，而是“保留叙事顺序，改成代码导读”。

## 当前实现边界或问题

明确提醒：

- 本仓库并不是完整 MCP 规范实现
- 文档和代码存在版本漂移
- 测试覆盖和稳定性都有限

## 下一篇看什么

下一篇：`01-why-mcp-from-react-to-protocol.md`
```

- [ ] **Step 2: 写 `01-why-mcp-from-react-to-protocol.md`**

将文档写成下面这个结构，并填入与本仓库对应的解释：

```markdown
# 从 ReAct 到 MCP：为什么这个仓库会长成现在这样

## 这一篇回答什么问题

解释为什么一个“让模型会用工具”的问题，最后会落成这个仓库里的 `client`、`server`、`tool`、`resource`、`message` 几层结构。

## 最小前置知识

用 2 到 4 段中文说明：

- ReAct 的核心循环是 Thought / Action / Observation
- 如果动作集合不是在 prompt 里硬编码，就需要一种“发现能力 + 调用能力”的协议
- MCP 解决的是“工具和数据能力如何被 Host/Client/Server 规范化暴露”的问题

## 代码地图

- `README.md`
- `include/mcp_client.h`
- `include/mcp_server.h`
- `include/mcp_tool.h`
- `include/mcp_resource.h`
- `include/mcp_message.h`
- `examples/agent_example.cpp`

## 主调用链

写出一条抽象到具体的路径：

1. 模型需要外部动作
2. 外部动作需要统一描述
3. 统一描述需要协议对象
4. 协议对象在本仓库里被拆成 message / tool / resource / server / client
5. transport 只是承载这些对象的通信方式

## 关键实现细节

至少覆盖：

- 为什么 `mcp_message.h` 是数据层基础
- 为什么 `tool` 和 `resource` 是独立抽象
- 为什么 `client` 是抽象接口，具体实现按 transport 分成多类
- 为什么 `server` 要同时处理协议逻辑和 transport 接入

## 和 mcp.pdf 的对应关系

指出本篇对应 `mcp.pdf` 中 ReAct 背景与“MCP 为什么出现”的部分。

## 当前实现边界或问题

指出：

- `agent_example.cpp` 只是演示，不代表完整 agent runtime
- 这个仓库主要实现的是 MCP 通信与工具/资源承载，不是完整的 agent 框架

## 下一篇看什么

下一篇：`02-entities-host-client-server-in-this-repo.md`
```

- [ ] **Step 3: 验证 `00` 和 `01` 的中文结构与编号跳转**

Run:

```bash
sed -n '1,220p' docs/mcp-code-walkthrough/00-reading-map.md
sed -n '1,260p' docs/mcp-code-walkthrough/01-why-mcp-from-react-to-protocol.md
```

Expected:

```text
两篇文档均为中文正文，技术标识保持英文，且 `00` 的“下一篇”跳到 `01`
```

- [ ] **Step 4: 检查没有残留英文模板占位**

Run:

```bash
rg -n "<章节标题>|TODO|TBD|What this chapter answers|Minimum prerequisite knowledge" docs/mcp-code-walkthrough/00-reading-map.md docs/mcp-code-walkthrough/01-why-mcp-from-react-to-protocol.md
```

Expected:

```text
无输出
```

- [ ] **Step 5: Commit**

```bash
git add docs/mcp-code-walkthrough/00-reading-map.md docs/mcp-code-walkthrough/01-why-mcp-from-react-to-protocol.md
git commit -m "docs: add MCP walkthrough introduction chapters"
```

### Task 3: 写 `02`、`03`、`04`，覆盖实体、Server primitives 与 client features

**Files:**
- Modify: `docs/mcp-code-walkthrough/02-entities-host-client-server-in-this-repo.md`
- Modify: `docs/mcp-code-walkthrough/03-server-primitives-tools-resources-and-what-is-missing.md`
- Modify: `docs/mcp-code-walkthrough/04-client-features-and-current-gaps.md`

- [ ] **Step 1: 写 `02-entities-host-client-server-in-this-repo.md`**

正文必须覆盖这些点：

```markdown
- Host 在这个仓库里没有被建成一个统一类，而是体现在“谁持有 client、谁驱动对话与工具调用”
- `mcp::client` 是抽象接口，具体实现是 `sse_client`、`stdio_client`、`streamable_http_client`
- `mcp::server` 是服务端核心入口
- `examples/` 里的程序如何分别扮演“使用 server 的人”和“使用 client 的人”
```

并在“代码地图”里至少引用：

```markdown
- `include/mcp_client.h`
- `include/mcp_server.h`
- `examples/server_example.cpp`
- `examples/sse_client_example.cpp`
- `examples/stdio_client_example.cpp`
- `examples/streamable_http_client_example.cpp`
```

- [ ] **Step 2: 写 `03-server-primitives-tools-resources-and-what-is-missing.md`**

正文必须覆盖这些点：

```markdown
- `tool` 结构和 `tool_builder`
- `server.register_tool(...)` 如何把工具挂到 `tools/list` / `tools/call`
- `resource`、`text_resource`、`binary_resource`、`file_resource`
- `server.register_resource(...)` 和 `server.register_resource_template(...)`
- `resources/list`、`resources/read`、`resources/subscribe` 在 `src/mcp_server.cpp` 中的处理路径
- prompts 在仓库中基本缺位，不能假装已经实现
```

- [ ] **Step 3: 写 `04-client-features-and-current-gaps.md`**

正文必须覆盖这些点：

```markdown
- `roots`、`sampling`、`elicitation` 在 `mcp.pdf` 和 MCP 语义里的位置
- 为什么它们属于 client-provided capabilities
- 本仓库的 `client` 抽象里没有把这些能力完整建模成一套可调用实现
- `initialize` / `capabilities` 只提供了声明接口，不等于能力真的落地
```

并在“当前实现边界或问题”里明确写出：

```markdown
- 这些能力更多停留在协议概念和 capability 声明层
- 仓库没有形成一条完整的 `sampling/createMessage` 或 `elicitation/create` 执行链
```

- [ ] **Step 4: 检查 `02` 到 `04` 的章节跳转和关键词覆盖**

Run:

```bash
rg -n "下一篇：|tools/list|tools/call|resources/list|resources/read|roots|sampling|elicitation|prompts" docs/mcp-code-walkthrough/02-entities-host-client-server-in-this-repo.md docs/mcp-code-walkthrough/03-server-primitives-tools-resources-and-what-is-missing.md docs/mcp-code-walkthrough/04-client-features-and-current-gaps.md
```

Expected:

```text
每篇文档都包含“下一篇”段落，且关键协议术语都已出现
```

- [ ] **Step 5: Commit**

```bash
git add docs/mcp-code-walkthrough/02-entities-host-client-server-in-this-repo.md docs/mcp-code-walkthrough/03-server-primitives-tools-resources-and-what-is-missing.md docs/mcp-code-walkthrough/04-client-features-and-current-gaps.md
git commit -m "docs: add MCP entities and primitives chapters"
```

### Task 4: 写 `05`、`06`、`07`，覆盖数据层、服务端主流程与三种 client

**Files:**
- Modify: `docs/mcp-code-walkthrough/05-data-layer-json-rpc-to-cpp-types.md`
- Modify: `docs/mcp-code-walkthrough/06-server-transport-and-session-lifecycle.md`
- Modify: `docs/mcp-code-walkthrough/07-clients-sse-stdio-streamable-http.md`

- [ ] **Step 1: 写 `05-data-layer-json-rpc-to-cpp-types.md`**

正文必须覆盖这些点：

```markdown
- `request::create`、`request::create_notification`
- `response::create_success`、`response::create_error`
- `id`、notification、`jsonrpc`、`params` 的含义
- `mcp_exception` 和 `error_code`
- 为什么 `mcp_message.cpp` 几乎没有实现，而核心逻辑都放在 header 里
```

- [ ] **Step 2: 写 `06-server-transport-and-session-lifecycle.md`**

正文必须覆盖这些点：

```markdown
- `server::start` 如何挂接 `/sse`、`/message`、`/mcp`
- legacy SSE transport 和 streamable HTTP transport 的差别
- `handle_sse`、`handle_jsonrpc`、`handle_mcp_post`、`handle_mcp_get`、`handle_mcp_delete`
- `initialize` 与 `notifications/initialized`
- `event_dispatcher`
- session id、session timeout、maintenance thread
- `thread_pool`
```

并在“当前实现边界或问题”中明确写出至少这些点：

```markdown
- server 代码同时承担协议和 transport 逻辑，职责较重
- 测试与运行日志已经暴露出端口和生命周期上的脆弱性
```

- [ ] **Step 3: 写 `07-clients-sse-stdio-streamable-http.md`**

正文必须覆盖这些点：

```markdown
- 三个 client 都实现 `mcp::client`
- `sse_client` 依赖先打开 SSE，再通过 endpoint 事件拿到 message endpoint
- `stdio_client` 通过子进程和管道通信
- `streamable_http_client` 通过 `Mcp-Session-Id` 维护 session
- 三者在初始化、错误处理、同步等待和适用场景上的差异
```

并给出一张最少包含这四列的对比表：

```markdown
| client | transport | 初始化关键动作 | 典型风险 |
| --- | --- | --- | --- |
```

- [ ] **Step 4: 验证 `05` 到 `07` 是否覆盖关键类与方法**

Run:

```bash
rg -n "request::create|response::create_success|handle_mcp_post|event_dispatcher|thread_pool|sse_client|stdio_client|streamable_http_client|Mcp-Session-Id" docs/mcp-code-walkthrough/05-data-layer-json-rpc-to-cpp-types.md docs/mcp-code-walkthrough/06-server-transport-and-session-lifecycle.md docs/mcp-code-walkthrough/07-clients-sse-stdio-streamable-http.md
```

Expected:

```text
上述关键标识都在对应章节中被解释
```

- [ ] **Step 5: Commit**

```bash
git add docs/mcp-code-walkthrough/05-data-layer-json-rpc-to-cpp-types.md docs/mcp-code-walkthrough/06-server-transport-and-session-lifecycle.md docs/mcp-code-walkthrough/07-clients-sse-stdio-streamable-http.md
git commit -m "docs: add MCP transport and client chapters"
```

### Task 5: 写 `08`、`09`，收束 examples、tests、ext 与已知问题

**Files:**
- Modify: `docs/mcp-code-walkthrough/08-examples-tests-and-extension-server.md`
- Modify: `docs/mcp-code-walkthrough/09-known-issues-and-reading-notes.md`

- [ ] **Step 1: 写 `08-examples-tests-and-extension-server.md`**

正文必须覆盖这些点：

```markdown
- `examples/server_example.cpp`
- `examples/sse_client_example.cpp`
- `examples/stdio_client_example.cpp`
- `examples/streamable_http_client_example.cpp`
- `examples/agent_example.cpp`
- `test/mcp_test.cpp`
- `test/streamable_http_test.cpp`
- `ext/server/src/main.cpp`
- `ext/server/src/plugin_loader.cpp`
```

说明重点：

```markdown
- examples 展示了库作者预期的使用方式
- tests 展示了项目试图验证什么，以及目前哪里还脆弱
- ext/server 是基于核心库再包一层的插件式服务端，而不是核心库本身
```

- [ ] **Step 2: 写 `09-known-issues-and-reading-notes.md`**

正文必须整理出一份面向读者的“阅读风险清单”，至少包括：

```markdown
- README / 注释与协议版本表述漂移
- prompts 未形成完整实现
- client features 大多停留在 capability 概念层
- `test/streamable_http_test.cpp` 未接入 `test/CMakeLists.txt`
- 测试依赖固定端口，稳定性有限
- `ext/server` 的插件 schema 传播不完整
- 插件层和若干生命周期路径存在实现缺陷
```

并在结尾写出“如果要继续深入源码，推荐从哪几个文件开始复读”。

- [ ] **Step 3: 检查系列是否已完整闭环**

Run:

```bash
find docs/mcp-code-walkthrough -maxdepth 1 -type f | sort
rg -n "下一篇：" docs/mcp-code-walkthrough
```

Expected:

```text
10 个章节文件全部存在，且 `00` 到 `08` 都有“下一篇”跳转，`09` 作为收束篇不再向后跳转
```

- [ ] **Step 4: Commit**

```bash
git add docs/mcp-code-walkthrough/08-examples-tests-and-extension-server.md docs/mcp-code-walkthrough/09-known-issues-and-reading-notes.md
git commit -m "docs: add MCP examples and issues chapters"
```

### Task 6: 全量校对与最终验证

**Files:**
- Modify: `docs/mcp-code-walkthrough/00-reading-map.md`
- Modify: `docs/mcp-code-walkthrough/01-why-mcp-from-react-to-protocol.md`
- Modify: `docs/mcp-code-walkthrough/02-entities-host-client-server-in-this-repo.md`
- Modify: `docs/mcp-code-walkthrough/03-server-primitives-tools-resources-and-what-is-missing.md`
- Modify: `docs/mcp-code-walkthrough/04-client-features-and-current-gaps.md`
- Modify: `docs/mcp-code-walkthrough/05-data-layer-json-rpc-to-cpp-types.md`
- Modify: `docs/mcp-code-walkthrough/06-server-transport-and-session-lifecycle.md`
- Modify: `docs/mcp-code-walkthrough/07-clients-sse-stdio-streamable-http.md`
- Modify: `docs/mcp-code-walkthrough/08-examples-tests-and-extension-server.md`
- Modify: `docs/mcp-code-walkthrough/09-known-issues-and-reading-notes.md`

- [ ] **Step 1: 检查是否有占位符、空章节或英文模板残留**

Run:

```bash
rg -n "TODO|TBD|<章节标题>|What this chapter answers|Minimum prerequisite knowledge" docs/mcp-code-walkthrough
rg -n "^## (这一篇回答什么问题|最小前置知识|代码地图|主调用链|关键实现细节|和 mcp.pdf 的对应关系|当前实现边界或问题|下一篇看什么)$" docs/mcp-code-walkthrough
```

Expected:

```text
第一条命令无输出；第二条命令能在所有章节文件中看到统一中文标题
```

- [ ] **Step 2: 检查 Markdown 文件的基本格式**

Run:

```bash
git diff --check -- docs/mcp-code-walkthrough
```

Expected:

```text
无 trailing whitespace、无冲突标记、无格式错误输出
```

- [ ] **Step 3: 抽查每篇文档都引用了真实仓库对象**

Run:

```bash
rg -n "include/|src/|examples/|test/|ext/server/|mcp::|tools/list|resources/read|initialize|Mcp-Session-Id" docs/mcp-code-walkthrough
```

Expected:

```text
每篇文档都至少包含一组真实文件路径或真实符号引用
```

- [ ] **Step 4: 抽查中文输出约束是否被满足**

Run:

```bash
sed -n '1,80p' docs/mcp-code-walkthrough/06-server-transport-and-session-lifecycle.md
sed -n '1,80p' docs/mcp-code-walkthrough/07-clients-sse-stdio-streamable-http.md
```

Expected:

```text
正文为中文，方法名、类名、路径、协议字段保持英文原样
```

- [ ] **Step 5: Commit**

```bash
git add docs/mcp-code-walkthrough
git commit -m "docs: finalize MCP code walkthrough series"
```
