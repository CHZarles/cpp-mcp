# MCP Code Walkthrough Design

## Goal

Generate a numbered series of code explanation documents for this repository, using the narrative order of `mcp.pdf` as the high-level guide while keeping the content focused on the actual C++ implementation in this codebase.

The target reader is:

- Comfortable with C++
- Interested in understanding how this repository works internally
- Not looking for beginner-first protocol tutorials

Output language requirement:

- All generated walkthrough documents must be written in Chinese
- Source code identifiers, protocol method names, class names, and file paths should remain in their original forms when referenced

The design goal is not to restate `mcp.pdf` page by page. The goal is to preserve its conceptual progression and rewrite it as an implementation-oriented walkthrough of this repository.

## Scope

In scope:

- Reading `mcp.pdf` and extracting its narrative sequence
- Mapping that sequence onto the repository's real implementation
- Writing a numbered documentation series under `docs/mcp-code-walkthrough/`
- Explaining code structure, control flow, protocol mapping, and implementation boundaries
- Adding lightweight prerequisite explanations only where they are necessary for understanding the current chapter

Out of scope:

- Changing library behavior
- Fixing code or tests as part of this documentation task
- Rewriting the existing README
- Writing a protocol-spec-complete MCP tutorial independent of this repository
- Producing a slide-by-slide transcription of `mcp.pdf`

## Source Narrative Order

The document series should follow the idea progression extracted from `mcp.pdf`:

1. ReAct as background
2. Why MCP exists
3. Host / Client / Server entities
4. Server primitives
5. Client features
6. Data layer and JSON-RPC
7. Mapping the protocol model onto this repository's implementation

This ordering is the narrative spine. The documents may regroup subtopics so that each chapter aligns with the repository's real structure instead of the exact slide boundaries in the PDF.

## Audience and Tone

Audience profile:

- Can read C++ headers and source files directly
- Understands classes, callbacks, threads, HTTP, and JSON
- Wants code structure and call flow more than conceptual onboarding

Tone and explanation depth:

- Concise and technical
- Concept explanations should be minimal and only introduced when needed
- Avoid beginner-oriented digressions
- Prefer explicit file and symbol references over generic prose
- Write the prose in Chinese, while preserving English technical identifiers where precision matters

## Output Structure

The documentation series will be written to:

- `docs/mcp-code-walkthrough/`

Planned files:

1. `00-reading-map.md`
2. `01-why-mcp-from-react-to-protocol.md`
3. `02-entities-host-client-server-in-this-repo.md`
4. `03-server-primitives-tools-resources-and-what-is-missing.md`
5. `04-client-features-and-current-gaps.md`
6. `05-data-layer-json-rpc-to-cpp-types.md`
7. `06-server-transport-and-session-lifecycle.md`
8. `07-clients-sse-stdio-streamable-http.md`
9. `08-examples-tests-and-extension-server.md`
10. `09-known-issues-and-reading-notes.md`

## Chapter Intent

### `00-reading-map.md`

Purpose:

- Explain how to use the series
- Show the mapping from `mcp.pdf` topics to repository modules
- Provide the recommended reading order

### `01-why-mcp-from-react-to-protocol.md`

Purpose:

- Connect ReAct-style tool use to the motivation for MCP
- Explain why this repository is split into protocol, client, server, tool, and resource layers

### `02-entities-host-client-server-in-this-repo.md`

Purpose:

- Map Host / Client / Server from MCP terminology onto concrete classes and runtime relationships in this repository

### `03-server-primitives-tools-resources-and-what-is-missing.md`

Purpose:

- Explain how server primitives are represented in this repository
- Focus on `tools/list`, `tools/call`, `resources/list`, `resources/read`, `resources/subscribe`, and resource templates
- Explicitly identify missing or weakly implemented areas such as prompts

### `04-client-features-and-current-gaps.md`

Purpose:

- Explain the MCP idea of client-provided capabilities such as roots, sampling, and elicitation
- Compare that idea against what this repository actually implements or omits

### `05-data-layer-json-rpc-to-cpp-types.md`

Purpose:

