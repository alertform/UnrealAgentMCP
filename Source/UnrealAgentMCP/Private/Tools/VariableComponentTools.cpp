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
#include "SubobjectDataSubsystem.h"
#include "SubobjectData.h"
#include "Tools/McpToolUtils.h"
#include "Tools/NodeGraphUtils.h"
#include "Tools/PropertyBridge.h"

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
			return FAgentMcpToolResult::Error(FString::Printf(TEXT("Variable '%s' is not a Blueprint-defined variable on '%s'. Only Blueprint-defined variables can have flags set (inherited C++ properties cannot); for a new variable call add_variable first."), *VariableName, *Blueprint->GetName()));
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
		// The UBlueprintEditorLibrary setters mutate Blueprint->NewVariables directly WITHOUT calling
		// Blueprint->Modify() — without this line the flag changes would not be undoable.
		Blueprint->Modify();

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

	// ── Subobject helpers ────────────────────────────────────────────────────

	/**
	 * Gathers all FSubobjectDataHandle for the Blueprint and finds the one
	 * whose variable name (FSubobjectData::GetVariableName) matches ComponentName.
	 * Returns an invalid handle if not found.
	 */
	FSubobjectDataHandle FindComponentHandleByName(
		USubobjectDataSubsystem* Subsystem,
		UBlueprint* Blueprint,
		const FName& ComponentName,
		TArray<FSubobjectDataHandle>& OutHandles)
	{
		Subsystem->K2_GatherSubobjectDataForBlueprint(Blueprint, OutHandles);
		for (const FSubobjectDataHandle& H : OutHandles)
		{
			FSubobjectData Data;
			if (Subsystem->K2_FindSubobjectDataFromHandle(H, Data))
			{
				if (Data.GetVariableName() == ComponentName)
				{
					return H;
				}
			}
		}
		return FSubobjectDataHandle::InvalidHandle;
	}

	// ── add_component ────────────────────────────────────────────────────────

	FAgentMcpToolResult HandleAddComponent(const TSharedPtr<FJsonObject>& Args)
	{
		// ── validate required args ────────────────────────────────────────────
		FString BlueprintPath;
		if (!Args.IsValid() || !Args->TryGetStringField(TEXT("blueprint_path"), BlueprintPath))
		{
			return FAgentMcpToolResult::Error(TEXT("Missing required string argument 'blueprint_path'."));
		}
		FString ComponentClassName;
		if (!Args->TryGetStringField(TEXT("component_class"), ComponentClassName) || ComponentClassName.IsEmpty())
		{
			return FAgentMcpToolResult::Error(TEXT("Missing required string argument 'component_class'."));
		}

		FString Error;
		UBlueprint* Blueprint = NodeGraphUtils::ResolveBlueprint(BlueprintPath, Error);
		if (!Blueprint)
		{
			return FAgentMcpToolResult::Error(Error);
		}

		// ── resolve component class ───────────────────────────────────────────
		UClass* ComponentClass = UClass::TryFindTypeSlow<UClass>(ComponentClassName);
		if (!ComponentClass || !ComponentClass->IsChildOf(UActorComponent::StaticClass()))
		{
			return FAgentMcpToolResult::Error(FString::Printf(
				TEXT("Component class '%s' not found or is not a UActorComponent subclass."), *ComponentClassName));
		}

		// ── optional desired name ─────────────────────────────────────────────
		FString DesiredName;
		Args->TryGetStringField(TEXT("component_name"), DesiredName);
		// Trim whitespace; treat whitespace-only as not-provided.
		DesiredName = DesiredName.TrimStartAndEnd();

		// ── acquire subsystem ────────────────────────────────────────────────
		USubobjectDataSubsystem* Subsystem = USubobjectDataSubsystem::Get();
		if (!Subsystem)
		{
			return FAgentMcpToolResult::Error(TEXT("USubobjectDataSubsystem unavailable."));
		}

		// ── gather existing handles to get root ──────────────────────────────
		TArray<FSubobjectDataHandle> Handles;
		Subsystem->K2_GatherSubobjectDataForBlueprint(Blueprint, Handles);
		if (Handles.Num() == 0)
		{
			return FAgentMcpToolResult::Error(TEXT("Blueprint has no subobject root — cannot add component."));
		}

		// ── transaction + AddNewSubobject ────────────────────────────────────
		FScopedTransaction Transaction(NSLOCTEXT("AgentMcp", "AddComponent", "MCP: Add Component"));

		FAddNewSubobjectParams Params;
		// Handles[0] is the root ACTOR node for actor blueprints (AddNewSubobject reroutes to the
		// real scene root internally via FindParentForNewSubobject). Non-actor blueprints would land
		// in the FailReason error path below.
		Params.ParentHandle     = Handles[0];
		Params.NewClass         = ComponentClass;
		Params.BlueprintContext = Blueprint;
		// Avoid a redundant MarkBlueprintAsStructurallyModified inside AddNewSubobject — we call it
		// explicitly below so that the transaction boundary is clear and consistent with other tools.
		Params.bSkipMarkBlueprintModified = true;

		FText FailReason;
		FSubobjectDataHandle NewHandle = Subsystem->AddNewSubobject(Params, FailReason);
		if (!NewHandle.IsValid())
		{
			Transaction.Cancel();
			return FAgentMcpToolResult::Error(FString::Printf(
				TEXT("AddNewSubobject failed: %s"), *FailReason.ToString()));
		}

		// ── optional rename ──────────────────────────────────────────────────
		if (!DesiredName.IsEmpty())
		{
			FText RenameError;
			if (!Subsystem->IsValidRename(NewHandle, FText::FromString(DesiredName), RenameError))
			{
				Transaction.Cancel();
				return FAgentMcpToolResult::Error(FString::Printf(
					TEXT("Cannot name component '%s': %s"), *DesiredName, *RenameError.ToString()));
			}
			USubobjectDataSubsystem::RenameSubobjectMemberVariable(
				Blueprint, NewHandle, FName(*DesiredName));
		}

		// Explicit structural mark (bSkipMarkBlueprintModified=true was set above).
		FBlueprintEditorUtils::MarkBlueprintAsStructurallyModified(Blueprint);

		// ── read back actual variable name ───────────────────────────────────
		FSubobjectData NewData;
		FString ActualName = DesiredName;
		if (Subsystem->K2_FindSubobjectDataFromHandle(NewHandle, NewData))
		{
			ActualName = NewData.GetVariableName().ToString();
		}

		TSharedRef<FJsonObject> Result = MakeShared<FJsonObject>();
		Result->SetBoolField(TEXT("added"), true);
		Result->SetStringField(TEXT("component_name"), ActualName);
		Result->SetStringField(TEXT("handle_note"), TEXT("component_name is the Blueprint variable name; use it in attach_component and set_component_property"));
		return FAgentMcpToolResult::Success(ToolUtils::SerializeObject(Result));
	}

	// ── attach_component ─────────────────────────────────────────────────────

	FAgentMcpToolResult HandleAttachComponent(const TSharedPtr<FJsonObject>& Args)
	{
		FString BlueprintPath;
		if (!Args.IsValid() || !Args->TryGetStringField(TEXT("blueprint_path"), BlueprintPath))
		{
			return FAgentMcpToolResult::Error(TEXT("Missing required string argument 'blueprint_path'."));
		}
		FString ChildName, ParentName;
		if (!Args->TryGetStringField(TEXT("child_name"), ChildName) || ChildName.IsEmpty())
		{
			return FAgentMcpToolResult::Error(TEXT("Missing required string argument 'child_name'."));
		}
		if (!Args->TryGetStringField(TEXT("parent_name"), ParentName) || ParentName.IsEmpty())
		{
			return FAgentMcpToolResult::Error(TEXT("Missing required string argument 'parent_name'."));
		}

		FString Error;
		UBlueprint* Blueprint = NodeGraphUtils::ResolveBlueprint(BlueprintPath, Error);
		if (!Blueprint)
		{
			return FAgentMcpToolResult::Error(Error);
		}

		USubobjectDataSubsystem* Subsystem = USubobjectDataSubsystem::Get();
		if (!Subsystem)
		{
			return FAgentMcpToolResult::Error(TEXT("USubobjectDataSubsystem unavailable."));
		}

		// Gather once, then scan the same array twice — avoids two K2_GatherSubobjectDataForBlueprint calls.
		TArray<FSubobjectDataHandle> Handles;
		Subsystem->K2_GatherSubobjectDataForBlueprint(Blueprint, Handles);
		FSubobjectDataHandle ChildHandle  = FSubobjectDataHandle::InvalidHandle;
		FSubobjectDataHandle ParentHandle = FSubobjectDataHandle::InvalidHandle;
		for (const FSubobjectDataHandle& H : Handles)
		{
			FSubobjectData Data;
			if (Subsystem->K2_FindSubobjectDataFromHandle(H, Data))
			{
				const FName VarName = Data.GetVariableName();
				if (VarName == FName(*ChildName))  { ChildHandle  = H; }
				if (VarName == FName(*ParentName)) { ParentHandle = H; }
			}
		}

		if (!ChildHandle.IsValid())
		{
			return FAgentMcpToolResult::Error(FString::Printf(
				TEXT("Component '%s' not found in Blueprint '%s'. Use the component_name returned by add_component."),
				*ChildName, *Blueprint->GetName()));
		}
		if (!ParentHandle.IsValid())
		{
			return FAgentMcpToolResult::Error(FString::Printf(
				TEXT("Parent component '%s' not found in Blueprint '%s'."),
				*ParentName, *Blueprint->GetName()));
		}

		FScopedTransaction Transaction(NSLOCTEXT("AgentMcp", "AttachComponent", "MCP: Attach Component"));

		if (!Subsystem->AttachSubobject(ParentHandle, ChildHandle))
		{
			Transaction.Cancel();
			return FAgentMcpToolResult::Error(FString::Printf(
				TEXT("AttachSubobject failed for child '%s' -> parent '%s'."), *ChildName, *ParentName));
		}

		FBlueprintEditorUtils::MarkBlueprintAsStructurallyModified(Blueprint);

		TSharedRef<FJsonObject> Result = MakeShared<FJsonObject>();
		Result->SetBoolField(TEXT("attached"), true);
		return FAgentMcpToolResult::Success(ToolUtils::SerializeObject(Result));
	}

	// ── set_component_property ───────────────────────────────────────────────

	FAgentMcpToolResult HandleSetComponentProperty(const TSharedPtr<FJsonObject>& Args)
	{
		FString BlueprintPath;
		if (!Args.IsValid() || !Args->TryGetStringField(TEXT("blueprint_path"), BlueprintPath))
		{
			return FAgentMcpToolResult::Error(TEXT("Missing required string argument 'blueprint_path'."));
		}
		FString ComponentName;
		if (!Args->TryGetStringField(TEXT("component_name"), ComponentName) || ComponentName.IsEmpty())
		{
			return FAgentMcpToolResult::Error(TEXT("Missing required string argument 'component_name'."));
		}
		FString PropertyName, Value;
		if (!Args->TryGetStringField(TEXT("property"), PropertyName) || PropertyName.IsEmpty())
		{
			return FAgentMcpToolResult::Error(TEXT("Missing required string argument 'property'."));
		}
		if (!Args->TryGetStringField(TEXT("value"), Value))
		{
			return FAgentMcpToolResult::Error(TEXT("Missing required string argument 'value'."));
		}

		FString Error;
		UBlueprint* Blueprint = NodeGraphUtils::ResolveBlueprint(BlueprintPath, Error);
		if (!Blueprint)
		{
			return FAgentMcpToolResult::Error(Error);
		}

		USubobjectDataSubsystem* Subsystem = USubobjectDataSubsystem::Get();
		if (!Subsystem)
		{
			return FAgentMcpToolResult::Error(TEXT("USubobjectDataSubsystem unavailable."));
		}

		TArray<FSubobjectDataHandle> Handles;
		FSubobjectDataHandle CompHandle = FindComponentHandleByName(
			Subsystem, Blueprint, FName(*ComponentName), Handles);
		if (!CompHandle.IsValid())
		{
			return FAgentMcpToolResult::Error(FString::Printf(
				TEXT("Component '%s' not found in Blueprint '%s'. Use the component_name returned by add_component."),
				*ComponentName, *Blueprint->GetName()));
		}

		// GetObjectForBlueprint returns the component TEMPLATE (archetype) for BP-owned components,
		// so mutations are inherited by all instances — exactly what we need for a Blueprint setter.
		// GetMutableObjectForBlueprint is private (subsystem-friend only), so we const_cast the
		// public const accessor; this matches how Blueprint editor panels mutate component templates.
		FSubobjectData CompData;
		if (!Subsystem->K2_FindSubobjectDataFromHandle(CompHandle, CompData))
		{
			return FAgentMcpToolResult::Error(TEXT("Failed to resolve subobject data from handle."));
		}

		// Native C++ components' "template" is the parent CDO's subobject — mutating it would leak
		// across every Blueprint and instance of that C++ class. Only Blueprint-owned (SCS) and
		// inherited-SCS components (which get per-BP override templates) are safely mutable here.
		if (CompData.IsNativeComponent())
		{
			return FAgentMcpToolResult::Error(FString::Printf(
				TEXT("Component '%s' is a native C++ component; its template lives on the C++ CDO and cannot be safely mutated via MCP. Override such defaults in C++ or via the Details panel."),
				*ComponentName));
		}

		UObject* Template = const_cast<UObject*>(CompData.GetObjectForBlueprint(Blueprint));
		if (!Template)
		{
			return FAgentMcpToolResult::Error(FString::Printf(
				TEXT("No component template found for '%s' in Blueprint context — component may be inherited (read-only)."),
				*ComponentName));
		}

		FScopedTransaction Transaction(NSLOCTEXT("AgentMcp", "SetComponentProperty", "MCP: Set Component Property"));
		Template->Modify();

		if (!PropertyBridge::SetPropertyFromString(Template, PropertyName, Value, Error, /*bRejectTemplateDisabled=*/true))
		{
			Transaction.Cancel();
			return FAgentMcpToolResult::Error(Error);
		}

		FBlueprintEditorUtils::MarkBlueprintAsModified(Blueprint);

		FString ReadBack, Type;
		PropertyBridge::GetPropertyAsString(Template, PropertyName, ReadBack, Type, Error);

		TSharedRef<FJsonObject> Result = MakeShared<FJsonObject>();
		Result->SetBoolField(TEXT("set"), true);
		Result->SetStringField(TEXT("property"), PropertyName);
		Result->SetStringField(TEXT("value"), ReadBack);
		return FAgentMcpToolResult::Success(ToolUtils::SerializeObject(Result));
	}

} // namespace

