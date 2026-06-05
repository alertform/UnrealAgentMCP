#include "Tools/EditorSessionTools.h"

#include "AssetRegistry/AssetData.h"
#include "AssetRegistry/AssetRegistryModule.h"
#include "AssetRegistry/IAssetRegistry.h"
#include "Containers/UnrealString.h"
#include "Core/AgentMcpAuditLog.h"
#include "Core/AgentMcpLogCapture.h"
#include "Core/AgentMcpToolRegistry.h"
#include "Core/McpTypes.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "Editor.h"
#include "FileHelpers.h"
#include "Misc/PackageName.h"
#include "Misc/Paths.h"
#include "Modules/ModuleManager.h"
#include "Tools/McpToolUtils.h"
#include "UnrealClient.h"

namespace
{
	using namespace AgentMcp;

	FAgentMcpToolResult HandleReadOutputLog(const TSharedPtr<FJsonObject>& Args)
	{
		int32 Count = 100;
		FString Filter, Category;
		if (Args.IsValid())
		{
			double Number = 0.0;
			if (Args->TryGetNumberField(TEXT("lines"), Number))
			{
				Count = FMath::Clamp(static_cast<int32>(Number), 1, FAgentMcpLogCapture::MaxLines);
			}
			Args->TryGetStringField(TEXT("filter"), Filter);
			Args->TryGetStringField(TEXT("category"), Category);
		}
		const TArray<FString> Recent = FAgentMcpLogCapture::Get().Recent(Count, Filter, Category);
		TArray<TSharedPtr<FJsonValue>> LineValues;
		LineValues.Reserve(Recent.Num());
		for (const FString& Line : Recent)
		{
			LineValues.Add(MakeShared<FJsonValueString>(Line));
		}
		TSharedRef<FJsonObject> Result = MakeShared<FJsonObject>();
		Result->SetNumberField(TEXT("returned"), Recent.Num());
		Result->SetArrayField(TEXT("lines"), LineValues);
		return FAgentMcpToolResult::Success(ToolUtils::SerializeObject(Result));
	}

	FAgentMcpToolResult HandleUndo(const TSharedPtr<FJsonObject>& /*Args*/)
	{
		if (!GEditor)
		{
			return FAgentMcpToolResult::Error(TEXT("GEditor unavailable (not running inside the editor)."));
		}
		const bool bUndone = GEditor->UndoTransaction();
		TSharedRef<FJsonObject> Result = MakeShared<FJsonObject>();
		Result->SetBoolField(TEXT("undone"), bUndone);
		return FAgentMcpToolResult::Success(ToolUtils::SerializeObject(Result));
	}

	FAgentMcpToolResult HandleRedo(const TSharedPtr<FJsonObject>& /*Args*/)
	{
		if (!GEditor)
		{
			return FAgentMcpToolResult::Error(TEXT("GEditor unavailable (not running inside the editor)."));
		}
		const bool bRedone = GEditor->RedoTransaction();
		TSharedRef<FJsonObject> Result = MakeShared<FJsonObject>();
		Result->SetBoolField(TEXT("redone"), bRedone);
		return FAgentMcpToolResult::Success(ToolUtils::SerializeObject(Result));
	}

	FAgentMcpToolResult HandleConsoleCommand(const TSharedPtr<FJsonObject>& Args)
	{
		FString Command;
		if (!Args.IsValid() || !Args->TryGetStringField(TEXT("command"), Command) || Command.TrimStartAndEnd().IsEmpty())
		{
			return FAgentMcpToolResult::Error(TEXT("console_command requires a non-empty 'command' string."));
		}
		if (!GEditor)
		{
			return FAgentMcpToolResult::Error(TEXT("GEditor unavailable (not running inside the editor)."));
		}
		FStringOutputDevice Output;
		Output.SetAutoEmitLineTerminator(false);
		const bool bHandled = GEditor->Exec(GEditor->GetEditorWorldContext().World(), *Command, Output);
		TSharedRef<FJsonObject> Result = MakeShared<FJsonObject>();
		Result->SetBoolField(TEXT("handled"), bHandled);
		Result->SetStringField(TEXT("output"), Output);
		return FAgentMcpToolResult::Success(ToolUtils::SerializeObject(Result));
	}

