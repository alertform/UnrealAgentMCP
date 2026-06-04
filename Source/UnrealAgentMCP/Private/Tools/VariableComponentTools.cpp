#include "Tools/VariableComponentTools.h"

#include "BlueprintEditorLibrary.h"
#include "Core/AgentMcpToolRegistry.h"
#include "Core/McpTypes.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "EdGraph/EdGraphPin.h"
#include "Engine/Blueprint.h"
#include "Kismet2/BlueprintEditorUtils.h"
#include "ScopedTransaction.h"
#include "Tools/McpToolUtils.h"
#include "Tools/NodeGraphUtils.h"

namespace
{
	using namespace AgentMcp;

	/**
	 * Parses a variable type token into an FEdGraphPinType.
	 * Supports: bool, int, real, string, name, text, byte,
	 *           object:ClassName, class:ClassName.
	 */
	bool ParseVariableType(const FString& TypeToken, FEdGraphPinType& OutType, FString& OutError)
	{
		FString ObjectClassName;
		if (TypeToken.Split(TEXT(":"), nullptr, &ObjectClassName))
		{
			UClass* RefClass = UClass::TryFindTypeSlow<UClass>(ObjectClassName);
			if (!RefClass)
			{
				OutError = FString::Printf(TEXT("Class '%s' not found for reference type."), *ObjectClassName);
				return false;
			}
			OutType = TypeToken.StartsWith(TEXT("class:"))
				? UBlueprintEditorLibrary::GetClassReferenceType(RefClass)
				: UBlueprintEditorLibrary::GetObjectReferenceType(RefClass);
			return true;
		}
		// GetBasicTypeByName silently returns the int type for unknown tokens — pre-validate.
		static const TSet<FName> Supported = { "bool", "int", "real", "string", "name", "text", "byte" };
		if (!Supported.Contains(FName(*TypeToken)))
		{
			OutError = FString::Printf(TEXT("Unknown type '%s'. Supported: bool, int, real, string, name, text, byte, object:ClassName, class:ClassName."), *TypeToken);
			return false;
		}
		OutType = UBlueprintEditorLibrary::GetBasicTypeByName(FName(*TypeToken));
		return true;
	}

	/**
	 * Returns true if a variable named VarName already exists in Blueprint->NewVariables.
	 * Uses FBlueprintEditorUtils::FindNewVariableIndex (returns INDEX_NONE on miss).
	 */
	bool VariableExists(const UBlueprint* Blueprint, const FName& VarName)
	{
		return FBlueprintEditorUtils::FindNewVariableIndex(Blueprint, VarName) != INDEX_NONE;
	}

	FAgentMcpToolResult HandleAddVariable(const TSharedPtr<FJsonObject>& Args)
	{
		// ── validate required args ────────────────────────────────────────────────
		FString BlueprintPath;
		if (!Args.IsValid() || !Args->TryGetStringField(TEXT("blueprint_path"), BlueprintPath))
		{
			return FAgentMcpToolResult::Error(TEXT("Missing required string argument 'blueprint_path'."));
		}
		FString VariableName;
		if (!Args->TryGetStringField(TEXT("variable_name"), VariableName) || VariableName.IsEmpty())
		{
			return FAgentMcpToolResult::Error(TEXT("Missing required string argument 'variable_name'."));
		}
		FString TypeToken;
		if (!Args->TryGetStringField(TEXT("type"), TypeToken) || TypeToken.IsEmpty())
		{
			return FAgentMcpToolResult::Error(TEXT("Missing required string argument 'type' (e.g. bool, int, real, string, object:Actor)."));
		}

		FString Error;
		UBlueprint* Blueprint = NodeGraphUtils::ResolveBlueprint(BlueprintPath, Error);
		if (!Blueprint)
		{
			return FAgentMcpToolResult::Error(Error);
		}

		// ── type parse (before any mutation) ──────────────────────────────────────
		FEdGraphPinType PinType;
		if (!ParseVariableType(TypeToken, PinType, Error))
		{
			return FAgentMcpToolResult::Error(Error);
		}

		// ── duplicate name pre-check ───────────────────────────────────────────────
		const FName VarFName(*VariableName);
		if (VariableExists(Blueprint, VarFName))
		{
			return FAgentMcpToolResult::Error(FString::Printf(TEXT("Variable '%s' already exists in blueprint '%s'."), *VariableName, *Blueprint->GetName()));
		}

		// ── optional default value ────────────────────────────────────────────────
		FString DefaultValue;
		Args->TryGetStringField(TEXT("default_value"), DefaultValue);

		// ── transaction + mutation ────────────────────────────────────────────────
		FScopedTransaction Transaction(NSLOCTEXT("AgentMcp", "AddVariable", "MCP: Add Variable"));

		const bool bAdded = FBlueprintEditorUtils::AddMemberVariable(Blueprint, VarFName, PinType, DefaultValue);
		if (!bAdded)
		{
			Transaction.Cancel();
			return FAgentMcpToolResult::Error(FString::Printf(TEXT("FBlueprintEditorUtils::AddMemberVariable failed for variable '%s' (type may be invalid or blueprint locked)."), *VariableName));
		}

		FBlueprintEditorUtils::MarkBlueprintAsStructurallyModified(Blueprint);

		TSharedRef<FJsonObject> Result = MakeShared<FJsonObject>();
		Result->SetBoolField(TEXT("added"), true);
		Result->SetStringField(TEXT("variable"), VariableName);
		Result->SetStringField(TEXT("type"), TypeToken);
		return FAgentMcpToolResult::Success(ToolUtils::SerializeObject(Result));
	}

