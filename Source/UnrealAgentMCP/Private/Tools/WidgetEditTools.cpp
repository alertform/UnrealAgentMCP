#include "Tools/WidgetTools.h"

#include "Animation/WidgetAnimation.h"
#include "Blueprint/WidgetTree.h"
#include "Components/Widget.h"
#include "Core/AgentMcpToolRegistry.h"
#include "Core/McpTypes.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "EdGraph/EdGraph.h"
#include "Editor.h"
#include "K2Node_ComponentBoundEvent.h"
#include "Kismet2/BlueprintEditorUtils.h"
#include "Kismet2/KismetEditorUtilities.h"
#include "MovieScene.h"
#include "MVVMBlueprintView.h"
#include "MVVMBlueprintViewBinding.h"
#include "MVVMEditorSubsystem.h"
#include "MVVMPropertyPath.h"
#include "ScopedTransaction.h"
#include "Tools/McpToolUtils.h"
#include "Tools/NodeGraphUtils.h"
#include "Tools/WidgetToolsShared.h"
#include "WidgetBlueprint.h"

namespace
{
	using namespace AgentMcp;
	using namespace AgentMcp::WidgetShared;

	// ─────────────────────────────────────────────────────────────────────
	// add_component_event handler
	// ─────────────────────────────────────────────────────────────────────

