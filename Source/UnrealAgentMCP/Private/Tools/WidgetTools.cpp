#include "Tools/WidgetTools.h"

#include "AssetRegistry/AssetData.h"
#include "AssetRegistry/AssetRegistryModule.h"
#include "AssetRegistry/IAssetRegistry.h"
#include "BaseWidgetBlueprint.h"
#include "Blueprint/IUserListEntry.h"
#include "Blueprint/UserWidget.h"
#include "Blueprint/WidgetTree.h"
#include "Components/PanelWidget.h"
#include "Components/Widget.h"
#include "Core/AgentMcpToolRegistry.h"
#include "Core/McpTypes.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "EdGraph/EdGraph.h"
#include "K2Node_ComponentBoundEvent.h"
#include "Kismet2/BlueprintEditorUtils.h"
#include "Kismet2/KismetEditorUtilities.h"
#include "Misc/PackageName.h"
#include "Modules/ModuleManager.h"
#include "ScopedTransaction.h"
#include "Tools/McpToolUtils.h"
#include "Tools/NodeGraphUtils.h"
#include "Tools/PropertyBridge.h"
#include "UObject/UObjectGlobals.h"
#include "WidgetBlueprint.h"
#include "Animation/WidgetAnimation.h"
#include "Editor.h"
#include "MVVMBlueprintView.h"
#include "MVVMBlueprintViewBinding.h"
#include "MVVMEditorSubsystem.h"
#include "MVVMPropertyPath.h"

namespace
{
	using namespace AgentMcp;

	// ─────────────────────────────────────────────────────────────────────
	// Helpers
	// ─────────────────────────────────────────────────────────────────────

	/**
	 * Loads a blueprint and casts it to UWidgetBlueprint.
	 * Returns nullptr + error when not a widget BP.
	 */
	UWidgetBlueprint* ResolveWidgetBlueprint(const FString& Path, FString& OutError)
	{
		UBlueprint* BP = NodeGraphUtils::ResolveBlueprint(Path, OutError);
		if (!BP)
		{
			return nullptr;
		}
		UWidgetBlueprint* WBP = Cast<UWidgetBlueprint>(BP);
		if (!WBP)
		{
			OutError = FString::Printf(TEXT("'%s' is not a WidgetBlueprint."), *Path);
			return nullptr;
		}
		return WBP;
	}

