#include "Tools/BlueprintTools.h"

#include "BlueprintEditorLibrary.h"
#include "Core/AgentMcpToolRegistry.h"
#include "Core/McpTypes.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "Engine/Blueprint.h"
#include "GameFramework/Actor.h"
#include "Kismet2/BlueprintEditorUtils.h"
#include "Kismet2/CompilerResultsLog.h"
#include "Kismet2/KismetEditorUtilities.h"
#include "Logging/TokenizedMessage.h"
#include "ScopedTransaction.h"
#include "Tools/McpToolUtils.h"
#include "Tools/NodeGraphUtils.h"
#include "Tools/PropertyBridge.h"
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

	/** Shared: resolve blueprint_path arg -> CDO. Returns nullptr + OutError on failure. */
	UObject* ResolveCdo(const TSharedPtr<FJsonObject>& Args, UBlueprint*& OutBlueprint, FString& OutError)
	{
		FString BlueprintPath;
		if (!Args.IsValid() || !Args->TryGetStringField(TEXT("blueprint_path"), BlueprintPath))
		{
			OutError = TEXT("Missing required string argument 'blueprint_path'.");
			return nullptr;
		}
		OutBlueprint = NodeGraphUtils::ResolveBlueprint(BlueprintPath, OutError);
		if (!OutBlueprint)
		{
			return nullptr;
		}
		UClass* Generated = OutBlueprint->GeneratedClass;
		if (!Generated || !Generated->GetDefaultObject())
		{
			OutError = TEXT("Blueprint has no generated class/CDO yet - call compile_blueprint first.");
			return nullptr;
		}
		return Generated->GetDefaultObject();
	}

	FAgentMcpToolResult HandleGetCdoProperty(const TSharedPtr<FJsonObject>& Args)
	{
		UBlueprint* Blueprint = nullptr;
		FString Error;
		UObject* Cdo = ResolveCdo(Args, Blueprint, Error);
		if (!Cdo) { return FAgentMcpToolResult::Error(Error); }

		FString PropertyName;
		if (!Args->TryGetStringField(TEXT("property"), PropertyName))
		{
			return FAgentMcpToolResult::Error(TEXT("Missing required string argument 'property'."));
		}
		FString Value, Type;
		if (!PropertyBridge::GetPropertyAsString(Cdo, PropertyName, Value, Type, Error))
		{
			return FAgentMcpToolResult::Error(Error);
		}
		TSharedRef<FJsonObject> Result = MakeShared<FJsonObject>();
		Result->SetStringField(TEXT("property"), PropertyName);
		Result->SetStringField(TEXT("value"), Value);
		Result->SetStringField(TEXT("type"), Type);
		return FAgentMcpToolResult::Success(ToolUtils::SerializeObject(Result));
	}

	FAgentMcpToolResult HandleReparentBlueprint(const TSharedPtr<FJsonObject>& Args)
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

		FString NewParentClassName;
		if (!Args->TryGetStringField(TEXT("new_parent_class"), NewParentClassName))
		{
			return FAgentMcpToolResult::Error(TEXT("Missing required string argument 'new_parent_class'."));
		}

		// Resolve class by short name or full path.
		UClass* NewClass = nullptr;
		if (NewParentClassName.Contains(TEXT(".")))
		{
			NewClass = FindObject<UClass>(nullptr, *NewParentClassName);
		}
		if (!NewClass)
		{
			NewClass = UClass::TryFindTypeSlow<UClass>(NewParentClassName);
		}
		if (!NewClass)
		{
			return FAgentMcpToolResult::Error(FString::Printf(TEXT("Class '%s' not found. Use a short name like 'Pawn' or a full path like '/Script/Engine.Pawn'."), *NewParentClassName));
		}

		// Cycle guard: prevent reparenting to the blueprint's own generated class or a subclass of it.
		if (Blueprint->GeneratedClass && NewClass->IsChildOf(Blueprint->GeneratedClass))
		{
			return FAgentMcpToolResult::Error(FString::Printf(TEXT("Reparenting to '%s' would create an inheritance cycle (it is a child of this blueprint's generated class)."), *NewParentClassName));
		}

		FScopedTransaction Transaction(NSLOCTEXT("AgentMcp", "ReparentBlueprint", "MCP: Reparent Blueprint"));

		// ReparentBlueprint is void — verify effect via ParentClass afterwards.
		UBlueprintEditorLibrary::ReparentBlueprint(Blueprint, NewClass);

		if (Blueprint->ParentClass != NewClass)
		{
			Transaction.Cancel();
			return FAgentMcpToolResult::Error(FString::Printf(TEXT("ReparentBlueprint did not take effect for class '%s'. The class may not be Blueprintable or compatible."), *NewParentClassName));
		}

		FBlueprintEditorUtils::MarkBlueprintAsStructurallyModified(Blueprint);

		TSharedRef<FJsonObject> Result = MakeShared<FJsonObject>();
		Result->SetBoolField(TEXT("reparented"), true);
		Result->SetStringField(TEXT("new_parent"), NewClass->GetPathName());
		return FAgentMcpToolResult::Success(ToolUtils::SerializeObject(Result));
	}

	FAgentMcpToolResult HandleSetCdoProperty(const TSharedPtr<FJsonObject>& Args)
	{
		UBlueprint* Blueprint = nullptr;
		FString Error;
		UObject* Cdo = ResolveCdo(Args, Blueprint, Error);
		if (!Cdo) { return FAgentMcpToolResult::Error(Error); }

		FString PropertyName, Value;
		if (!Args->TryGetStringField(TEXT("property"), PropertyName) || !Args->TryGetStringField(TEXT("value"), Value))
		{
			return FAgentMcpToolResult::Error(TEXT("set_cdo_property requires 'property' and 'value' strings."));
		}

		// Non-const so failure paths can Cancel() — avoids leaving an empty undo entry when
		// validation fails after Modify() (mirrors the add_node Cancel pattern in NodeGraphTools.cpp).
		FScopedTransaction Transaction(NSLOCTEXT("AgentMcp", "SetCdoProperty", "MCP: Set CDO Property"));
		Cdo->Modify();

		if (!PropertyBridge::SetPropertyFromString(Cdo, PropertyName, Value, Error, /*bRejectTemplateDisabled=*/true))
		{
			Transaction.Cancel();
			return FAgentMcpToolResult::Error(Error);
		}
		FBlueprintEditorUtils::MarkBlueprintAsModified(Blueprint);

		FString ReadBack, Type;
		PropertyBridge::GetPropertyAsString(Cdo, PropertyName, ReadBack, Type, Error);

		TSharedRef<FJsonObject> Result = MakeShared<FJsonObject>();
		Result->SetBoolField(TEXT("set"), true);
		Result->SetStringField(TEXT("property"), PropertyName);
		Result->SetStringField(TEXT("value"), ReadBack);
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
	{
		FAgentMcpToolDef Def;
		Def.Name = TEXT("get_cdo_property");
		Def.Description = TEXT("Reads a property value from a Blueprint's Class Default Object (CDO) and returns it as a UE text string. Args: blueprint_path (required), property (required, case-sensitive C++ member name, e.g. bCanBeDamaged). Returns {property, value, type}.");
		Def.InputSchema = MakeShared<FJsonObject>();
		Def.InputSchema->SetStringField(TEXT("type"), TEXT("object"));
		{
			TSharedRef<FJsonObject> Properties = MakeShared<FJsonObject>();
			TSharedRef<FJsonObject> PathProp = MakeShared<FJsonObject>();
			PathProp->SetStringField(TEXT("type"), TEXT("string"));
			PathProp->SetStringField(TEXT("description"), TEXT("Path of the Blueprint, e.g. /Game/Dev/BP_MyActor"));
			Properties->SetObjectField(TEXT("blueprint_path"), PathProp);
			TSharedRef<FJsonObject> PropProp = MakeShared<FJsonObject>();
			PropProp->SetStringField(TEXT("type"), TEXT("string"));
			PropProp->SetStringField(TEXT("description"), TEXT("Case-sensitive C++ property name on the CDO, e.g. bCanBeDamaged"));
			Properties->SetObjectField(TEXT("property"), PropProp);
			Def.InputSchema->SetObjectField(TEXT("properties"), Properties);
			TArray<TSharedPtr<FJsonValue>> Required;
			Required.Add(MakeShared<FJsonValueString>(TEXT("blueprint_path")));
			Required.Add(MakeShared<FJsonValueString>(TEXT("property")));
			Def.InputSchema->SetArrayField(TEXT("required"), Required);
		}
		Def.Tier = EAgentMcpTier::ReadOnly;
		Def.Handler = FAgentMcpToolHandler::CreateStatic(&HandleGetCdoProperty);
		FAgentMcpToolRegistry::Get().Register(MoveTemp(Def));
	}
	{
		FAgentMcpToolDef Def;
		Def.Name = TEXT("set_cdo_property");
		Def.Description = TEXT("Sets a property on a Blueprint's Class Default Object (CDO). value uses UE ImportText syntax (True/False for booleans, 42 for integers, (X=1,Y=2,Z=3) for vectors, /Script/... for object paths). Only EditAnywhere/EditDefaultsOnly properties are accepted; non-editable properties return an error. Note: object-reference values (/Game/...) synchronously LOAD the referenced asset into the editor. Changes propagate to instances on next compile_blueprint. Args: blueprint_path (required), property (required), value (required). Returns {set, property, value (readback)}.");
		Def.InputSchema = MakeShared<FJsonObject>();
		Def.InputSchema->SetStringField(TEXT("type"), TEXT("object"));
		{
			TSharedRef<FJsonObject> Properties = MakeShared<FJsonObject>();
			TSharedRef<FJsonObject> PathProp = MakeShared<FJsonObject>();
			PathProp->SetStringField(TEXT("type"), TEXT("string"));
			PathProp->SetStringField(TEXT("description"), TEXT("Path of the Blueprint, e.g. /Game/Dev/BP_MyActor"));
			Properties->SetObjectField(TEXT("blueprint_path"), PathProp);
			TSharedRef<FJsonObject> PropProp = MakeShared<FJsonObject>();
			PropProp->SetStringField(TEXT("type"), TEXT("string"));
			PropProp->SetStringField(TEXT("description"), TEXT("Case-sensitive C++ property name, e.g. bCanBeDamaged"));
			Properties->SetObjectField(TEXT("property"), PropProp);
			TSharedRef<FJsonObject> ValProp = MakeShared<FJsonObject>();
			ValProp->SetStringField(TEXT("type"), TEXT("string"));
			ValProp->SetStringField(TEXT("description"), TEXT("Value in UE ImportText format: True/False (bool), 42 (int), 3.14 (float), (X=1,Y=2,Z=3) (vector), /Game/Path.Asset (object ref)"));
			Properties->SetObjectField(TEXT("value"), ValProp);
			Def.InputSchema->SetObjectField(TEXT("properties"), Properties);
			TArray<TSharedPtr<FJsonValue>> Required;
			Required.Add(MakeShared<FJsonValueString>(TEXT("blueprint_path")));
			Required.Add(MakeShared<FJsonValueString>(TEXT("property")));
			Required.Add(MakeShared<FJsonValueString>(TEXT("value")));
			Def.InputSchema->SetArrayField(TEXT("required"), Required);
		}
		Def.Tier = EAgentMcpTier::SafeWrite;
		Def.Handler = FAgentMcpToolHandler::CreateStatic(&HandleSetCdoProperty);
		FAgentMcpToolRegistry::Get().Register(MoveTemp(Def));
	}
	{
		FAgentMcpToolDef Def;
		Def.Name = TEXT("reparent_blueprint");
		Def.Description = TEXT("Changes the parent class of a Blueprint. DANGEROUS for established blueprints: existing nodes/variables referencing the old parent may break - always compile_blueprint afterwards and read the messages. Args: blueprint_path (required), new_parent_class (required, short name like 'Pawn' or full path like '/Script/Engine.Pawn'). Returns {reparented, new_parent}.");
		Def.InputSchema = MakeShared<FJsonObject>();
		Def.InputSchema->SetStringField(TEXT("type"), TEXT("object"));
		{
			TSharedRef<FJsonObject> Properties = MakeShared<FJsonObject>();
			TSharedRef<FJsonObject> PathProp = MakeShared<FJsonObject>();
			PathProp->SetStringField(TEXT("type"), TEXT("string"));
			PathProp->SetStringField(TEXT("description"), TEXT("Path of the Blueprint to reparent, e.g. /Game/Dev/BP_MyActor"));
			Properties->SetObjectField(TEXT("blueprint_path"), PathProp);
			TSharedRef<FJsonObject> ClassProp = MakeShared<FJsonObject>();
			ClassProp->SetStringField(TEXT("type"), TEXT("string"));
			ClassProp->SetStringField(TEXT("description"), TEXT("New parent class: short name (Pawn, Character) or full path (/Script/Engine.Pawn). Must be Blueprintable."));
			Properties->SetObjectField(TEXT("new_parent_class"), ClassProp);
			Def.InputSchema->SetObjectField(TEXT("properties"), Properties);
			TArray<TSharedPtr<FJsonValue>> Required;
			Required.Add(MakeShared<FJsonValueString>(TEXT("blueprint_path")));
			Required.Add(MakeShared<FJsonValueString>(TEXT("new_parent_class")));
			Def.InputSchema->SetArrayField(TEXT("required"), Required);
		}
		Def.Tier = EAgentMcpTier::SafeWrite;
		Def.Handler = FAgentMcpToolHandler::CreateStatic(&HandleReparentBlueprint);
		FAgentMcpToolRegistry::Get().Register(MoveTemp(Def));
	}
}