	FAgentMcpToolResult HandleTakeScreenshot(const TSharedPtr<FJsonObject>& Args)
	{
		FString Filename = TEXT("AgentMcp");
		if (Args.IsValid())
		{
			Args->TryGetStringField(TEXT("filename"), Filename);
		}
		// Sanitize: bare name only, no path traversal; engine writes under Saved/Screenshots.
		Filename = FPaths::GetCleanFilename(Filename);
		if (Filename.IsEmpty())
		{
			Filename = TEXT("AgentMcp");
		}
		FScreenshotRequest::RequestScreenshot(Filename, /*bInShowUI=*/false, /*bAddFilenameSuffix=*/true);
		// Editor viewports redraw lazily (non-realtime viewports skip Draw when idle) and the request
		// is only serviced inside a Draw — force one so the file actually materializes.
		if (GEditor)
		{
			GEditor->RedrawAllViewports();
		}
		TSharedRef<FJsonObject> Result = MakeShared<FJsonObject>();
		Result->SetBoolField(TEXT("queued"), true);
		// Build the expected path locally: FScreenshotRequest::GetFilename() is a shared static that
		// is only populated when a request is SERVICED on a rendered frame — reading it here would
		// return the previous screenshot's path (or nothing), not this request's.
		Result->SetStringField(TEXT("path"), FPaths::Combine(FPaths::ScreenShotDir(), Filename));
		Result->SetStringField(TEXT("note"), TEXT("Captured on the next rendered frame; the engine appends a numeric suffix to the filename. With no viewport rendering (headless) the file never materializes."));
		return FAgentMcpToolResult::Success(ToolUtils::SerializeObject(Result));
	}

	FAgentMcpToolResult HandleAuditTail(const TSharedPtr<FJsonObject>& Args)
	{
		int32 Count = 50;
		if (Args.IsValid())
		{
			double Number = 0.0;
			if (Args->TryGetNumberField(TEXT("lines"), Number))
			{
				Count = FMath::Clamp(static_cast<int32>(Number), 1, 500);
			}
		}
		const TArray<FString> Entries = FAgentMcpAuditLog::Get().Tail(Count);
		TArray<TSharedPtr<FJsonValue>> EntryValues;
		EntryValues.Reserve(Entries.Num());
		for (const FString& Entry : Entries)
		{
			EntryValues.Add(MakeShared<FJsonValueString>(Entry));
		}
		TSharedRef<FJsonObject> Result = MakeShared<FJsonObject>();
		Result->SetStringField(TEXT("file"), FAgentMcpAuditLog::Get().CurrentFilePath());
		Result->SetNumberField(TEXT("returned"), Entries.Num());
		Result->SetArrayField(TEXT("entries"), EntryValues);
		return FAgentMcpToolResult::Success(ToolUtils::SerializeObject(Result));
	}

	FAgentMcpToolResult HandleListDirtyPackages(const TSharedPtr<FJsonObject>& /*Args*/)
	{
		TArray<UPackage*> DirtyMaps;
		TArray<UPackage*> DirtyContent;
		UEditorLoadingAndSavingUtils::GetDirtyMapPackages(DirtyMaps);
		UEditorLoadingAndSavingUtils::GetDirtyContentPackages(DirtyContent);

		TArray<TSharedPtr<FJsonValue>> MapNames;
		MapNames.Reserve(DirtyMaps.Num());
		for (UPackage* Pkg : DirtyMaps)
		{
			if (IsValid(Pkg)) // null AND pending-kill guard (T4 review)
			{
				MapNames.Add(MakeShared<FJsonValueString>(Pkg->GetName()));
			}
		}

		TArray<TSharedPtr<FJsonValue>> ContentNames;
		ContentNames.Reserve(DirtyContent.Num());
		for (UPackage* Pkg : DirtyContent)
		{
			if (IsValid(Pkg)) // null AND pending-kill guard (T4 review)
			{
				ContentNames.Add(MakeShared<FJsonValueString>(Pkg->GetName()));
			}
		}

		TSharedRef<FJsonObject> Result = MakeShared<FJsonObject>();
		Result->SetNumberField(TEXT("count"), MapNames.Num() + ContentNames.Num());
		Result->SetArrayField(TEXT("maps"), MapNames);
		Result->SetArrayField(TEXT("content"), ContentNames);
		return FAgentMcpToolResult::Success(ToolUtils::SerializeObject(Result));
	}

