# UnrealAgentMCP

Native MCP (Model Context Protocol) server **inside** the Unreal Editor.
No middleman process: the C++ plugin speaks MCP streamable-HTTP directly.

```
Claude Code ──MCP streamable-HTTP (JSON-RPC 2.0, POST /mcp)──► UE Editor (this plugin)
```

## Status: P2 (Blueprint node-graph editing) — closed loop verified

- MCP methods: `initialize` / `notifications/initialized` / `ping` / `tools/list` / `tools/call`
- Tools (9): `engine_info`, `list_assets`, `read_graph`, `add_node`, `connect_pins`, `set_pin_default`, `delete_node`, `create_blueprint`, `compile_blueprint`
- Closed loop: the agent edits the graph, compiles, reads structured errors back, fixes, recompiles. The P2 acceptance criterion (BeginPlay → PrintString, compiled clean) is locked in as an automation test (`UnrealAgentMCP.NodeGraph.P2AcceptanceClosedLoop`)
- Honest agent contract: schema-rejected pin defaults are errors (never silent), broken/conversion-inserted links are reported, deleted nodes echo their identity, ghost events are reused (locale-safe) instead of duplicated
- Every mutation is transaction-wrapped: Ctrl+Z undoes agent edits step by step
- Roadmap: actor/component/CDO tools + tier enforcement + audit log (P3), Enhanced Input + auto-layout + polish (P4)

## Setup

1. The plugin lives in `Plugins/UnrealAgentMCP`. Build the editor target and launch the editor.
   Look for `LogAgentMcp: MCP server listening on http://127.0.0.1:17777/mcp` in the Output Log.
2. Connect Claude Code:
   ```
   claude mcp add --transport http unreal http://127.0.0.1:17777/mcp
   ```
3. Port / auto-start / permission tier are configurable under **Project Settings > Plugins > Unreal Agent MCP**.

## Architecture

```
Source/UnrealAgentMCP/
├── Public/
│   ├── UnrealAgentMCPModule.h      module lifecycle (starts server on editor launch)
│   ├── AgentMcpSettings.h          UDeveloperSettings config page
│   ├── Core/AgentMcpTier.h         ReadOnly / SafeWrite / Destructive permission tiers
│   ├── Core/McpTypes.h             tool result/definition types + PluginVersion
│   ├── Core/AgentMcpToolRegistry.h tool registry (name + JSON schema + tier + handler)
│   └── Server/McpProtocol.h        pure JSON-RPC handler — unit-testable without HTTP
└── Private/
    ├── Server/McpHttpServer.*      transport (engine HTTPServer module, game-thread handlers)
    ├── Tools/                      tool implementations (one family per file)
    └── Tests/                      automation tests (13 in P1)
```

Layer contract: the protocol layer never sees HTTP; the transport never sees tools; tools never see JSON-RPC. Everything meets at the registry.

## Security model

- Binds 127.0.0.1 only (engine HTTPServer default — verify with `netstat -an | findstr 17777`).
- Tools carry a permission tier (ReadOnly / SafeWrite / Destructive); tier enforcement + audit log land in P3.
- All write tools (from P2 on) run inside editor transactions — every agent edit is Ctrl+Z undoable.

## Tests

```
UnrealEditor-Cmd.exe <project.uproject> -ExecCmds="Automation RunTests UnrealAgentMCP" -TestExit="Automation Test Queue Empty" -NullRHI -unattended -nopause -nosplash -log
```

18 tests: registry (5), JSON-RPC protocol incl. hostile-input edge cases (7), tools end-to-end (1), node graph + blueprint tools incl. the P2 acceptance closed loop (5).

## Smoke test (curl)

```powershell
Invoke-RestMethod -Uri http://127.0.0.1:17777/mcp -Method Post -ContentType "application/json" `
  -Body '{"jsonrpc":"2.0","id":1,"method":"tools/call","params":{"name":"engine_info"}}'
```