void AgentMcp::Tools::RegisterVariableComponentTools()
{
	// ── add_variable ──────────────────────────────────────────────────────────
	{
		FAgentMcpToolDef Def;
		Def.Name = TEXT("add_variable");
		Def.Description = TEXT("Adds a new member variable to a Blueprint. type supports: bool, int, real, string, name, text, byte, object:ClassName (soft object ref), class:ClassName (class ref). default_value uses UE ImportText syntax (e.g. 42.5, True, None) - not applied for object:/class: reference types. Wrapped in an undo transaction. Returns {added:true, variable, type}.");
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

	// ── add_component ─────────────────────────────────────────────────────────
	{
		FAgentMcpToolDef Def;
		Def.Name = TEXT("add_component");
		Def.Description = TEXT("Adds a new component of the given class to a Blueprint via USubobjectDataSubsystem. component_name sets the Blueprint variable name (if omitted the engine generates one). Returns {added:true, component_name, handle_note}. Use the returned component_name in attach_component and set_component_property.");
		Def.InputSchema = MakeShared<FJsonObject>();
		Def.InputSchema->SetStringField(TEXT("type"), TEXT("object"));
		{
			TSharedRef<FJsonObject> Props = MakeShared<FJsonObject>();

			TSharedRef<FJsonObject> BpProp = MakeShared<FJsonObject>();
			BpProp->SetStringField(TEXT("type"), TEXT("string"));
			BpProp->SetStringField(TEXT("description"), TEXT("Absolute package path to the Blueprint asset."));
			Props->SetObjectField(TEXT("blueprint_path"), BpProp);

			TSharedRef<FJsonObject> ClsProp = MakeShared<FJsonObject>();
			ClsProp->SetStringField(TEXT("type"), TEXT("string"));
			ClsProp->SetStringField(TEXT("description"), TEXT("Short class name of the component to add, e.g. SceneComponent, StaticMeshComponent, PointLightComponent. Must be a UActorComponent subclass."));
			Props->SetObjectField(TEXT("component_class"), ClsProp);

			TSharedRef<FJsonObject> NameProp = MakeShared<FJsonObject>();
			NameProp->SetStringField(TEXT("type"), TEXT("string"));
			NameProp->SetStringField(TEXT("description"), TEXT("Optional desired Blueprint variable name for the new component. If omitted the engine auto-generates one."));
			Props->SetObjectField(TEXT("component_name"), NameProp);

			Def.InputSchema->SetObjectField(TEXT("properties"), Props);

			TArray<TSharedPtr<FJsonValue>> Required;
			Required.Add(MakeShared<FJsonValueString>(TEXT("blueprint_path")));
			Required.Add(MakeShared<FJsonValueString>(TEXT("component_class")));
			Def.InputSchema->SetArrayField(TEXT("required"), Required);
		}
		Def.Tier = EAgentMcpTier::SafeWrite;
		Def.Handler = FAgentMcpToolHandler::CreateStatic(&HandleAddComponent);
		FAgentMcpToolRegistry::Get().Register(MoveTemp(Def));
	}

	// ── attach_component ──────────────────────────────────────────────────────
	{
		FAgentMcpToolDef Def;
		Def.Name = TEXT("attach_component");
		Def.Description = TEXT("Attaches child_name component under parent_name component in the Blueprint's component hierarchy. Both names are Blueprint variable names as returned by add_component. Wrapped in an undo transaction. Returns {attached:true}.");
		Def.InputSchema = MakeShared<FJsonObject>();
		Def.InputSchema->SetStringField(TEXT("type"), TEXT("object"));
		{
			TSharedRef<FJsonObject> Props = MakeShared<FJsonObject>();

			TSharedRef<FJsonObject> BpProp = MakeShared<FJsonObject>();
			BpProp->SetStringField(TEXT("type"), TEXT("string"));
			BpProp->SetStringField(TEXT("description"), TEXT("Absolute package path to the Blueprint asset."));
			Props->SetObjectField(TEXT("blueprint_path"), BpProp);

			TSharedRef<FJsonObject> ChildProp = MakeShared<FJsonObject>();
			ChildProp->SetStringField(TEXT("type"), TEXT("string"));
			ChildProp->SetStringField(TEXT("description"), TEXT("Blueprint variable name of the component to move (the child). From add_component's component_name."));
			Props->SetObjectField(TEXT("child_name"), ChildProp);

			TSharedRef<FJsonObject> ParentProp = MakeShared<FJsonObject>();
			ParentProp->SetStringField(TEXT("type"), TEXT("string"));
			ParentProp->SetStringField(TEXT("description"), TEXT("Blueprint variable name of the component to attach under (the new parent). From add_component's component_name."));
			Props->SetObjectField(TEXT("parent_name"), ParentProp);

			Def.InputSchema->SetObjectField(TEXT("properties"), Props);

			TArray<TSharedPtr<FJsonValue>> Required;
			Required.Add(MakeShared<FJsonValueString>(TEXT("blueprint_path")));
			Required.Add(MakeShared<FJsonValueString>(TEXT("child_name")));
			Required.Add(MakeShared<FJsonValueString>(TEXT("parent_name")));
			Def.InputSchema->SetArrayField(TEXT("required"), Required);
		}
		Def.Tier = EAgentMcpTier::SafeWrite;
		Def.Handler = FAgentMcpToolHandler::CreateStatic(&HandleAttachComponent);
		FAgentMcpToolRegistry::Get().Register(MoveTemp(Def));
	}

	// ── set_component_property ────────────────────────────────────────────────
	{
		FAgentMcpToolDef Def;
		Def.Name = TEXT("set_component_property");
		Def.Description = TEXT("Sets a property on a Blueprint component's template (archetype) so all instances inherit the value. component_name is the Blueprint variable name from add_component. value uses UE ImportText syntax. Only EditAnywhere/EditDefaultsOnly properties are writable. Wrapped in an undo transaction. Returns {set:true, property, value(readback)}.");
		Def.InputSchema = MakeShared<FJsonObject>();
		Def.InputSchema->SetStringField(TEXT("type"), TEXT("object"));
		{
			TSharedRef<FJsonObject> Props = MakeShared<FJsonObject>();

			TSharedRef<FJsonObject> BpProp = MakeShared<FJsonObject>();
			BpProp->SetStringField(TEXT("type"), TEXT("string"));
			BpProp->SetStringField(TEXT("description"), TEXT("Absolute package path to the Blueprint asset."));
			Props->SetObjectField(TEXT("blueprint_path"), BpProp);

			TSharedRef<FJsonObject> CompProp = MakeShared<FJsonObject>();
			CompProp->SetStringField(TEXT("type"), TEXT("string"));
			CompProp->SetStringField(TEXT("description"), TEXT("Blueprint variable name of the component whose template property should be set. From add_component's component_name."));
			Props->SetObjectField(TEXT("component_name"), CompProp);

			TSharedRef<FJsonObject> PropProp = MakeShared<FJsonObject>();
			PropProp->SetStringField(TEXT("type"), TEXT("string"));
			PropProp->SetStringField(TEXT("description"), TEXT("C++ property name on the component class, e.g. Intensity, CastShadows, RelativeLocation."));
			Props->SetObjectField(TEXT("property"), PropProp);

			TSharedRef<FJsonObject> ValProp = MakeShared<FJsonObject>();
			ValProp->SetStringField(TEXT("type"), TEXT("string"));
			ValProp->SetStringField(TEXT("description"), TEXT("New value in UE ImportText syntax: True/False (bool), 42 (int), 1234.0 (float), (X=0,Y=0,Z=0) (vector). Only EditAnywhere/EditDefaultsOnly properties accepted."));
			Props->SetObjectField(TEXT("value"), ValProp);

			Def.InputSchema->SetObjectField(TEXT("properties"), Props);

			TArray<TSharedPtr<FJsonValue>> Required;
			Required.Add(MakeShared<FJsonValueString>(TEXT("blueprint_path")));
			Required.Add(MakeShared<FJsonValueString>(TEXT("component_name")));
			Required.Add(MakeShared<FJsonValueString>(TEXT("property")));
			Required.Add(MakeShared<FJsonValueString>(TEXT("value")));
			Def.InputSchema->SetArrayField(TEXT("required"), Required);
		}
		Def.Tier = EAgentMcpTier::SafeWrite;
		Def.Handler = FAgentMcpToolHandler::CreateStatic(&HandleSetComponentProperty);
		FAgentMcpToolRegistry::Get().Register(MoveTemp(Def));
	}
}
