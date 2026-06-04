# UnrealAgentMCP

Native MCP (Model Context Protocol) server **inside** the Unreal Editor.
No middleman process: the C++ plugin speaks MCP streamable-HTTP directly.

```
Claude Code ──MCP streamable-HTTP (JSON-RPC 2.0, POST /mcp)──► UE Editor (this plugin)
```

## Status: P1 (protocol skeleton) — verified end-to-end

- MCP methods: `initialize` / `notifications/initialized` / `ping` / `tools/list` / `tools/call`
- Tools: `engine_info`, `list_assets` (read-only)
- `claude mcp list` health check: ✓ Connected (UE 5.5, 2026-06-04)
- Roadmap: Blueprint node-graph editing (P2), actor/BP-class/component tools + tier enforcement + audit log (P3), Enhanced Input + polish (P4)

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

13 tests: registry (5), JSON-RPC protocol incl. hostile-input edge cases (7), tools end-to-end (1).

## Smoke test (curl)

```powershell
Invoke-RestMethod -Uri http://127.0.0.1:17777/mcp -Method Post -ContentType "application/json" `
  -Body '{"jsonrpc":"2.0","id":1,"method":"tools/call","params":{"name":"engine_info"}}'
```