	/** Resolves a class token: in-memory name/path, /Game content path (with or without
	 *  .Name_C suffix — normalized), or /Script/ native path. Returns nullptr when unresolvable.
	 *  Shared by ResolveWidgetClass and the EntryWidgetClass special-case in HandleSetWidgetProperty. */
	UClass* ResolveClassToken(const FString& Token)
	{
		UClass* Class = UClass::TryFindTypeSlow<UClass>(Token);
		if (!Class && Token.StartsWith(TEXT("/")) && !Token.StartsWith(TEXT("/Script/")))
		{
			// Content path (user widget): accept /Game/UI/WBP_X, /Game/UI/WBP_X.WBP_X and
			// /Game/UI/WBP_X.WBP_X_C alike — normalize to the generated-class object path
			// (T4 review finding: callers naturally omit the _C suffix; 1.2 dogfooding confirmed).
			FString ClassPath = Token;
			if (!ClassPath.EndsWith(TEXT("_C")))
			{
				FString PackagePath = ClassPath;
				FString ObjectName;
				if (PackagePath.Split(TEXT("."), &PackagePath, &ObjectName))
				{
					ClassPath = FString::Printf(TEXT("%s.%s_C"), *PackagePath, *ObjectName);
				}
				else
				{
					ClassPath = FString::Printf(TEXT("%s.%s_C"), *ClassPath, *FPackageName::GetShortName(ClassPath));
				}
			}
			// Try loading the _C generated class directly first (covers already-compiled BPs).
			// NOTE: safe only when the package is already resident — a cold synchronous load
			// of a WidgetBlueprint can stall in -NullRHI automation (widget compiler absent).
			Class = LoadObject<UClass>(nullptr, *ClassPath);
			if (!Class)
			{
				// _C not in memory — consult the asset registry for the Blueprint's
				// GeneratedClass tag (the full generated-class object path, stored without
				// loading the asset). TryFindTypeSlow resolves it when the class is resident;
				// LoadObject is the last resort for the not-yet-resident case.
				// Deliberately NO NativeParentClass fallback: substituting the bare C++ base
				// for the Blueprint class would silently break consumers like EntryWidgetClass
				// (no widget tree, no bindings) — an unresolvable class must be an ERROR
				// (T1 review finding).
				FString PackagePath = ClassPath;
				int32 DotIdx = INDEX_NONE;
				if (PackagePath.FindChar(TEXT('.'), DotIdx))
				{
					PackagePath = PackagePath.Left(DotIdx);
				}
				IAssetRegistry& AR = FModuleManager::LoadModuleChecked<FAssetRegistryModule>(TEXT("AssetRegistry")).Get();
				// Do NOT call AR.WaitForCompletion() — in -NullRHI automation runs it triggers
				// a FlushAsyncLoading that queues WBP packages which then stall (widget compiler
				// is unavailable), preventing the metadata from being indexed. The registry
				// file-scan metadata is already available for already-discovered assets.
				const FString AssetName = FPackageName::GetShortName(PackagePath);
				TArray<FAssetData> Found;
				AR.GetAssetsByPackageName(FName(*PackagePath), Found);
				if (Found.Num() == 0)
				{
					const FAssetData DirectData = AR.GetAssetByObjectPath(
						FSoftObjectPath(PackagePath + TEXT(".") + AssetName));
					if (DirectData.IsValid())
					{
						Found.Add(DirectData);
					}
				}
				if (Found.Num() > 0)
				{
					// Blueprint tag values use export-text format (e.g. "Class'/Game/X.X_C'").
					// FPackageName::ExportTextPathToObjectPath strips the wrapper to a plain path.
					FAssetTagValueRef GenClassTag = Found[0].TagsAndValues.FindTag(TEXT("GeneratedClass"));
					if (GenClassTag.IsSet())
					{
						const FString TagValue = GenClassTag.AsString();
						const FString ObjectPath = FPackageName::ExportTextPathToObjectPath(TagValue);
						const FString& ResolvePath = ObjectPath.IsEmpty() ? TagValue : ObjectPath;
						Class = UClass::TryFindTypeSlow<UClass>(ResolvePath);
						if (!Class)
						{
							Class = LoadObject<UClass>(nullptr, *ResolvePath);
						}
					}
				}
			}
		}
		return Class;
	}

	/**
	 * Resolves a widget class from a short name like "Button"/"TextBlock"/"VerticalBox"
	 * (tries /Script/UMG.<Name> first via TryFindTypeSlow) or a full path like
	 * /Game/UI/WBP_MyWidget. The resolved class must be a UWidget subclass.
	 */
	UClass* ResolveWidgetClass(const FString& ClassToken, FString& OutError)
	{
		UClass* Class = ResolveClassToken(ClassToken);
		if (!Class)
		{
			// Fallback: try prefixing /Script/UMG. for bare names like "Button".
			const FString ScriptPath = FString::Printf(TEXT("/Script/UMG.%s"), *ClassToken);
			Class = LoadObject<UClass>(nullptr, *ScriptPath);
		}
		if (!Class)
		{
			OutError = FString::Printf(
				TEXT("Widget class '%s' not found. Use a short name like 'Button', 'TextBlock', 'VerticalBox', a /Script/UMG.X path, or a user-widget path like /Game/UI/WBP_MyWidget."),
				*ClassToken);
			return nullptr;
		}
		if (!Class->IsChildOf(UWidget::StaticClass()))
		{
			OutError = FString::Printf(TEXT("'%s' is not a UWidget subclass."), *ClassToken);
			return nullptr;
		}
		return Class;
	}

	// ─────────────────────────────────────────────────────────────────────
	// add_widget handler
	// ─────────────────────────────────────────────────────────────────────