	FAgentMcpToolResult HandleLoadLevel(const TSharedPtr<FJsonObject>& Args)
	{
		FString MapPath;
		if (!Args.IsValid() || !Args->TryGetStringField(TEXT("map_path"), MapPath) || MapPath.TrimStartAndEnd().IsEmpty())
		{
			return FAgentMcpToolResult::Error(TEXT("load_level requires a non-empty 'map_path' string (e.g. /Game/Maps/ThirdPersonMap)."));
		}

		// Normalise: strip the trailing .MapName suffix if the caller passed the long form (e.g. /Game/X/Map.Map).
		// Asset registry always stores the package name without the object suffix.
		{
			int32 DotIdx;
			if (MapPath.FindLastChar(TEXT('.'), DotIdx))
			{
				MapPath = MapPath.Left(DotIdx);
			}
		}

		// --- Step 1: Confirm the asset exists in the registry and is a World. ---
		FAssetRegistryModule& RegistryMod = FModuleManager::LoadModuleChecked<FAssetRegistryModule>(TEXT("AssetRegistry"));
		IAssetRegistry& AR = RegistryMod.Get();
		AR.WaitForCompletion();

		FAssetData AssetData = AR.GetAssetByObjectPath(FSoftObjectPath(MapPath + TEXT(".") + FPackageName::GetShortName(MapPath)));
		if (!AssetData.IsValid())
		{
			// Fallback: some registry versions need the package name directly.
			TArray<FAssetData> Found;
			AR.GetAssetsByPackageName(FName(*MapPath), Found);
			if (Found.Num() > 0)
			{
				AssetData = Found[0];
			}
		}

		if (!AssetData.IsValid())
		{
			return FAgentMcpToolResult::Error(
				FString::Printf(TEXT("Map not found: '%s'. Use list_assets or search_assets to discover maps."), *MapPath));
		}

		// Verify the asset class is World.
		const FString AssetClassName = AssetData.AssetClassPath.GetAssetName().ToString();
		if (AssetClassName != TEXT("World"))
		{
			return FAgentMcpToolResult::Error(
				FString::Printf(TEXT("Asset '%s' is a '%s', not a World. Provide a map (World) asset path."), *MapPath, *AssetClassName));
		}

		// --- Step 2: Dirty-package guard. Never trigger a save dialog (unattended would hang). ---
		{
			TArray<UPackage*> DirtyMaps;
			TArray<UPackage*> DirtyContent;
			UEditorLoadingAndSavingUtils::GetDirtyMapPackages(DirtyMaps);
			UEditorLoadingAndSavingUtils::GetDirtyContentPackages(DirtyContent);

			TArray<FString> DirtyNames;
			for (UPackage* Pkg : DirtyMaps)
			{
				if (IsValid(Pkg)) DirtyNames.Add(Pkg->GetName());
			}
			for (UPackage* Pkg : DirtyContent)
			{
				if (IsValid(Pkg)) DirtyNames.Add(Pkg->GetName());
			}

			if (DirtyNames.Num() > 0)
			{
				FString DirtyList;
				for (const FString& Name : DirtyNames)
				{
					DirtyList += TEXT("  ") + Name + TEXT("\n");
				}
				return FAgentMcpToolResult::Error(
					FString::Printf(TEXT("Cannot load map: %d unsaved package(s) detected. Save via save_asset first:\n%s"),
						DirtyNames.Num(), *DirtyList));
			}
		}

		// --- Step 3: Load the map. LoadMap is a global editor operation and is NOT undoable. ---
		if (!GEditor)
		{
			return FAgentMcpToolResult::Error(TEXT("GEditor unavailable (not running inside the editor)."));
		}

		UWorld* NewWorld = UEditorLoadingAndSavingUtils::LoadMap(MapPath);
		if (!NewWorld)
		{
			return FAgentMcpToolResult::Error(
				FString::Printf(TEXT("LoadMap returned null for '%s'. The file may be corrupted or on a read-only mount."), *MapPath));
		}

		TSharedRef<FJsonObject> Result = MakeShared<FJsonObject>();
		Result->SetBoolField(TEXT("loaded"), true);
		Result->SetStringField(TEXT("world"), NewWorld->GetOutermost()->GetName());
		return FAgentMcpToolResult::Success(ToolUtils::SerializeObject(Result));
	}
}