	FAgentMcpToolResult HandleAddComponentEvent(const TSharedPtr<FJsonObject>& Args)
	{
		if (!Args.IsValid())
		{
			return FAgentMcpToolResult::Error(TEXT("Missing arguments."));
		}

		FString BlueprintPath;
		if (!Args->TryGetStringField(TEXT("blueprint_path"), BlueprintPath))
		{
			return FAgentMcpToolResult::Error(TEXT("Missing required string argument 'blueprint_path'."));
		}
		FString ComponentName;
		if (!Args->TryGetStringField(TEXT("component_name"), ComponentName) || ComponentName.IsEmpty())
		{
			return FAgentMcpToolResult::Error(TEXT("Missing required string argument 'component_name'."));
		}
		FString EventName;
		if (!Args->TryGetStringField(TEXT("event_name"), EventName) || EventName.IsEmpty())
		{
			return FAgentMcpToolResult::Error(TEXT("Missing required string argument 'event_name'."));
		}

		// Optional position / graph args.
		int32 PosX = 0, PosY = 0;
		double Number = 0.0;
		if (Args->TryGetNumberField(TEXT("pos_x"), Number)) { PosX = static_cast<int32>(Number); }
		if (Args->TryGetNumberField(TEXT("pos_y"), Number)) { PosY = static_cast<int32>(Number); }
		FString GraphName;
		Args->TryGetStringField(TEXT("graph_name"), GraphName);

		// ── 1. Resolve Blueprint ─────────────────────────────────────────────
		FString Error;
		UBlueprint* BP = NodeGraphUtils::ResolveBlueprint(BlueprintPath, Error);
		if (!BP)
		{
			return FAgentMcpToolResult::Error(Error);
		}

		// ── 2. Resolve owner class for the delegate property ─────────────────
		//
		// For WidgetBlueprints: the component is a widget in the WidgetTree.
		// For Actor blueprints: the component is an SCS template.
		//
		// OwnerClass is the class that DECLARES the delegate property (e.g. UButton for OnClicked).
		UClass* OwnerClass = nullptr;

		if (UWidgetBlueprint* WBP = Cast<UWidgetBlueprint>(BP))
		{
			UWidgetTree* Tree = WBP->WidgetTree;
			if (!Tree)
			{
				return FAgentMcpToolResult::Error(TEXT("WidgetBlueprint has no WidgetTree."));
			}
			UWidget* Widget = Tree->FindWidget(FName(*ComponentName));
			if (!Widget)
			{
				return FAgentMcpToolResult::Error(FString::Printf(
					TEXT("Widget '%s' not found in tree. Use list_widgets to enumerate available widgets."),
					*ComponentName));
			}
			// Ensure the widget is exposed as a Blueprint variable so the skeleton class gets
			// an FObjectProperty for it — required for the bound-event node's CompProp lookup.
			// Intentional: committed as its own transaction; bIsVariable=true is safe (and usually
			// desired) to keep even if the node spawn below fails — no rollback on downstream error.
			if (!Widget->bIsVariable)
			{
				FScopedTransaction Transaction(NSLOCTEXT("AgentMcp", "MakeWidgetVariable", "MCP: Make Widget Variable"));
				Widget->Modify();
				Widget->bIsVariable = true;
				FBlueprintEditorUtils::MarkBlueprintAsStructurallyModified(BP);
			}
			OwnerClass = Widget->GetClass();
		}
		else
		{
			// Actor Blueprint: find the SCS component template by variable name.
			// The template's class is what owns the delegate.
			UClass* SkelClass = BP->SkeletonGeneratedClass;
			if (!SkelClass)
			{
				return FAgentMcpToolResult::Error(TEXT("Blueprint has no SkeletonGeneratedClass."));
			}
			FObjectProperty* CompPropCheck = FindFProperty<FObjectProperty>(SkelClass, FName(*ComponentName));
			if (!CompPropCheck)
			{
				return FAgentMcpToolResult::Error(FString::Printf(
					TEXT("Component variable '%s' not found on blueprint class '%s'."),
					*ComponentName, *BP->GetName()));
			}
			OwnerClass = CompPropCheck->PropertyClass;
		}

		if (!OwnerClass)
		{
			return FAgentMcpToolResult::Error(FString::Printf(
				TEXT("Could not determine owner class for component '%s'."), *ComponentName));
		}

		// ── 3. Find the delegate property on the owner class ─────────────────
		FMulticastDelegateProperty* DelegateProp =
			FindFProperty<FMulticastDelegateProperty>(OwnerClass, FName(*EventName));
		if (!DelegateProp)
		{
			return FAgentMcpToolResult::Error(FString::Printf(
				TEXT("Class '%s' has no multicast delegate '%s'. Button examples: OnClicked, OnPressed, OnReleased, OnHovered."),
				*OwnerClass->GetName(), *EventName));
		}

		// ── 4. Dedup: one bound event per (component, delegate) ──────────────
		const UK2Node_ComponentBoundEvent* Existing =
			FKismetEditorUtilities::FindBoundEventForComponent(
				BP, FName(*EventName), FName(*ComponentName));
		if (Existing)
		{
			// Return the existing node without modification.
			TSharedRef<FJsonObject> Result = NodeGraphUtils::NodeToJson(Existing);
			Result->SetStringField(TEXT("node_id"), Existing->NodeGuid.ToString());
			Result->SetBoolField(TEXT("existing"), true);
			return FAgentMcpToolResult::Success(ToolUtils::SerializeObject(Result));
		}

		// ── 5. Resolve FObjectProperty on SkeletonGeneratedClass for CompProp ─
		UClass* SkelClass = BP->SkeletonGeneratedClass;
		FObjectProperty* CompProp = SkelClass
			? FindFProperty<FObjectProperty>(SkelClass, FName(*ComponentName))
			: nullptr;

		if (!CompProp)
		{
			// Trigger a skeleton compile (no GC) and retry once.
			FKismetEditorUtilities::CompileBlueprint(BP, EBlueprintCompileOptions::SkipGarbageCollection);
			SkelClass = BP->SkeletonGeneratedClass;
			CompProp = SkelClass
				? FindFProperty<FObjectProperty>(SkelClass, FName(*ComponentName))
				: nullptr;
		}

		if (!CompProp)
		{
			return FAgentMcpToolResult::Error(FString::Printf(
				TEXT("'%s' is not exposed as a Blueprint variable even after recompile. "
					"Ensure the widget has bIsVariable=true (add_widget sets it by default)."),
				*ComponentName));
		}

		// ── 6. Resolve target graph ───────────────────────────────────────────
		UEdGraph* Graph = NodeGraphUtils::ResolveGraph(BP, GraphName, Error);
		if (!Graph)
		{
			return FAgentMcpToolResult::Error(Error);
		}

		// ── 7. Spawn the node ─────────────────────────────────────────────────
		FScopedTransaction Transaction(NSLOCTEXT("AgentMcp", "AddComponentEvent", "MCP: Add Component Event"));
		Graph->Modify();

		FGraphNodeCreator<UK2Node_ComponentBoundEvent> Creator(*Graph);
		UK2Node_ComponentBoundEvent* Node = Creator.CreateNode(/*bSelectNewNode=*/false);
		// InitializeComponentBoundEventParams MUST be called before Finalize (it calls GetBlueprint()).
		Node->InitializeComponentBoundEventParams(CompProp, DelegateProp);
		Node->NodePosX = PosX;
		Node->NodePosY = PosY;
		Creator.Finalize();

		FBlueprintEditorUtils::MarkBlueprintAsStructurallyModified(BP);

		TSharedRef<FJsonObject> Result = NodeGraphUtils::NodeToJson(Node);
		Result->SetStringField(TEXT("node_id"), Node->NodeGuid.ToString());
		Result->SetBoolField(TEXT("existing"), false);
		return FAgentMcpToolResult::Success(ToolUtils::SerializeObject(Result));
	}