	FAgentMcpToolResult HandleSetVariableFlags(const TSharedPtr<FJsonObject>& Args)
	{
		// ── validate required args ────────────────────────────────────────────────
		FString BlueprintPath;
		if (!Args.IsValid() || !Args->TryGetStringField(TEXT("blueprint_path"), BlueprintPath))
		{
			return FAgentMcpToolResult::Error(TEXT("Missing required string argument 'blueprint_path'."));
		}
		FString VariableName;
		if (!Args->TryGetStringField(TEXT("variable_name"), VariableName) || VariableName.IsEmpty())
		{
			return FAgentMcpToolResult::Error(TEXT("Missing required string argument 'variable_name'."));
		}

		FString Error;
		UBlueprint* Blueprint = NodeGraphUtils::ResolveBlueprint(BlueprintPath, Error);
		if (!Blueprint)
		{
			return FAgentMcpToolResult::Error(Error);
		}

		// ── variable existence pre-check ──────────────────────────────────────────
		const FName VarFName(*VariableName);
		if (!VariableExists(Blueprint, VarFName))
		{
			return FAgentMcpToolResult::Error(FString::Printf(TEXT("Variable '%s' not found in blueprint '%s'. Call add_variable first."), *VariableName, *Blueprint->GetName()));
		}

		// ── read optional flag args ───────────────────────────────────────────────
		bool bInstanceEditable = false;
		bool bExposeOnSpawn = false;
		const bool bHasInstanceEditable = Args->TryGetBoolField(TEXT("instance_editable"), bInstanceEditable);
		const bool bHasExposeOnSpawn    = Args->TryGetBoolField(TEXT("expose_on_spawn"),   bExposeOnSpawn);

		if (!bHasInstanceEditable && !bHasExposeOnSpawn)
		{
			return FAgentMcpToolResult::Error(TEXT("set_variable_flags requires at least one of: instance_editable (bool), expose_on_spawn (bool)."));
		}

		// ── transaction + mutation ────────────────────────────────────────────────
		FScopedTransaction Transaction(NSLOCTEXT("AgentMcp", "SetVariableFlags", "MCP: Set Variable Flags"));

		if (bHasInstanceEditable)
		{
			UBlueprintEditorLibrary::SetBlueprintVariableInstanceEditable(Blueprint, VarFName, bInstanceEditable);
		}
		if (bHasExposeOnSpawn)
		{
			UBlueprintEditorLibrary::SetBlueprintVariableExposeOnSpawn(Blueprint, VarFName, bExposeOnSpawn);
		}

		FBlueprintEditorUtils::MarkBlueprintAsModified(Blueprint);

		TSharedRef<FJsonObject> Result = MakeShared<FJsonObject>();
		Result->SetBoolField(TEXT("set"), true);
		Result->SetStringField(TEXT("variable"), VariableName);
		return FAgentMcpToolResult::Success(ToolUtils::SerializeObject(Result));
	}

} // namespace

