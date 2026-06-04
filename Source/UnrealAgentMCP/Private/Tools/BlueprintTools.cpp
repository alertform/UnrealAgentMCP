#include "Tools/BlueprintTools.h"

#include "BlueprintEditorLibrary.h"
#include "Core/AgentMcpToolRegistry.h"
#include "Core/McpTypes.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "Engine/Blueprint.h"
#include "GameFramework/Actor.h"
#include "Kismet2/CompilerResultsLog.h"
#include "Kismet2/KismetEditorUtilities.h"
#include "Logging/TokenizedMessage.h"
#include "Tools/McpToolUtils.h"
#include "Tools/NodeGraphUtils.h"
#include "UObject/UObjectGlobals.h"

namespace
{
	using namespace AgentMcp;

	FAgentMcpToolResult HandleCreateBlueprint(const TSharedPtr<FJsonObject>& Args)
	{
		FString AssetPath;
		if (!Args.IsValid() || !Args->TryGetStringField(TEXT("asset_path"), AssetPath))
		{
			return FAgentMcpToolResult::Error(TEXT("Missing required string argument 'asset_path' (e.g. /Game/Dev/BP_New)."));
		}
		if (!AssetPath.StartsWith(TEXT("/")))
		{
			return FAgentMcpToolResult::Error(FString::Printf(TEXT("'asset_path' must be an absolute package path. Got: '%s'"), *AssetPath));
		}

		FString ParentClassName = TEXT("Actor");
		Args->TryGetStringField(TEXT("parent_class"), ParentClassName);

		// Resolve class: accept short names (e.g. "Actor") or full paths (e.g. "/Script/Engine.Actor").
		// Use StaticFindFirstObject with the full path when possible to avoid the TryFindTypeSlow
		// "short type name" log warning. Fall back to TryFindTypeSlow for legacy short names.
		UClass* ParentClass = nullptr;
		if (ParentClassName.Contains(TEXT(".")))
		{
			ParentClass = FindObject<UClass>(nullptr, *ParentClassName);
		}
		if (!ParentClass)
		{
			ParentClass = UClass::TryFindTypeSlow<UClass>(ParentClassName);
		}
		if (!ParentClass)
		{
			return FAgentMcpToolResult::Error(FString::Printf(TEXT("Parent class '%s' not found. Use a short name like 'Actor' or a full path like '/Script/Engine.Actor'."), *ParentClassName));
		}

		// Delegate creation (and duplicate detection) entirely to the library.
		// CreateBlueprintAssetWithParent checks FindObject<UPackage> internally and returns nullptr on collision.
		// No FScopedTransaction here: asset creation does not go through the editor undo system
		// (no Modify() records are written). Undo for created assets = delete_asset, which is P3
		// Destructive-tier territory.
		UBlueprint* Blueprint = UBlueprintEditorLibrary::CreateBlueprintAssetWithParent(AssetPath, ParentClass);
		if (!Blueprint)
		{
			return FAgentMcpToolResult::Error(FString::Printf(TEXT("Failed to create blueprint at '%s' (asset already exists, invalid path, or non-Blueprintable parent class)."), *AssetPath));
		}

		TSharedRef<FJsonObject> Result = MakeShared<FJsonObject>();
		Result->SetStringField(TEXT("blueprint_path"), Blueprint->GetPathName());
		Result->SetStringField(TEXT("generated_class"), Blueprint->GeneratedClass ? Blueprint->GeneratedClass->GetPathName() : TEXT(""));
		return FAgentMcpToolResult::Success(ToolUtils::SerializeObject(Result));
	}