void AgentMcp::Tools::RegisterEditorSessionTools()
{
	{
		FAgentMcpToolDef Def;
		Def.Name = TEXT("read_output_log");
		Def.Description = TEXT("Returns recent editor Output Log lines from an in-memory ring buffer (last 2000 lines since editor start). Args: lines (int, default 100), filter (case-insensitive substring), category (exact log category, e.g. LogBlueprint). Use after compile_blueprint or console_command to inspect engine output.");
		Def.InputSchema = MakeShared<FJsonObject>();
		Def.InputSchema->SetStringField(TEXT("type"), TEXT("object"));
		{
			TSharedRef<FJsonObject> Properties = MakeShared<FJsonObject>();
			TSharedRef<FJsonObject> LinesProp = MakeShared<FJsonObject>();
			LinesProp->SetStringField(TEXT("type"), TEXT("integer"));
			LinesProp->SetStringField(TEXT("description"), TEXT("Max lines to return (1-2000, default 100)"));
			Properties->SetObjectField(TEXT("lines"), LinesProp);
			TSharedRef<FJsonObject> FilterProp = MakeShared<FJsonObject>();
			FilterProp->SetStringField(TEXT("type"), TEXT("string"));
			FilterProp->SetStringField(TEXT("description"), TEXT("Case-insensitive substring filter"));
			Properties->SetObjectField(TEXT("filter"), FilterProp);
			TSharedRef<FJsonObject> CategoryProp = MakeShared<FJsonObject>();
			CategoryProp->SetStringField(TEXT("type"), TEXT("string"));
			CategoryProp->SetStringField(TEXT("description"), TEXT("Exact log category name, e.g. LogAgentMcp"));
			Properties->SetObjectField(TEXT("category"), CategoryProp);
			Def.InputSchema->SetObjectField(TEXT("properties"), Properties);
		}
		Def.Tier = EAgentMcpTier::ReadOnly;
		Def.Handler = FAgentMcpToolHandler::CreateStatic(&HandleReadOutputLog);
		FAgentMcpToolRegistry::Get().Register(MoveTemp(Def));
	}
	{
		FAgentMcpToolDef Def;
		Def.Name = TEXT("undo");
		Def.Description = TEXT("Undoes the most recent editor transaction (Ctrl+Z) on the EDITOR-WIDE undo stack - the top transaction may be a user's manual edit, not necessarily an MCP operation. Only call when you know no concurrent user editing is happening. Returns {undone: bool}; false means the undo stack was empty or unavailable.");
		Def.InputSchema = MakeShared<FJsonObject>();
		Def.InputSchema->SetStringField(TEXT("type"), TEXT("object"));
		Def.InputSchema->SetObjectField(TEXT("properties"), MakeShared<FJsonObject>());
		Def.Tier = EAgentMcpTier::SafeWrite;
		Def.Handler = FAgentMcpToolHandler::CreateStatic(&HandleUndo);
		FAgentMcpToolRegistry::Get().Register(MoveTemp(Def));
	}
	{
		FAgentMcpToolDef Def;
		Def.Name = TEXT("redo");
		Def.Description = TEXT("Redoes the most recently undone editor transaction (Ctrl+Y) on the EDITOR-WIDE redo stack (same bluntness caveat as undo). Returns {redone: bool}; false means nothing to redo.");
		Def.InputSchema = MakeShared<FJsonObject>();
		Def.InputSchema->SetStringField(TEXT("type"), TEXT("object"));
		Def.InputSchema->SetObjectField(TEXT("properties"), MakeShared<FJsonObject>());
		Def.Tier = EAgentMcpTier::SafeWrite;
		Def.Handler = FAgentMcpToolHandler::CreateStatic(&HandleRedo);
		FAgentMcpToolRegistry::Get().Register(MoveTemp(Def));
	}
	{
		FAgentMcpToolDef Def;
		Def.Name = TEXT("console_command");
		Def.Description = TEXT("DESTRUCTIVE tier: executes an arbitrary Unreal console command (e.g. stat fps, gc.CollectGarbageEveryFrame 1) including commands that can quit or modify editor state. Requires PermissionTier raised to Destructive in Project Settings > Plugins > Unreal Agent MCP. Returns {handled: bool, output: string} - output captures only what the command writes to the provided device; many commands log to the engine log instead (use read_output_log to see that).");
		Def.InputSchema = MakeShared<FJsonObject>();
		Def.InputSchema->SetStringField(TEXT("type"), TEXT("object"));
		{
			TSharedRef<FJsonObject> Properties = MakeShared<FJsonObject>();
			TSharedRef<FJsonObject> CommandProp = MakeShared<FJsonObject>();
			CommandProp->SetStringField(TEXT("type"), TEXT("string"));
			CommandProp->SetStringField(TEXT("description"), TEXT("The console command string to execute (e.g. 'stat fps', 'quit')."));
			Properties->SetObjectField(TEXT("command"), CommandProp);
			Def.InputSchema->SetObjectField(TEXT("properties"), Properties);
			TArray<TSharedPtr<FJsonValue>> Required;
			Required.Add(MakeShared<FJsonValueString>(TEXT("command")));
			Def.InputSchema->SetArrayField(TEXT("required"), Required);
		}
		Def.Tier = EAgentMcpTier::Destructive;
		Def.Handler = FAgentMcpToolHandler::CreateStatic(&HandleConsoleCommand);
		FAgentMcpToolRegistry::Get().Register(MoveTemp(Def));
	}
	{
		FAgentMcpToolDef Def;
		Def.Name = TEXT("take_screenshot");
		Def.Description = TEXT("Queues a screenshot request; the file is written on the next rendered frame under Saved/Screenshots/. With no viewport rendering (NullRHI / headless) the file never materializes — use for E2E validation only. Returns {queued: true, path, note}.");
		Def.InputSchema = MakeShared<FJsonObject>();
		Def.InputSchema->SetStringField(TEXT("type"), TEXT("object"));
		{
			TSharedRef<FJsonObject> Properties = MakeShared<FJsonObject>();
			TSharedRef<FJsonObject> FilenameProp = MakeShared<FJsonObject>();
			FilenameProp->SetStringField(TEXT("type"), TEXT("string"));
			FilenameProp->SetStringField(TEXT("description"), TEXT("Base filename (no extension, no path). Defaults to 'AgentMcp'. A numeric suffix is always appended."));
			Properties->SetObjectField(TEXT("filename"), FilenameProp);
			Def.InputSchema->SetObjectField(TEXT("properties"), Properties);
		}
		// ReadOnly by the tier's letter: a screenshot writes a PNG under Saved/Screenshots (disk
		// side-effect) but touches no EDITOR STATE — no transaction, no asset, nothing undoable.
		Def.Tier = EAgentMcpTier::ReadOnly;
		Def.Handler = FAgentMcpToolHandler::CreateStatic(&HandleTakeScreenshot);
		FAgentMcpToolRegistry::Get().Register(MoveTemp(Def));
	}
	{
		FAgentMcpToolDef Def;
		Def.Name = TEXT("audit_tail");
		Def.Description = TEXT("Returns the last N lines of today's JSONL audit log (Saved/AgentMCP/audit-YYYYMMDD.jsonl). Each entry records tool, args digest, duration, is_error, rejected_by_tier. Returns {file, returned, entries:[raw JSONL strings]}.");
		Def.InputSchema = MakeShared<FJsonObject>();
		Def.InputSchema->SetStringField(TEXT("type"), TEXT("object"));
		{
			TSharedRef<FJsonObject> Properties = MakeShared<FJsonObject>();
			TSharedRef<FJsonObject> LinesProp = MakeShared<FJsonObject>();
			LinesProp->SetStringField(TEXT("type"), TEXT("integer"));
			LinesProp->SetStringField(TEXT("description"), TEXT("Max entries to return (1-500, default 50)"));
			Properties->SetObjectField(TEXT("lines"), LinesProp);
			Def.InputSchema->SetObjectField(TEXT("properties"), Properties);
		}
		Def.Tier = EAgentMcpTier::ReadOnly;
		Def.Handler = FAgentMcpToolHandler::CreateStatic(&HandleAuditTail);
		FAgentMcpToolRegistry::Get().Register(MoveTemp(Def));
	}
	{
		FAgentMcpToolDef Def;
		Def.Name = TEXT("list_dirty_packages");
		Def.Description = TEXT("Lists unsaved (dirty) packages — maps and content separately. Call before closing the editor or after a batch of edits to know what needs persisting. Content packages: save via save_asset. Map packages: must be saved in-editor (File > Save) — save_asset refuses them by design.");
		Def.InputSchema = MakeShared<FJsonObject>();
		Def.InputSchema->SetStringField(TEXT("type"), TEXT("object"));
		Def.InputSchema->SetObjectField(TEXT("properties"), MakeShared<FJsonObject>());
		Def.Tier = EAgentMcpTier::ReadOnly;
		Def.Handler = FAgentMcpToolHandler::CreateStatic(&HandleListDirtyPackages);
		FAgentMcpToolRegistry::Get().Register(MoveTemp(Def));
	}
	{
		FAgentMcpToolDef Def;
		Def.Name = TEXT("load_level");
		Def.Description = TEXT("Loads a map (World asset) into the editor as the active level. "
			"Accepts both package form (/Game/Maps/ThirdPersonMap) and object-path form (/Game/Maps/ThirdPersonMap.ThirdPersonMap). "
			"Validates the asset exists and is a World before loading. "
			"Aborts with an error listing all dirty packages if any unsaved maps or content exist — save via save_asset first. "
			"NOTE: this is a global editor operation (equivalent to File > Open Level) and is NOT undoable. "
			"Returns {loaded: true, world: \"<package name>\"}.");
		Def.InputSchema = MakeShared<FJsonObject>();
		Def.InputSchema->SetStringField(TEXT("type"), TEXT("object"));
		{
			TSharedRef<FJsonObject> Properties = MakeShared<FJsonObject>();
			TSharedRef<FJsonObject> MapPathProp = MakeShared<FJsonObject>();
			MapPathProp->SetStringField(TEXT("type"), TEXT("string"));
			MapPathProp->SetStringField(TEXT("description"), TEXT("Package path of the map to load, e.g. /Game/Maps/ThirdPersonMap. Also accepts the full object path with suffix."));
			Properties->SetObjectField(TEXT("map_path"), MapPathProp);
			Def.InputSchema->SetObjectField(TEXT("properties"), Properties);
			TArray<TSharedPtr<FJsonValue>> Required;
			Required.Add(MakeShared<FJsonValueString>(TEXT("map_path")));
			Def.InputSchema->SetArrayField(TEXT("required"), Required);
		}
		Def.Tier = EAgentMcpTier::SafeWrite;
		Def.Handler = FAgentMcpToolHandler::CreateStatic(&HandleLoadLevel);
		FAgentMcpToolRegistry::Get().Register(MoveTemp(Def));
	}
}
