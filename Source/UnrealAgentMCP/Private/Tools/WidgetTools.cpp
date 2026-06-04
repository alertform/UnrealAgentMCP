#include "Tools/WidgetTools.h"

#include "Blueprint/IUserListEntry.h"
#include "Blueprint/UserWidget.h"
#include "Blueprint/WidgetTree.h"
#include "Components/PanelWidget.h"
#include "Components/Widget.h"
#include "Core/AgentMcpToolRegistry.h"
#include "Core/McpTypes.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "Kismet2/BlueprintEditorUtils.h"
#include "ScopedTransaction.h"
#include "Tools/McpToolUtils.h"
#include "Tools/NodeGraphUtils.h"
#include "Tools/PropertyBridge.h"
#include "UObject/UObjectGlobals.h"
#include "WidgetBlueprint.h"

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

	/**
	 * Resolves a widget class from a short name like "Button"/"TextBlock"/"VerticalBox"
	 * (tries /Script/UMG.<Name> first via TryFindTypeSlow) or a full path like
	 * /Game/UI/WBP_MyWidget. The resolved class must be a UWidget subclass.
	 */
	UClass* ResolveWidgetClass(const FString& ClassToken, FString& OutError)
	{
		// TryFindTypeSlow handles both short names and full /Script/ paths.
		UClass* Class = UClass::TryFindTypeSlow<UClass>(ClassToken);
		if (!Class)
		{
			// Fallback: try prefixing /Script/UMG. for bare names like "Button".
			const FString ScriptPath = FString::Printf(TEXT("/Script/UMG.%s"), *ClassToken);
			Class = LoadObject<UClass>(nullptr, *ScriptPath);
		}
		if (!Class)
		{
			OutError = FString::Printf(
				TEXT("Widget class '%s' not found. Use a short name like 'Button', 'TextBlock', 'VerticalBox', or a full path like /Script/UMG.Button."),
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

		// Ensure WidgetTree exists.
		UWidgetTree* Tree = WBP->WidgetTree;
		if (!Tree)
		{
			Tree = NewObject<UWidgetTree>(WBP, NAME_None, RF_Transactional);
			WBP->WidgetTree = Tree;
		}

		// Optional placement args.
		FString ParentName;
		Args->TryGetStringField(TEXT("parent_name"), ParentName);
		bool bAsRoot = false;
		Args->TryGetBoolField(TEXT("as_root"), bAsRoot);

		// Validate placement before opening transaction.
		if (bAsRoot && Tree->RootWidget != nullptr)
		{
			return FAgentMcpToolResult::Error(TEXT("as_root:true but the tree already has a root widget."));
		}

		UPanelWidget* ParentPanel = nullptr;
		if (!ParentName.IsEmpty())
		{
			UWidget* ParentWidget = Tree->FindWidget(FName(*ParentName));
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
			if (Tree->RootWidget != nullptr)
			{
				return FAgentMcpToolResult::Error(
					TEXT("parent_name is required when the tree already has a root widget."));
			}
			// Treat this as root placement.
			bAsRoot = true;
		}

		// Deduplicate name.
		FName DesiredName(*WidgetName);
		FName ActualName = DesiredName;
		if (StaticFindObject(UObject::StaticClass(), Tree, *WidgetName) != nullptr)
		{
			ActualName = MakeUniqueObjectName(Tree, WidgetClass, DesiredName);
		}

		// Open transaction.
		FScopedTransaction Transaction(NSLOCTEXT("AgentMcp", "AddWidget", "MCP: Add Widget"));
		Tree->SetFlags(RF_Transactional);
		Tree->Modify();
		WBP->Modify();

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
		if (PropertyName.Equals(TEXT("EntryWidgetClass"), ESearchCase::IgnoreCase))
		{
			UClass* EntryClass = UClass::TryFindTypeSlow<UClass>(Value);
			if (!EntryClass)
			{
				EntryClass = LoadObject<UClass>(nullptr, *Value);
			}
			if (!EntryClass)
			{
				return FAgentMcpToolResult::Error(FString::Printf(
					TEXT("Could not resolve EntryWidgetClass value '%s' to a UClass."), *Value));
			}
			if (!EntryClass->ImplementsInterface(UUserListEntry::StaticClass()))
			{
				return FAgentMcpToolResult::Error(FString::Printf(
					TEXT("Class '%s' does not implement IUserListEntry; it cannot be used as EntryWidgetClass."),
					*EntryClass->GetName()));
			}
		}

		// Transaction + set.
		FScopedTransaction Transaction(NSLOCTEXT("AgentMcp", "SetWidgetProperty", "MCP: Set Widget Property"));
		Widget->Modify();

		if (!PropertyBridge::SetPropertyFromString(Widget, PropertyName, Value, Error, /*bRejectTemplateDisabled=*/false))
		{
			Transaction.Cancel();
			return FAgentMcpToolResult::Error(Error);
		}

		FBlueprintEditorUtils::MarkBlueprintAsStructurallyModified(WBP);

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
