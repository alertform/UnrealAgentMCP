# UnrealAgentMCP

Native MCP (Model Context Protocol) server **inside** the Unreal Editor.
No middleman process: the C++ plugin speaks MCP streamable-HTTP directly.

```
Claude Code ──MCP streamable-HTTP (JSON-RPC 2.0, POST /mcp)──► UE Editor (this plugin)
```

## Status: 1.3-dev — 52 tools

- MCP methods: `initialize` / `notifications/initialized` / `ping` / `tools/list` / `tools/call`
- Tools (52): `engine_info`, `list_assets`, `read_graph`, `add_node`, `connect_pins`, `set_pin_default`, `delete_node`, `auto_layout`, `add_component_event`, `create_blueprint`, `compile_blueprint`, `undo`, `redo`, `read_output_log`, `take_screenshot`, `console_command`, `audit_tail`, `list_dirty_packages`, `load_level`, `get_cdo_property`, `set_cdo_property`, `reparent_blueprint`, `search_assets`, `get_asset_info`, `get_references`, `save_asset`, `delete_asset`, `spawn_actor`, `set_actor_transform`, `set_actor_property`, `query_actors`, `destroy_actor`, `add_variable`, `set_variable_flags`, `add_component`, `attach_component`, `set_component_property`, `create_input_action`, `create_mapping_context`, `add_mapping_entry`, `add_widget`, `list_widgets`, `set_widget_property`, `rename_widget`, `add_viewmodel`, `add_view_binding`, `list_view_bindings`, `remove_view_binding`, `create_anim_montage`, `add_anim_notify`, `set_ge_target_tags`, `add_compatible_skeleton`
- **Three-tier permission ceiling** (ReadOnly / SafeWrite / Destructive) enforced at the single dispatch seam. Destructive tools (`console_command`, `destroy_actor`) are rejected out of the box — raise the ceiling deliberately in Project Settings. Rejections carry a structured `rejected_by_tier` field so agents can branch on policy, not message text
- **Full audit trail**: every call (and every rejection) lands in `Saved/AgentMCP/audit-YYYYMMDD.jsonl`; Destructive calls additionally write a pre-execute `:started` entry so even an editor-killing command is forensically attributable. Inspect from the agent via `audit_tail`
- Closed loop: the agent edits the graph, compiles, reads structured errors back, fixes, recompiles (P2 acceptance locked in `UnrealAgentMCP.NodeGraph.P2AcceptanceClosedLoop`)
- Honest agent contract: schema-rejected pin defaults are errors (never silent), broken/conversion-inserted links are reported, deleted nodes echo their identity, ghost events are reused (locale-safe), undo/redo descriptions warn about the editor-wide stack
- Every mutation is transaction-wrapped: Ctrl+Z undoes agent edits step by step
- Enhanced Input authoring: create `UInputAction` / `UInputMappingContext` data assets and bind keys (validated against `EKeys`) without leaving the agent loop
- `auto_layout` untangles agent-built graphs: longest-path layering, left-to-right data/exec flow, zero node overlap — one transaction, one Ctrl+Z
- **UMG widget-tree authoring** (1.1): create/parent/enumerate widgets and set template properties (`EntryWidgetClass` validated against `IUserListEntry`); created widgets are always Blueprint variables so downstream tools can bind to them
- **MVVM view authoring** (1.1): add viewmodels (custom name + creation type) and declarative property bindings to WidgetBlueprints — validate-first design surfaces every path-resolution failure the underlying editor APIs would silently swallow; type-mismatched pairs get conversion functions auto-discovered (e.g. int↔float), and bindings can be listed/removed by id
- `add_component_event` (1.1): bound events (e.g. Button `OnClicked`) for widgets and actor components — replicates the editor path without popping UI, one event per (component, delegate) enforced
- Class/object pin defaults (1.1): `set_pin_default` handles `PC_Class`/`PC_Object`/soft pins via `TrySetDefaultObject` with meta-class pre-validation
- `delete_asset` (1.1, Destructive): refuses when on-disk referencers exist unless `force=true`; existence pre-checked so missing assets are clean tool errors
- WorldSettings opt-in (1.1): `query_actors include_system=true` exposes the WorldSettings actor so agents can set the per-map GameMode Override; level-script actors stay hidden
- `rename_widget` (1.2): headless replication of the editor's rename path — display label, UObject FName, delegate/animation/navigation bindings, BP variable references, and MVVM binding destination paths all stay in sync (bindings with conversion functions are reported for manual re-add rather than silently broken)
- `list_dirty_packages` (1.2): unsaved-state visibility (maps vs content) — call before quitting the editor; born from a live incident where unsaved binding edits were lost on quit
- Content class paths everywhere (1.2): `/Game/UI/WBP_X` resolves without the `.WBP_X_C` suffix in `EntryWidgetClass` too; unresolvable classes are honest errors, never silent base-class substitutions
- `get_asset_info` filters the Find-in-Blueprint binary blob and oversized tags (1.2) — responses stay agent-sized
- **Anim montage authoring + GE target tags** (1.3-dev): `create_anim_montage` builds a skeleton-matched UAnimMontage from any AnimSequence; `add_anim_notify` attaches any UAnimNotify subclass at an absolute time or fraction with optional property import; `set_ge_target_tags` grants tags to a GameplayEffect's target via `UTargetTagsGameplayEffectComponent` — dogfooding source: GA_Fireball cooldown + hit-event authoring workflow
- **Level automation** (1.3-dev): `load_level` switches the editor's active map by package path — validates asset existence + class, aborts on dirty packages (never pops a save dialog), NOT undoable (global editor operation); `engine_info` now includes `current_level` (outermost package name of the active World, empty string when no world is loaded)
- `add_compatible_skeleton` (1.3-dev): registers a marketplace/foreign USkeleton as compatible with the project's main skeleton via `USkeleton::AddCompatibleSkeleton` — fixes the common issue where animation packs ship their own USkeleton copy that prevents animations from playing on the project character; idempotent (already-registered = clean success)