	FAgentMcpToolResult HandleAddWidget(const TSharedPtr<FJsonObject>& Args)
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
		FString WidgetClassToken;
		if (!Args->TryGetStringField(TEXT("widget_class"), WidgetClassToken) || WidgetClassToken.IsEmpty())
		{
			return FAgentMcpToolResult::Error(TEXT("Missing required string argument 'widget_class'."));
		}
		FString WidgetName;
		if (!Args->TryGetStringField(TEXT("widget_name"), WidgetName) || WidgetName.IsEmpty())
		{
			return FAgentMcpToolResult::Error(TEXT("Missing required string argument 'widget_name'."));
		}

		FString Error;
		UWidgetBlueprint* WBP = ResolveWidgetBlueprint(BlueprintPath, Error);
		if (!WBP)
		{
			return FAgentMcpToolResult::Error(Error);
		}

		UClass* WidgetClass = ResolveWidgetClass(WidgetClassToken, Error);
		if (!WidgetClass)
		{
			return FAgentMcpToolResult::Error(Error);
		}

		// May be null on a freshly created blueprint — created inside the transaction below
		// so a Cancel() rolls the assignment back (T4 review finding).
		UWidgetTree* Tree = WBP->WidgetTree;

		// Optional placement args.
		FString ParentName;
		Args->TryGetStringField(TEXT("parent_name"), ParentName);
		bool bAsRoot = false;
		Args->TryGetBoolField(TEXT("as_root"), bAsRoot);

		// Validate placement before opening transaction (Tree may be null = empty tree).
		if (bAsRoot && Tree && Tree->RootWidget != nullptr)
		{
			return FAgentMcpToolResult::Error(TEXT("as_root:true but the tree already has a root widget."));
		}

		UPanelWidget* ParentPanel = nullptr;
		if (!ParentName.IsEmpty())
		{
			UWidget* ParentWidget = Tree ? Tree->FindWidget(FName(*ParentName)) : nullptr;
			if (!ParentWidget)
			{
				return FAgentMcpToolResult::Error(FString::Printf(
					TEXT("parent_name widget '%s' not found in the tree. Use list_widgets to enumerate."), *ParentName));
			}
			ParentPanel = Cast<UPanelWidget>(ParentWidget);
			if (!ParentPanel)
			{
				return FAgentMcpToolResult::Error(FString::Printf(
					TEXT("parent_name widget '%s' is not a UPanelWidget and cannot have children."), *ParentName));
			}
		}

		// If neither as_root nor parent_name, auto-root only when tree is empty.
		if (!bAsRoot && ParentName.IsEmpty())
		{
			if (Tree && Tree->RootWidget != nullptr)
			{
				return FAgentMcpToolResult::Error(
					TEXT("parent_name is required when the tree already has a root widget."));
			}
			// Treat this as root placement.
			bAsRoot = true;
		}

		// Deduplicate name via flat tree lookup (StaticFindObject would misread names
		// containing path separators — T4 review finding).
		FName DesiredName(*WidgetName);
		FName ActualName = DesiredName;
		if (Tree && Tree->FindWidget(DesiredName) != nullptr)
		{
			ActualName = MakeUniqueObjectName(Tree, WidgetClass, DesiredName);
		}

		// Open transaction.
		FScopedTransaction Transaction(NSLOCTEXT("AgentMcp", "AddWidget", "MCP: Add Widget"));
		WBP->Modify();
		if (!Tree)
		{
			Tree = NewObject<UWidgetTree>(WBP, NAME_None, RF_Transactional);
			WBP->WidgetTree = Tree;
		}
		Tree->SetFlags(RF_Transactional);
		Tree->Modify();

		// Construct.
		UWidget* NewWidget = Tree->ConstructWidget<UWidget>(WidgetClass, ActualName);
		if (!NewWidget)
		{
			Transaction.Cancel();
			return FAgentMcpToolResult::Error(FString::Printf(
				TEXT("UWidgetTree::ConstructWidget failed for class '%s'."), *WidgetClassToken));
		}

		NewWidget->CreatedFromPalette();
		NewWidget->bIsVariable = true;