	// ─────────────────────────────────────────────────────────────────────
	// rename_widget handler
	// ─────────────────────────────────────────────────────────────────────

	/**
	 * Validates a proposed widget name: [A-Za-z0-9_], non-empty, <=100 chars.
	 * Returns true when valid. SlugStringForValidName is not exported — manual
	 * validation is used (same char-set as the engine uses for FNames that become
	 * BP variable names).
	 */
	static bool IsValidWidgetName(const FString& Name, FString& OutError)
	{
		if (Name.IsEmpty())
		{
			OutError = TEXT("new_name must not be empty.");
			return false;
		}
		if (Name.Len() > 100)
		{
			OutError = TEXT("new_name exceeds 100 characters.");
			return false;
		}
		for (TCHAR Ch : Name)
		{
			if (!FChar::IsAlnum(Ch) && Ch != TEXT('_'))
			{
				OutError = FString::Printf(
					TEXT("new_name '%s' contains invalid character '%c'. Allowed: [A-Za-z0-9_]."),
					*Name, Ch);
				return false;
			}
		}
		return true;
	}

	FAgentMcpToolResult HandleRenameWidget(const TSharedPtr<FJsonObject>& Args)
	{
		if (!Args.IsValid())
		{
			return FAgentMcpToolResult::Error(TEXT("Missing arguments."));
		}

		FString BlueprintPath;
		if (!Args->TryGetStringField(TEXT("blueprint_path"), BlueprintPath))
		{
			return FAgentMcpToolResult::Error(TEXT("Missing required string argument 'blueprint_path'."));
		}
		FString OldName;
		if (!Args->TryGetStringField(TEXT("widget_name"), OldName) || OldName.IsEmpty())
		{
			return FAgentMcpToolResult::Error(TEXT("Missing required string argument 'widget_name'."));
		}
		FString NewName;
		if (!Args->TryGetStringField(TEXT("new_name"), NewName) || NewName.IsEmpty())
		{
			return FAgentMcpToolResult::Error(TEXT("Missing required string argument 'new_name'."));
		}

		// ── Validate new_name charset ────────────────────────────────────────
		FString ValidationError;
		if (!IsValidWidgetName(NewName, ValidationError))
		{
			return FAgentMcpToolResult::Error(ValidationError);
		}

		// ── Resolve blueprint ────────────────────────────────────────────────
		FString Error;
		UWidgetBlueprint* WBP = ResolveWidgetBlueprint(BlueprintPath, Error);
		if (!WBP)
		{
			return FAgentMcpToolResult::Error(Error);
		}

		UWidgetTree* Tree = WBP->WidgetTree;
		if (!Tree)
		{
			return FAgentMcpToolResult::Error(TEXT("WidgetBlueprint has no WidgetTree."));
		}

		// ── Find old widget ───────────────────────────────────────────────────
		const FName OldFName(*OldName);
		UWidget* Widget = Tree->FindWidget(OldFName);
		if (!Widget)
		{
			return FAgentMcpToolResult::Error(FString::Printf(
				TEXT("Widget '%s' not found. Use list_widgets to enumerate available widgets."),
				*OldName));
		}

		// ── Collision checks ─────────────────────────────────────────────────
		const FName NewFName(*NewName);

		// Check via WidgetTree (widgets).
		UWidget* Existing = Tree->FindWidget(NewFName);
		if (Existing && Existing != Widget)
		{
			return FAgentMcpToolResult::Error(FString::Printf(
				TEXT("A widget named '%s' already exists in the tree. Use list_widgets to see names."),
				*NewName));
		}
		// Self-collision: renaming a widget to its own name is a no-op collision.
		if (Existing == Widget)
		{
			return FAgentMcpToolResult::Error(FString::Printf(
				TEXT("Widget '%s' already has that name."),
				*NewName));
		}
		// Also check non-widget subobjects (slots share the same outer).
		UObject* SubObj = StaticFindObject(UObject::StaticClass(), Tree, *NewName);
		if (SubObj && SubObj != Widget)
		{
			return FAgentMcpToolResult::Error(FString::Printf(
				TEXT("Name '%s' is already taken by another subobject."),
				*NewName));
		}

		// ── MVVM binding scan (before transaction) ───────────────────────────
		// Collect ids of bindings whose DestinationPath.GetWidgetName() == OldFName.
		// Skip bindings that have any conversion object — rewiring conversion pins is out of scope.
		TArray<FGuid> BindingsToSync;
		TArray<FGuid> BindingsSkipped;

		UMVVMEditorSubsystem* MvvmSub = GEditor ? GEditor->GetEditorSubsystem<UMVVMEditorSubsystem>() : nullptr;
		UMVVMBlueprintView* View = MvvmSub ? MvvmSub->GetView(WBP) : nullptr;
		if (View)
		{
			for (const FMVVMBlueprintViewBinding& Binding : View->GetBindings())
			{
				if (Binding.DestinationPath.GetWidgetName() == OldFName)
				{
					const bool bHasConversion =
						(Binding.Conversion.SourceToDestinationConversion != nullptr) ||
						(Binding.Conversion.DestinationToSourceConversion != nullptr);
					if (bHasConversion)
					{
						BindingsSkipped.Add(Binding.BindingId);
					}
					else
					{
						BindingsToSync.Add(Binding.BindingId);
					}
				}
			}
		}

		// ── Transaction: rename widget THEN sync MVVM paths ──────────────────
		// NOTE: Renaming AWAY from a BindWidget-matched name (e.g. HostButton) will
		// silently unbind the C++ UPROPERTY pointer at next compile. The compiler warning
		// will surface this — this rename is intentional.
		FScopedTransaction Transaction(NSLOCTEXT("AgentMcp", "RenameWidget", "MCP: Rename Widget"));
		WBP->Modify();
		Widget->Modify();

		const FString NewNameStr = NewName;
		Widget->SetDisplayLabel(NewNameStr);
		if (!Widget->Rename(*NewNameStr))
		{
			// UObject-level failure the pre-checks missed (reserved/auto-suffixed names).
			// Cancel so the SetDisplayLabel above rolls back too (T3 review finding).
			Transaction.Cancel();
			return FAgentMcpToolResult::Error(FString::Printf(
				TEXT("UObject::Rename failed for widget '%s' -> '%s'. The name may be reserved."),
				*OldName, *NewName));
		}

		// Update FDelegateEditorBinding references (property bindings in the editor).
		for (FDelegateEditorBinding& Binding : WBP->Bindings)
		{
			if (Binding.ObjectName == OldName)
			{
				Binding.ObjectName = NewNameStr;
			}
		}

		// Update widget animation bindings — including the MovieScene possessable display
		// name, mirroring FWidgetBlueprintEditorUtils::RenameWidget (T3 review finding:
		// without Modify+SetName the Sequencer track header keeps the stale name and the
		// possessable rename is missing from the undo record).
		for (UWidgetAnimation* Anim : WBP->Animations)
		{
			if (!Anim)
			{
				continue;
			}
			for (FWidgetAnimationBinding& AnimBinding : Anim->AnimationBindings)
			{
				if (AnimBinding.WidgetName == OldFName)
				{
					AnimBinding.WidgetName = NewFName;
					if (Anim->MovieScene)
					{
						Anim->MovieScene->Modify();
						if (AnimBinding.SlotWidgetName == NAME_None)
						{
							if (FMovieScenePossessable* Possessable = Anim->MovieScene->FindPossessable(AnimBinding.AnimationGuid))
							{
								Possessable->SetName(NewFName.ToString());
							}
						}
					}
				}
			}
		}

		// Update navigation bindings.
		Tree->ForEachWidget([OldFName, NewFName](UWidget* W)
		{
			if (W && W->Navigation)
			{
				W->Navigation->SetFlags(RF_Transactional);
				W->Navigation->Modify();
				W->Navigation->TryToRenameBinding(OldFName, NewFName);
			}
		});

		// Update variable references in all graphs.
		FBlueprintEditorUtils::ReplaceVariableReferences(WBP, OldFName, NewFName);

		// Mark structural modification.
		FBlueprintEditorUtils::MarkBlueprintAsStructurallyModified(WBP);

		// ── MVVM path sync (still inside the transaction) ────────────────────
		int32 BindingsUpdated = 0;
		if (MvvmSub && View && BindingsToSync.Num() > 0)
		{
			for (const FGuid& Id : BindingsToSync)
			{
				// Re-fetch after potential TArray reallocation from MarkBlueprintAsStructurallyModified.
				FMVVMBlueprintViewBinding* BindingPtr = View->GetBinding(Id);
				if (!BindingPtr)
				{
					continue;
				}
				// Copy the existing path, swap the widget name, write back via subsystem.
				FMVVMBlueprintPropertyPath NewPath = BindingPtr->DestinationPath;
				NewPath.SetWidgetName(NewFName);
				MvvmSub->SetDestinationPathForBinding(WBP, *BindingPtr, NewPath, /*bAllowEventConversion=*/false);
				++BindingsUpdated;
			}
		}

		// ── Build result ──────────────────────────────────────────────────────
		TSharedRef<FJsonObject> Result = MakeShared<FJsonObject>();
		Result->SetBoolField(TEXT("renamed"), true);
		Result->SetStringField(TEXT("name"), NewName);
		Result->SetNumberField(TEXT("mvvm_bindings_updated"), BindingsUpdated);
		if (BindingsSkipped.Num() > 0)
		{
			TArray<TSharedPtr<FJsonValue>> SkippedArr;
			for (const FGuid& Id : BindingsSkipped)
			{
				SkippedArr.Add(MakeShared<FJsonValueString>(Id.ToString()));
			}
			Result->SetArrayField(TEXT("mvvm_bindings_skipped_conversions"), SkippedArr);
		}
		return FAgentMcpToolResult::Success(ToolUtils::SerializeObject(Result));
	}

} // anonymous namespace