- Explain how JSON-RPC request / response / notification concepts map onto `request`, `response`, and `mcp_exception`
- Show how these data structures feed later client and server logic

### `06-server-transport-and-session-lifecycle.md`

Purpose:

- Explain the main server implementation path
- Cover legacy SSE transport, streamable HTTP transport, session creation, initialization, notification handoff, dispatcher behavior, and concurrency model

### `07-clients-sse-stdio-streamable-http.md`

Purpose:

- Compare the three concrete client implementations
- Show initialization flow, transport differences, synchronization strategy, and failure points

### `08-examples-tests-and-extension-server.md`

Purpose:

- Explain how examples, tests, and `ext/server` help reveal intended usage and implementation boundaries
- Include the plugin-based extension server as an example of code layered on top of the core library

### `09-known-issues-and-reading-notes.md`

Purpose:

- Collect important implementation caveats discovered during repository analysis
- Help readers distinguish stable architecture from current project-specific flaws or gaps

## Per-Chapter Template

Each generated chapter should follow the same structure:

1. `What this chapter answers`
2. `Minimum prerequisite knowledge`
3. `Code map`
4. `Main call flow`
5. `Key implementation details`
6. `How this relates to the PDF narrative`
7. `Current implementation boundaries or issues`
8. `What to read next`

This uniform template is intended to make the series predictable and easy to skim.

## Writing Rules

### Core principle

The documents must not blur the difference between protocol intent and repository behavior.

Every chapter should explicitly separate:

- What MCP or JSON-RPC conceptually expects
- What this repository actually implements
- What this repository does not implement, only partially implements, or implements in a project-specific way

### Practical rules

- Use real file references whenever they materially improve navigation
- Prefer call chains and data flow over line-by-line commentary
- Explain only the parts of the code that matter for understanding structure and runtime behavior
- Keep prerequisite explanations short and local to the chapter where they are needed
- Do not claim protocol completeness where the repository only covers a subset
- Write the explanation text in Chinese, but do not translate protocol method names, code symbols, or repository paths

## Repository Reality to Preserve

The generated documentation must reflect the current analyzed state of the repository, including:

- The core library is a C++17 static library built with CMake
- The repository contains a server implementation and three client implementations:
  - SSE client
  - stdio client
  - streamable HTTP client
- Server primitives are implemented unevenly
- Client-side MCP features such as roots, sampling, and elicitation are largely absent as concrete implementations
- Examples and tests reveal both intended usage and current fragility
- The extension server and plugin system exist but have clear limitations
- There are code and documentation drifts that should be called out instead of hidden

## Known Boundaries to Surface in the Documentation

The documentation series should call out, where relevant:

- Prompts are not implemented as a first-class, complete feature in the core library
- Client features such as roots, sampling, and elicitation are not fully realized
- Tests are incomplete and at least part of the test setup is fragile
- `test/streamable_http_test.cpp` exists but is not wired into `test/CMakeLists.txt`
- The extension server plugin layer has implementation limitations and at least one schema propagation gap
- Some comments and README sections still refer to older protocol framing while code has moved toward `2025-03-26`

These points should appear in the relevant chapters and also be summarized in `09-known-issues-and-reading-notes.md`.

## Non-Goals for the First Pass

The first pass of the documentation series should optimize for structural clarity, not encyclopedic completeness.

That means:

- No attempt to cover every helper function
- No attempt to document every README example exhaustively
- No speculative design proposals unless they help explain an implementation gap
- No protocol lawyering beyond what is needed to explain the code

## Validation Criteria

The design is successful if the generated document series:

- Follows the `mcp.pdf` narrative direction without becoming a transcription
- Helps a C++ reader understand how this repository is structured and how requests move through it
- Makes protocol intent, concrete implementation, and missing features visibly distinct
- Gives readers enough file-level anchors to continue reading the source directly
- Surfaces project limitations honestly instead of smoothing them over

## Implementation Plan Boundary

This spec only defines the documentation product and writing constraints.

The next phase should produce a concrete execution plan for:

- Creating the output directory
- Generating the numbered chapter files
- Reviewing chapter ordering and cross-links
- Verifying that all file references and claims match the current repository state