	FAgentMcpToolResult HandleCompileBlueprint(const TSharedPtr<FJsonObject>& Args)
	{
		FString BlueprintPath;
		if (!Args.IsValid() || !Args->TryGetStringField(TEXT("blueprint_path"), BlueprintPath))
		{
			return FAgentMcpToolResult::Error(TEXT("Missing required string argument 'blueprint_path'."));
		}
		FString Error;
		UBlueprint* Blueprint = NodeGraphUtils::ResolveBlueprint(BlueprintPath, Error);
		if (!Blueprint)
		{
			return FAgentMcpToolResult::Error(Error);
		}

		FCompilerResultsLog Results;
		FKismetEditorUtilities::CompileBlueprint(Blueprint, EBlueprintCompileOptions::None, &Results);

		TArray<TSharedPtr<FJsonValue>> Messages;
		for (const TSharedRef<FTokenizedMessage>& Message : Results.Messages)
		{
			TSharedRef<FJsonObject> Entry = MakeShared<FJsonObject>();
			Entry->SetStringField(TEXT("severity"),
				Message->GetSeverity() == EMessageSeverity::Error ? TEXT("error") : TEXT("warning"));
			Entry->SetStringField(TEXT("text"), Message->ToText().ToString());
			Messages.Add(MakeShared<FJsonValueObject>(Entry));
		}

		TSharedRef<FJsonObject> Result = MakeShared<FJsonObject>();
		Result->SetStringField(TEXT("status"), Results.NumErrors > 0 ? TEXT("error") : TEXT("ok"));
		Result->SetNumberField(TEXT("num_errors"), Results.NumErrors);
		Result->SetNumberField(TEXT("num_warnings"), Results.NumWarnings);
		Result->SetArrayField(TEXT("messages"), Messages);
		// Compile errors are the agent's correction signal, NOT a tool failure.
		return FAgentMcpToolResult::Success(ToolUtils::SerializeObject(Result));
	}
}

void AgentMcp::Tools::RegisterBlueprintTools()
{
	{
		FAgentMcpToolDef Def;
		Def.Name = TEXT("create_blueprint");
		Def.Description = TEXT("Creates a new Blueprint asset in memory (NOT saved to disk yet). Args: asset_path (required, e.g. /Game/Dev/BP_New), parent_class (default Actor; short name or /Script/ path). Returns {blueprint_path, generated_class}.");
		Def.InputSchema = MakeShared<FJsonObject>();
		Def.InputSchema->SetStringField(TEXT("type"), TEXT("object"));
		{
			TSharedRef<FJsonObject> Properties = MakeShared<FJsonObject>();
			TSharedRef<FJsonObject> PathProp = MakeShared<FJsonObject>();
			PathProp->SetStringField(TEXT("type"), TEXT("string"));
			PathProp->SetStringField(TEXT("description"), TEXT("Absolute asset path for the new Blueprint, e.g. /Game/Dev/BP_New"));
			Properties->SetObjectField(TEXT("asset_path"), PathProp);
			TSharedRef<FJsonObject> ParentProp = MakeShared<FJsonObject>();
			ParentProp->SetStringField(TEXT("type"), TEXT("string"));
			ParentProp->SetStringField(TEXT("description"), TEXT("Parent class: short name (Actor) or full path (/Script/Engine.Actor). Default Actor."));
			Properties->SetObjectField(TEXT("parent_class"), ParentProp);
			Def.InputSchema->SetObjectField(TEXT("properties"), Properties);
			TArray<TSharedPtr<FJsonValue>> Required;
			Required.Add(MakeShared<FJsonValueString>(TEXT("asset_path")));
			Def.InputSchema->SetArrayField(TEXT("required"), Required);
		}
		Def.Tier = EAgentMcpTier::SafeWrite;
		Def.Handler = FAgentMcpToolHandler::CreateStatic(&HandleCreateBlueprint);
		FAgentMcpToolRegistry::Get().Register(MoveTemp(Def));
	}
	{
		FAgentMcpToolDef Def;
		Def.Name = TEXT("compile_blueprint");
		Def.Description = TEXT("Compiles a Blueprint and returns {status, num_errors, num_warnings, messages[]}. Compile errors come back in messages (isError stays false) - read them and fix the graph. Args: blueprint_path (required).");
		Def.InputSchema = MakeShared<FJsonObject>();
		Def.InputSchema->SetStringField(TEXT("type"), TEXT("object"));
		{
			TSharedRef<FJsonObject> Properties = MakeShared<FJsonObject>();
			TSharedRef<FJsonObject> PathProp = MakeShared<FJsonObject>();
			PathProp->SetStringField(TEXT("type"), TEXT("string"));
			PathProp->SetStringField(TEXT("description"), TEXT("Path of the Blueprint to compile, e.g. /Game/Dev/BP_New"));
			Properties->SetObjectField(TEXT("blueprint_path"), PathProp);
			Def.InputSchema->SetObjectField(TEXT("properties"), Properties);
			TArray<TSharedPtr<FJsonValue>> Required;
			Required.Add(MakeShared<FJsonValueString>(TEXT("blueprint_path")));
			Def.InputSchema->SetArrayField(TEXT("required"), Required);
		}
		Def.Tier = EAgentMcpTier::SafeWrite;
		Def.Handler = FAgentMcpToolHandler::CreateStatic(&HandleCompileBlueprint);
		FAgentMcpToolRegistry::Get().Register(MoveTemp(Def));
	}
}