void AgentMcp::Tools::RegisterVariableComponentTools()
{
	// ── add_variable ──────────────────────────────────────────────────────────
	{
		FAgentMcpToolDef Def;
		Def.Name = TEXT("add_variable");
		Def.Description = TEXT("Adds a new member variable to a Blueprint. type supports: bool, int, real, string, name, text, byte, object:ClassName (soft object ref), class:ClassName (class ref). default_value uses UE ImportText syntax (e.g. 42.5, True, None). Wrapped in an undo transaction. Returns {added:true, variable, type}.");
		Def.InputSchema = MakeShared<FJsonObject>();
		Def.InputSchema->SetStringField(TEXT("type"), TEXT("object"));
		{
			TSharedRef<FJsonObject> Props = MakeShared<FJsonObject>();

			TSharedRef<FJsonObject> BpProp = MakeShared<FJsonObject>();
			BpProp->SetStringField(TEXT("type"), TEXT("string"));
			BpProp->SetStringField(TEXT("description"), TEXT("Absolute package path to the Blueprint asset, e.g. /Game/Blueprints/BP_MyActor."));
			Props->SetObjectField(TEXT("blueprint_path"), BpProp);

			TSharedRef<FJsonObject> NameProp = MakeShared<FJsonObject>();
			NameProp->SetStringField(TEXT("type"), TEXT("string"));
			NameProp->SetStringField(TEXT("description"), TEXT("C++ identifier-style name for the new variable. Must be unique within the Blueprint."));
			Props->SetObjectField(TEXT("variable_name"), NameProp);

			TSharedRef<FJsonObject> TypeProp = MakeShared<FJsonObject>();
			TypeProp->SetStringField(TEXT("type"), TEXT("string"));
			TypeProp->SetStringField(TEXT("description"), TEXT("Variable type token: bool, int, real, string, name, text, byte, object:ClassName (e.g. object:Actor), class:ClassName (e.g. class:Actor)."));
			Props->SetObjectField(TEXT("type"), TypeProp);

			TSharedRef<FJsonObject> DefaultProp = MakeShared<FJsonObject>();
			DefaultProp->SetStringField(TEXT("type"), TEXT("string"));
			DefaultProp->SetStringField(TEXT("description"), TEXT("Optional default value in UE ImportText syntax: True/False (bool), 42 (int), 3.14 (real), MyName (name), etc."));
			Props->SetObjectField(TEXT("default_value"), DefaultProp);

			Def.InputSchema->SetObjectField(TEXT("properties"), Props);

			TArray<TSharedPtr<FJsonValue>> Required;
			Required.Add(MakeShared<FJsonValueString>(TEXT("blueprint_path")));
			Required.Add(MakeShared<FJsonValueString>(TEXT("variable_name")));
			Required.Add(MakeShared<FJsonValueString>(TEXT("type")));
			Def.InputSchema->SetArrayField(TEXT("required"), Required);
		}
		Def.Tier = EAgentMcpTier::SafeWrite;
		Def.Handler = FAgentMcpToolHandler::CreateStatic(&HandleAddVariable);
		FAgentMcpToolRegistry::Get().Register(MoveTemp(Def));
	}

	// ── set_variable_flags ────────────────────────────────────────────────────
	{
		FAgentMcpToolDef Def;
		Def.Name = TEXT("set_variable_flags");
		Def.Description = TEXT("Sets instance_editable and/or expose_on_spawn flags on an existing Blueprint variable. Provide at least one flag. Wrapped in an undo transaction. Returns {set:true, variable}.");
		Def.InputSchema = MakeShared<FJsonObject>();
		Def.InputSchema->SetStringField(TEXT("type"), TEXT("object"));
		{
			TSharedRef<FJsonObject> Props = MakeShared<FJsonObject>();

			TSharedRef<FJsonObject> BpProp = MakeShared<FJsonObject>();
			BpProp->SetStringField(TEXT("type"), TEXT("string"));
			BpProp->SetStringField(TEXT("description"), TEXT("Absolute package path to the Blueprint asset."));
			Props->SetObjectField(TEXT("blueprint_path"), BpProp);

			TSharedRef<FJsonObject> NameProp = MakeShared<FJsonObject>();
			NameProp->SetStringField(TEXT("type"), TEXT("string"));
			NameProp->SetStringField(TEXT("description"), TEXT("Name of the variable to configure (must already exist — call add_variable first)."));
			Props->SetObjectField(TEXT("variable_name"), NameProp);

			TSharedRef<FJsonObject> IeProp = MakeShared<FJsonObject>();
			IeProp->SetStringField(TEXT("type"), TEXT("boolean"));
			IeProp->SetStringField(TEXT("description"), TEXT("When true, the variable is editable per-instance in the editor Details panel."));
			Props->SetObjectField(TEXT("instance_editable"), IeProp);

			TSharedRef<FJsonObject> EsProp = MakeShared<FJsonObject>();
			EsProp->SetStringField(TEXT("type"), TEXT("boolean"));
			EsProp->SetStringField(TEXT("description"), TEXT("When true, the variable is exposed as a spawn parameter on BeginPlay/construction."));
			Props->SetObjectField(TEXT("expose_on_spawn"), EsProp);

			Def.InputSchema->SetObjectField(TEXT("properties"), Props);

			TArray<TSharedPtr<FJsonValue>> Required;
			Required.Add(MakeShared<FJsonValueString>(TEXT("blueprint_path")));
			Required.Add(MakeShared<FJsonValueString>(TEXT("variable_name")));
			Def.InputSchema->SetArrayField(TEXT("required"), Required);
		}
		Def.Tier = EAgentMcpTier::SafeWrite;
		Def.Handler = FAgentMcpToolHandler::CreateStatic(&HandleSetVariableFlags);
		FAgentMcpToolRegistry::Get().Register(MoveTemp(Def));
	}
}
