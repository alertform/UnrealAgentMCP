# UnrealAgentMCP

Native MCP (Model Context Protocol) server **inside** the Unreal Editor.
No middleman process: the C++ plugin speaks MCP streamable-HTTP directly.

```
Claude Code ──MCP streamable-HTTP (JSON-RPC 2.0, POST /mcp)──► UE Editor (this plugin)
```

## Status: P3 complete — 32 tools, full tool-family coverage

- MCP methods: `initialize` / `notifications/initialized` / `ping` / `tools/list` / `tools/call`
- Tools (32): `engine_info`, `list_assets`, `read_graph`, `add_node`, `connect_pins`, `set_pin_default`, `delete_node`, `create_blueprint`, `compile_blueprint`, `undo`, `redo`, `read_output_log`, `take_screenshot`, `console_command`, `audit_tail`, `get_cdo_property`, `set_cdo_property`, `reparent_blueprint`, `search_assets`, `get_asset_info`, `get_references`, `save_asset`, `spawn_actor`, `set_actor_transform`, `set_actor_property`, `query_actors`, `destroy_actor`, `add_variable`, `set_variable_flags`, `add_component`, `attach_component`, `set_component_property`
- **Three-tier permission ceiling** (ReadOnly / SafeWrite / Destructive) enforced at the single dispatch seam. Destructive tools (`console_command`, `destroy_actor`) are rejected out of the box — raise the ceiling deliberately in Project Settings. Rejections carry a structured `rejected_by_tier` field so agents can branch on policy, not message text
- **Full audit trail**: every call (and every rejection) lands in `Saved/AgentMCP/audit-YYYYMMDD.jsonl`; Destructive calls additionally write a pre-execute `:started` entry so even an editor-killing command is forensically attributable. Inspect from the agent via `audit_tail`
- Closed loop: the agent edits the graph, compiles, reads structured errors back, fixes, recompiles (P2 acceptance locked in `UnrealAgentMCP.NodeGraph.P2AcceptanceClosedLoop`)
- Honest agent contract: schema-rejected pin defaults are errors (never silent), broken/conversion-inserted links are reported, deleted nodes echo their identity, ghost events are reused (locale-safe), undo/redo descriptions warn about the editor-wide stack
- Every mutation is transaction-wrapped: Ctrl+Z undoes agent edits step by step
- Roadmap: Enhanced Input + auto-layout + polish (P4)

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
- Tier enforcement and the JSONL audit log are **live** (P3a): the ceiling check sits at the one dispatch seam every call passes through; the tier ordering is guarded by a `static_assert`.
- All write tools run inside editor transactions — every agent edit is Ctrl+Z undoable. Validation failures cancel their transactions (no undo-history noise).
- Audit entries record a truncated plaintext dump of tool args — don't pass secrets in tool arguments.

## Tests

```
UnrealEditor-Cmd.exe <project.uproject> -ExecCmds="Automation RunTests UnrealAgentMCP" -TestExit="Automation Test Queue Empty" -NullRHI -unattended -nopause -nosplash -log
```

30 tests: registry (5), JSON-RPC protocol incl. hostile-input edge cases (7), tools end-to-end (1), node graph + blueprint tools incl. the P2 acceptance closed loop (5), safety core — audit trail, tier rejection, log capture, undo/redo round-trip, destructive tooling (5), P3b tool families — CDO get/set, asset search/info/refs/save, actor spawn/query/transform/destroy, variable add/flags, component add/attach/set, reparent (7).

## Smoke test (curl)

```powershell
Invoke-RestMethod -Uri http://127.0.0.1:17777/mcp -Method Post -ContentType "application/json" `
  -Body '{"jsonrpc":"2.0","id":1,"method":"tools/call","params":{"name":"engine_info"}}'
```