		// Place.
		FString PlacementParent;
		if (bAsRoot)
		{
			Tree->RootWidget = NewWidget;
			PlacementParent = TEXT("root");
		}
		else
		{
			checkf(ParentPanel, TEXT("ParentPanel must be valid here"));
			ParentPanel->Modify();
			UPanelSlot* Slot = ParentPanel->AddChild(NewWidget);
			if (!Slot)
			{
				Transaction.Cancel();
				return FAgentMcpToolResult::Error(FString::Printf(
					TEXT("UPanelWidget::AddChild rejected '%s' — panel may be full or incompatible."), *WidgetClassToken));
			}
			PlacementParent = ParentName;
		}

		FBlueprintEditorUtils::MarkBlueprintAsStructurallyModified(WBP);

		TSharedRef<FJsonObject> Result = MakeShared<FJsonObject>();
		Result->SetBoolField(TEXT("added"), true);
		Result->SetStringField(TEXT("name"), ActualName.ToString());
		Result->SetStringField(TEXT("class"), WidgetClass->GetName());
		Result->SetStringField(TEXT("parent"), PlacementParent);
		return FAgentMcpToolResult::Success(ToolUtils::SerializeObject(Result));
	}

	// ─────────────────────────────────────────────────────────────────────
	// list_widgets handler
	// ─────────────────────────────────────────────────────────────────────

	FAgentMcpToolResult HandleListWidgets(const TSharedPtr<FJsonObject>& Args)
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

		FString Error;
		UWidgetBlueprint* WBP = ResolveWidgetBlueprint(BlueprintPath, Error);
		if (!WBP)
		{
			return FAgentMcpToolResult::Error(Error);
		}

		UWidgetTree* Tree = WBP->WidgetTree;
		TArray<TSharedPtr<FJsonValue>> WidgetArray;

		if (Tree)
		{
			TArray<UWidget*> AllWidgets;
			Tree->GetAllWidgets(AllWidgets);

			for (UWidget* W : AllWidgets)
			{
				if (!W)
				{
					continue;
				}
				TSharedRef<FJsonObject> Entry = MakeShared<FJsonObject>();
				Entry->SetStringField(TEXT("name"), W->GetFName().ToString());
				Entry->SetStringField(TEXT("class"), W->GetClass()->GetName());
				Entry->SetStringField(TEXT("display_label"), W->GetDisplayLabel());
				Entry->SetBoolField(TEXT("is_variable"), W->bIsVariable);

				// Find parent.
				int32 ChildIndex = INDEX_NONE;
				UPanelWidget* ParentWidget = UWidgetTree::FindWidgetParent(W, ChildIndex);
				if (ParentWidget)
				{
					Entry->SetStringField(TEXT("parent"), ParentWidget->GetFName().ToString());
				}
				else
				{
					Entry->SetStringField(TEXT("parent"), FString());
				}

				WidgetArray.Add(MakeShared<FJsonValueObject>(Entry));
			}
		}

		TSharedRef<FJsonObject> Result = MakeShared<FJsonObject>();
		Result->SetNumberField(TEXT("count"), WidgetArray.Num());
		Result->SetArrayField(TEXT("widgets"), WidgetArray);
		return FAgentMcpToolResult::Success(ToolUtils::SerializeObject(Result));
	}

	// ─────────────────────────────────────────────────────────────────────
	// set_widget_property handler
	// ─────────────────────────────────────────────────────────────────────

	FAgentMcpToolResult HandleSetWidgetProperty(const TSharedPtr<FJsonObject>& Args)
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
		FString WidgetName;
		if (!Args->TryGetStringField(TEXT("widget_name"), WidgetName) || WidgetName.IsEmpty())
		{
			return FAgentMcpToolResult::Error(TEXT("Missing required string argument 'widget_name'."));
		}
		FString PropertyName;
		if (!Args->TryGetStringField(TEXT("property"), PropertyName) || PropertyName.IsEmpty())
		{
			return FAgentMcpToolResult::Error(TEXT("Missing required string argument 'property'."));
		}
		FString Value;
		if (!Args->TryGetStringField(TEXT("value"), Value))
		{
			return FAgentMcpToolResult::Error(TEXT("Missing required string argument 'value'."));
		}

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

		UWidget* Widget = Tree->FindWidget(FName(*WidgetName));
		if (!Widget)
		{
			return FAgentMcpToolResult::Error(FString::Printf(
				TEXT("Widget '%s' not found. Use list_widgets to see available widgets."), *WidgetName));
		}

		// Special-case EntryWidgetClass: verify the class implements IUserListEntry.
		// ResolveClassToken handles /Game content paths with or without the _C suffix
		// (1.2 dogfooding: callers pass /Game/UI/WBP_X, not the generated /Game/UI/WBP_X.WBP_X_C).
		if (PropertyName.Equals(TEXT("EntryWidgetClass"), ESearchCase::IgnoreCase))
		{
			UClass* EntryClass = ResolveClassToken(Value);
			if (!EntryClass)
			{
				return FAgentMcpToolResult::Error(FString::Printf(
					TEXT("Could not resolve EntryWidgetClass value '%s' to a UClass. Ensure the Blueprint is compiled in this editor session (open it once, or compile_blueprint it)."), *Value));
			}
			if (!EntryClass->ImplementsInterface(UUserListEntry::StaticClass()))
			{
				return FAgentMcpToolResult::Error(FString::Printf(
					TEXT("Class '%s' does not implement IUserListEntry; it cannot be used as EntryWidgetClass."),
					*EntryClass->GetName()));
			}
			// Normalize Value to the resolved class's full path so PropertyBridge can set the
			// property even when the caller passed a /Game/ path without the _C suffix — the class
			// IS in memory at this point, so GetPathName() is always valid.
			Value = EntryClass->GetPathName();
		}

		// Transaction + set.
		FScopedTransaction Transaction(NSLOCTEXT("AgentMcp", "SetWidgetProperty", "MCP: Set Widget Property"));
		Widget->Modify();

		if (!PropertyBridge::SetPropertyFromString(Widget, PropertyName, Value, Error, /*bRejectTemplateDisabled=*/false))
		{
			Transaction.Cancel();
			return FAgentMcpToolResult::Error(Error);
		}

		// Value-only change: plain modified is enough — the structural variant triggers a full
		// skeleton recompile and is reserved for topology changes (T4 review finding).
		FBlueprintEditorUtils::MarkBlueprintAsModified(WBP);

		// Read back.
		FString ReadBack, ReadBackType;
		PropertyBridge::GetPropertyAsString(Widget, PropertyName, ReadBack, ReadBackType, Error);

		TSharedRef<FJsonObject> Result = MakeShared<FJsonObject>();
		Result->SetBoolField(TEXT("set"), true);
		Result->SetStringField(TEXT("widget"), WidgetName);
		Result->SetStringField(TEXT("property"), PropertyName);
		Result->SetStringField(TEXT("value"), ReadBack);
		return FAgentMcpToolResult::Success(ToolUtils::SerializeObject(Result));
	}

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
	 * Validates a proposed widget name: [A-Za-z0-9_], non-empty, ≤100 chars.
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
				TEXT("Widget '%s' already has that name. already occupied."),
				*NewName));
		}
		// Also check non-widget subobjects (slots share the same outer).
		UObject* SubObj = StaticFindObject(UObject::StaticClass(), Tree, *NewName);
		if (SubObj && SubObj != Widget)
		{
			return FAgentMcpToolResult::Error(FString::Printf(
				TEXT("Name '%s' is already taken by another subobject. already occupied."),
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
		Widget->Rename(*NewNameStr);

		// Update FDelegateEditorBinding references (property bindings in the editor).
		for (FDelegateEditorBinding& Binding : WBP->Bindings)
		{
			if (Binding.ObjectName == OldName)
			{
				Binding.ObjectName = NewNameStr;
			}
		}

		// Update widget animation bindings.
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

void AgentMcp::Tools::RegisterWidgetTools()
{
	// add_widget
	{
		FAgentMcpToolDef Def;
		Def.Name = TEXT("add_widget");
		Def.Description = TEXT(
			"Adds a widget to a WidgetBlueprint's tree. "
			"Args: blueprint_path (req), widget_class (req — short name like 'Button'/'TextBlock'/'VerticalBox' or full /Script/UMG.<Name>), "
			"widget_name (req), parent_name (opt — must resolve to a UPanelWidget), as_root (opt bool). "
			"Name collision is resolved automatically via MakeUniqueObjectName — the actual name is in the response. "
			"Returns {added, name, class, parent}.");
		Def.InputSchema = MakeShared<FJsonObject>();
		Def.InputSchema->SetStringField(TEXT("type"), TEXT("object"));
		{
			TSharedRef<FJsonObject> Properties = MakeShared<FJsonObject>();

			TSharedRef<FJsonObject> BpPath = MakeShared<FJsonObject>();
			BpPath->SetStringField(TEXT("type"), TEXT("string"));
			BpPath->SetStringField(TEXT("description"), TEXT("Absolute asset path to the WidgetBlueprint, e.g. /Game/UI/WBP_MainMenu"));
			Properties->SetObjectField(TEXT("blueprint_path"), BpPath);

			TSharedRef<FJsonObject> WClass = MakeShared<FJsonObject>();
			WClass->SetStringField(TEXT("type"), TEXT("string"));
			WClass->SetStringField(TEXT("description"), TEXT("Widget class: short name like Button, TextBlock, VerticalBox, or a full path like /Script/UMG.Button"));
			Properties->SetObjectField(TEXT("widget_class"), WClass);

			TSharedRef<FJsonObject> WName = MakeShared<FJsonObject>();
			WName->SetStringField(TEXT("type"), TEXT("string"));
			WName->SetStringField(TEXT("description"), TEXT("Desired name for the new widget. Uniquified automatically on collision."));
			Properties->SetObjectField(TEXT("widget_name"), WName);

			TSharedRef<FJsonObject> PName = MakeShared<FJsonObject>();
			PName->SetStringField(TEXT("type"), TEXT("string"));
			PName->SetStringField(TEXT("description"), TEXT("Name of an existing UPanelWidget to attach to as a child. Required when the tree already has a root."));
			Properties->SetObjectField(TEXT("parent_name"), PName);

			TSharedRef<FJsonObject> AsRoot = MakeShared<FJsonObject>();
			AsRoot->SetStringField(TEXT("type"), TEXT("boolean"));
			AsRoot->SetStringField(TEXT("description"), TEXT("Set true to place this widget as the tree root. Errors if a root already exists."));
			Properties->SetObjectField(TEXT("as_root"), AsRoot);

			Def.InputSchema->SetObjectField(TEXT("properties"), Properties);

			TArray<TSharedPtr<FJsonValue>> Required;
			Required.Add(MakeShared<FJsonValueString>(TEXT("blueprint_path")));
			Required.Add(MakeShared<FJsonValueString>(TEXT("widget_class")));
			Required.Add(MakeShared<FJsonValueString>(TEXT("widget_name")));
			Def.InputSchema->SetArrayField(TEXT("required"), Required);
		}
		Def.Tier = EAgentMcpTier::SafeWrite;
		Def.Handler = FAgentMcpToolHandler::CreateStatic(&HandleAddWidget);
		FAgentMcpToolRegistry::Get().Register(MoveTemp(Def));
	}

	// list_widgets
	{
		FAgentMcpToolDef Def;
		Def.Name = TEXT("list_widgets");
		Def.Description = TEXT(
			"Enumerates all widgets in a WidgetBlueprint's tree. "
			"Args: blueprint_path (req). "
			"Returns {count, widgets:[{name, class, display_label, is_variable, parent}]}. "
			"parent is empty for the root widget.");
		Def.InputSchema = MakeShared<FJsonObject>();
		Def.InputSchema->SetStringField(TEXT("type"), TEXT("object"));
		{
			TSharedRef<FJsonObject> Properties = MakeShared<FJsonObject>();

			TSharedRef<FJsonObject> BpPath = MakeShared<FJsonObject>();
			BpPath->SetStringField(TEXT("type"), TEXT("string"));
			BpPath->SetStringField(TEXT("description"), TEXT("Absolute asset path to the WidgetBlueprint, e.g. /Game/UI/WBP_MainMenu"));
			Properties->SetObjectField(TEXT("blueprint_path"), BpPath);

			Def.InputSchema->SetObjectField(TEXT("properties"), Properties);

			TArray<TSharedPtr<FJsonValue>> Required;
			Required.Add(MakeShared<FJsonValueString>(TEXT("blueprint_path")));
			Def.InputSchema->SetArrayField(TEXT("required"), Required);
		}
		Def.Tier = EAgentMcpTier::ReadOnly;
		Def.Handler = FAgentMcpToolHandler::CreateStatic(&HandleListWidgets);
		FAgentMcpToolRegistry::Get().Register(MoveTemp(Def));
	}

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
			"new_name must match [A-Za-z0-9_], ≤100 chars. "
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
			NName->SetStringField(TEXT("description"), TEXT("New name for the widget. Must match [A-Za-z0-9_] and be ≤100 characters."));
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

	// set_widget_property
	{
		FAgentMcpToolDef Def;
		Def.Name = TEXT("set_widget_property");
		Def.Description = TEXT(
			"Sets a property on a widget template instance inside a WidgetBlueprint's tree. "
			"Args: blueprint_path (req), widget_name (req), property (req), value (req). "
			"Use list_widgets to enumerate available widgets. "
			"EntryWidgetClass is special-cased: the value class must implement IUserListEntry. "
			"Returns {set, widget, property, value:<readback>}.");
		Def.InputSchema = MakeShared<FJsonObject>();
		Def.InputSchema->SetStringField(TEXT("type"), TEXT("object"));
		{
			TSharedRef<FJsonObject> Properties = MakeShared<FJsonObject>();

			TSharedRef<FJsonObject> BpPath = MakeShared<FJsonObject>();
			BpPath->SetStringField(TEXT("type"), TEXT("string"));
			BpPath->SetStringField(TEXT("description"), TEXT("Absolute asset path to the WidgetBlueprint"));
			Properties->SetObjectField(TEXT("blueprint_path"), BpPath);

			TSharedRef<FJsonObject> WName = MakeShared<FJsonObject>();
			WName->SetStringField(TEXT("type"), TEXT("string"));
			WName->SetStringField(TEXT("description"), TEXT("Name of the widget to modify (use list_widgets to discover names)"));
			Properties->SetObjectField(TEXT("widget_name"), WName);

			TSharedRef<FJsonObject> Prop = MakeShared<FJsonObject>();
			Prop->SetStringField(TEXT("type"), TEXT("string"));
			Prop->SetStringField(TEXT("description"), TEXT("Property name to set, e.g. Text, ColorAndOpacity, EntryWidgetClass"));
			Properties->SetObjectField(TEXT("property"), Prop);

			TSharedRef<FJsonObject> Val = MakeShared<FJsonObject>();
			Val->SetStringField(TEXT("type"), TEXT("string"));
			Val->SetStringField(TEXT("description"), TEXT("Value to import via UE text format (culture-invariant)"));
			Properties->SetObjectField(TEXT("value"), Val);

			Def.InputSchema->SetObjectField(TEXT("properties"), Properties);

			TArray<TSharedPtr<FJsonValue>> Required;
			Required.Add(MakeShared<FJsonValueString>(TEXT("blueprint_path")));
			Required.Add(MakeShared<FJsonValueString>(TEXT("widget_name")));
			Required.Add(MakeShared<FJsonValueString>(TEXT("property")));
			Required.Add(MakeShared<FJsonValueString>(TEXT("value")));
			Def.InputSchema->SetArrayField(TEXT("required"), Required);
		}
		Def.Tier = EAgentMcpTier::SafeWrite;
		Def.Handler = FAgentMcpToolHandler::CreateStatic(&HandleSetWidgetProperty);
		FAgentMcpToolRegistry::Get().Register(MoveTemp(Def));
	}
}