This release was specified by **dogfooding**: 1.0 was pointed at real main-menu UI work, and every action that still required human hands in the editor (class pin dropdowns, widget creation, MVVM binding panels, GameMode overrides, asset deletion) became a 1.1 tool; 1.2 closed the gaps the 1.1 cycle itself surfaced (widget rename, dirty-state visibility, path ergonomics).

## Setup

1. The plugin lives in `Plugins/UnrealAgentMCP`. Build the editor target and launch the editor.
   Look for `LogAgentMcp: MCP server listening on http://127.0.0.1:18777/mcp` in the Output Log.
2. Connect Claude Code:
   ```
   claude mcp add --transport http unreal http://127.0.0.1:18777/mcp
   ```
3. Port / auto-start / permission tier are configurable under **Project Settings > Plugins > Unreal Agent MCP**.

> Upgrading from ≤1.1? The default port moved 17777 → 18777 (PIE's game traffic owns UDP 17777
> and the shared number confused diagnostics). Re-add the connection:
> `claude mcp remove unreal && claude mcp add --transport http unreal http://127.0.0.1:18777/mcp`

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
    └── Tests/                      automation tests (47, covering P1-P7)
```

Layer contract: the protocol layer never sees HTTP; the transport never sees tools; tools never see JSON-RPC. Everything meets at the registry.

## Security model

- Binds 127.0.0.1 only (engine HTTPServer default — verify with `netstat -an | findstr 18777`).
- Tier enforcement and the JSONL audit log are **live** (P3a): the ceiling check sits at the one dispatch seam every call passes through; the tier ordering is guarded by a `static_assert`.
- All write tools run inside editor transactions — every agent edit is Ctrl+Z undoable. Validation failures cancel their transactions (no undo-history noise).
- Audit entries record a truncated plaintext dump of tool args — don't pass secrets in tool arguments.

## Tests

```
UnrealEditor-Cmd.exe <project.uproject> -ExecCmds="Automation RunTests UnrealAgentMCP" -TestExit="Automation Test Queue Empty" -NullRHI -unattended -nopause -nosplash -log
```

42 tests (baseline): registry (5), JSON-RPC protocol incl. hostile-input edge cases (7), tools end-to-end (1), node graph + blueprint tools incl. the P2 acceptance closed loop (5), safety core — audit trail, tier rejection, log capture, undo/redo round-trip, destructive tooling (5), P3b tool families — CDO get/set, asset search/info/refs/save, actor spawn/query/transform/destroy, variable add/flags, component add/attach/set, reparent (7), input + layout — input asset creation, mapping entries, auto_layout invariants (3), P5 widget + MVVM + gap-fill — class pin defaults, system-actor access, referencer-gated delete, widget-tree authoring, component bound events, MVVM authoring error contracts, binding list/remove lifecycle (7), P6 polish — widget rename incl. MVVM reference sync, dirty-package listing (2). **+4 P7 tests** (1.3-dev): FCreateAnimMontageTest, FAddAnimNotifyTest, FSetGeTargetTagsTest, FAddCompatibleSkeletonTest (each with multiple error-path assertions). **+2 level-automation tests** (1.3-dev): FLoadLevelErrorPathTest (missing map → isError + "not found"), FLoadLevelHappyPathTest (load real map → loaded=true + engine_info.current_level verification; skips gracefully when LoadMap is unavailable in headless mode).

## Smoke test (curl)

```powershell
Invoke-RestMethod -Uri http://127.0.0.1:18777/mcp -Method Post -ContentType "application/json" `
  -Body '{"jsonrpc":"2.0","id":1,"method":"tools/call","params":{"name":"engine_info"}}'
```

## Standalone repository

The plugin is self-contained under `Plugins/UnrealAgentMCP` (no dependencies on the host
project's module). To extract it into its own repository with full history:

```bash
# From the host project root — creates a branch containing only the plugin's history
git subtree split --prefix=Plugins/UnrealAgentMCP -b unreal-agent-mcp-standalone

# Push that branch into a fresh repo
git push <new-repo-url> unreal-agent-mcp-standalone:main
```

Consumers then drop the repo into any UE 5.5 project's `Plugins/` folder (or add it as a
git submodule) and build the editor target.

## License

MIT — see [LICENSE](LICENSE).