// ─────────────────────────────────────────────────────────────────────────────
// Registration
// ─────────────────────────────────────────────────────────────────────────────

void AgentMcp::Tools::RegisterWidgetEditTools()
{
	// add_component_event
	{
		FAgentMcpToolDef Def;
		Def.Name = TEXT("add_component_event");
		Def.Description = TEXT(
			"Creates a component-bound event node (e.g. a Button's OnClicked) in a Blueprint's event graph. "
			"For WidgetBlueprints, component_name is the widget variable name (use list_widgets). "
			"For Actor Blueprints, component_name is the SCS component variable name. "
			"event_name is the multicast delegate property on the component class (e.g. OnClicked, OnPressed, OnReleased, OnHovered). "
			"Deduplicates: if a bound event already exists for that (component, delegate) pair it is returned with existing:true. "
			"Args: blueprint_path (req), component_name (req), event_name (req), graph_name (opt, default EventGraph), pos_x/pos_y (opt). "
			"Returns {node_id, class, title, pins:[...], existing}.");
		Def.InputSchema = MakeShared<FJsonObject>();
		Def.InputSchema->SetStringField(TEXT("type"), TEXT("object"));
		{
			TSharedRef<FJsonObject> Properties = MakeShared<FJsonObject>();

			TSharedRef<FJsonObject> BpPath = MakeShared<FJsonObject>();
			BpPath->SetStringField(TEXT("type"), TEXT("string"));
			BpPath->SetStringField(TEXT("description"), TEXT("Absolute asset path to the Blueprint, e.g. /Game/UI/WBP_MainMenu"));
			Properties->SetObjectField(TEXT("blueprint_path"), BpPath);

			TSharedRef<FJsonObject> CompName = MakeShared<FJsonObject>();
			CompName->SetStringField(TEXT("type"), TEXT("string"));
			CompName->SetStringField(TEXT("description"), TEXT("Variable name of the widget or component to bind to (use list_widgets for widget BPs, add_component name for actor BPs)."));
			Properties->SetObjectField(TEXT("component_name"), CompName);

			TSharedRef<FJsonObject> EvName = MakeShared<FJsonObject>();
			EvName->SetStringField(TEXT("type"), TEXT("string"));
			EvName->SetStringField(TEXT("description"), TEXT("Multicast delegate property name on the component class, e.g. OnClicked, OnPressed, OnReleased, OnHovered."));
			Properties->SetObjectField(TEXT("event_name"), EvName);

			TSharedRef<FJsonObject> GraphNameProp = MakeShared<FJsonObject>();
			GraphNameProp->SetStringField(TEXT("type"), TEXT("string"));
			GraphNameProp->SetStringField(TEXT("description"), TEXT("Graph name; defaults to the event graph."));
			Properties->SetObjectField(TEXT("graph_name"), GraphNameProp);

			TSharedRef<FJsonObject> PosX = MakeShared<FJsonObject>();
			PosX->SetStringField(TEXT("type"), TEXT("integer"));
			PosX->SetStringField(TEXT("description"), TEXT("Horizontal position of the new node in the graph (default 0)."));
			Properties->SetObjectField(TEXT("pos_x"), PosX);

			TSharedRef<FJsonObject> PosY = MakeShared<FJsonObject>();
			PosY->SetStringField(TEXT("type"), TEXT("integer"));
			PosY->SetStringField(TEXT("description"), TEXT("Vertical position of the new node in the graph (default 0)."));
			Properties->SetObjectField(TEXT("pos_y"), PosY);

			Def.InputSchema->SetObjectField(TEXT("properties"), Properties);

			TArray<TSharedPtr<FJsonValue>> Required;
			Required.Add(MakeShared<FJsonValueString>(TEXT("blueprint_path")));
			Required.Add(MakeShared<FJsonValueString>(TEXT("component_name")));
			Required.Add(MakeShared<FJsonValueString>(TEXT("event_name")));
			Def.InputSchema->SetArrayField(TEXT("required"), Required);
		}
		Def.Tier = EAgentMcpTier::SafeWrite;
		Def.Handler = FAgentMcpToolHandler::CreateStatic(&HandleAddComponentEvent);
		FAgentMcpToolRegistry::Get().Register(MoveTemp(Def));
	}

	// rename_widget
	{
		FAgentMcpToolDef Def;
		Def.Name = TEXT("rename_widget");
		Def.Description = TEXT(
			"Renames a widget inside a WidgetBlueprint's tree and syncs MVVM binding destination paths. "
			"Args: blueprint_path (req), widget_name (req — current name), new_name (req). "
			"new_name must match [A-Za-z0-9_], <=100 chars. "
			"WARNING: renaming a BindWidget-matched name (e.g. HostButton) will unbind the C++ UPROPERTY pointer at next compile — "
			"the compiler warning will surface this. "
			"MVVM bindings that have conversion functions are NOT rewritten (see mvvm_bindings_skipped_conversions in the result); "
			"remove and re-add those via remove_view_binding/add_view_binding. "
			"Returns {renamed:true, name:<new>, mvvm_bindings_updated:N, mvvm_bindings_skipped_conversions:[ids...]} "
			"(skipped array omitted when empty). "
			"Errors: widget not found ('list_widgets' hint); new_name invalid; new_name 'already' taken; not a WidgetBlueprint.");
		Def.InputSchema = MakeShared<FJsonObject>();
		Def.InputSchema->SetStringField(TEXT("type"), TEXT("object"));
		{
			TSharedRef<FJsonObject> Properties = MakeShared<FJsonObject>();

			TSharedRef<FJsonObject> BpPath = MakeShared<FJsonObject>();
			BpPath->SetStringField(TEXT("type"), TEXT("string"));
			BpPath->SetStringField(TEXT("description"), TEXT("Absolute asset path to the WidgetBlueprint, e.g. /Game/UI/WBP_MainMenu"));
			Properties->SetObjectField(TEXT("blueprint_path"), BpPath);

			TSharedRef<FJsonObject> WName = MakeShared<FJsonObject>();
			WName->SetStringField(TEXT("type"), TEXT("string"));
			WName->SetStringField(TEXT("description"), TEXT("Current name of the widget to rename (use list_widgets to discover names)."));
			Properties->SetObjectField(TEXT("widget_name"), WName);

			TSharedRef<FJsonObject> NName = MakeShared<FJsonObject>();
			NName->SetStringField(TEXT("type"), TEXT("string"));
			NName->SetStringField(TEXT("description"), TEXT("New name for the widget. Must match [A-Za-z0-9_] and be <=100 characters."));
			Properties->SetObjectField(TEXT("new_name"), NName);

			Def.InputSchema->SetObjectField(TEXT("properties"), Properties);

			TArray<TSharedPtr<FJsonValue>> Required;
			Required.Add(MakeShared<FJsonValueString>(TEXT("blueprint_path")));
			Required.Add(MakeShared<FJsonValueString>(TEXT("widget_name")));
			Required.Add(MakeShared<FJsonValueString>(TEXT("new_name")));
			Def.InputSchema->SetArrayField(TEXT("required"), Required);
		}
		Def.Tier = EAgentMcpTier::SafeWrite;
		Def.Handler = FAgentMcpToolHandler::CreateStatic(&HandleRenameWidget);
		FAgentMcpToolRegistry::Get().Register(MoveTemp(Def));
	}
}
