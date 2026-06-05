#include "Tools/MvvmTools.h"

#include "Blueprint/WidgetTree.h"
#include "Core/AgentMcpToolRegistry.h"
#include "Core/McpTypes.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "MVVMBlueprintFunctionReference.h"
#include "MVVMBlueprintView.h"
#include "MVVMBlueprintViewBinding.h"
#include "MVVMBlueprintViewModelContext.h"
#include "MVVMEditorSubsystem.h"
#include "MVVMPropertyPath.h"
#include "ScopedTransaction.h"
#include "Tools/McpToolUtils.h"
#include "Tools/MvvmToolsShared.h"
#include "Types/MVVMBindingMode.h"
#include "Types/MVVMConversionFunctionValue.h"
#include "Types/MVVMFieldVariant.h"

namespace
{
	using namespace AgentMcp;
	using namespace AgentMcp::MvvmShared;

	// ─────────────────────────────────────────────────────────────────────────
	// add_view_binding  (validate-first: no AddBinding until ALL inputs are OK)
	// ─────────────────────────────────────────────────────────────────────────

	FAgentMcpToolResult HandleAddViewBinding(const TSharedPtr<FJsonObject>& Args)
	{
		if (!Args.IsValid())
		{
			return FAgentMcpToolResult::Error(TEXT("Missing arguments."));
		}

		// ── Required args ───────────────────────────────────────────────────
		FString BlueprintPath;
		if (!Args->TryGetStringField(TEXT("blueprint_path"), BlueprintPath))
		{
			return FAgentMcpToolResult::Error(TEXT("Missing required string argument 'blueprint_path'."));
		}
		FString WidgetNameStr;
		if (!Args->TryGetStringField(TEXT("widget_name"), WidgetNameStr) || WidgetNameStr.IsEmpty())
		{
			return FAgentMcpToolResult::Error(TEXT("Missing required string argument 'widget_name'."));
		}
		FString WidgetPropName;
		if (!Args->TryGetStringField(TEXT("widget_property"), WidgetPropName) || WidgetPropName.IsEmpty())
		{
			return FAgentMcpToolResult::Error(TEXT("Missing required string argument 'widget_property'."));
		}
		FString VMNameStr;
		if (!Args->TryGetStringField(TEXT("viewmodel_name"), VMNameStr) || VMNameStr.IsEmpty())
		{
			return FAgentMcpToolResult::Error(TEXT("Missing required string argument 'viewmodel_name'."));
		}
		FString VMPropName;
		if (!Args->TryGetStringField(TEXT("viewmodel_property"), VMPropName) || VMPropName.IsEmpty())
		{
			return FAgentMcpToolResult::Error(TEXT("Missing required string argument 'viewmodel_property'."));
		}
		FString DirectionToken;
		if (!Args->TryGetStringField(TEXT("direction"), DirectionToken))
		{
			DirectionToken = TEXT("one_way"); // default
		}

		FString Error;

		// ── Parse direction ─────────────────────────────────────────────────
		EMVVMBindingMode BindingMode;
		if (!ParseBindingMode(DirectionToken, BindingMode, Error))
		{
			return FAgentMcpToolResult::Error(Error);
		}

		// ── Resolve blueprint ───────────────────────────────────────────────
		UWidgetBlueprint* WBP = ResolveWBP(BlueprintPath, Error);
		if (!WBP)
		{
			return FAgentMcpToolResult::Error(Error);
		}

		// ── Validate widget exists ──────────────────────────────────────────
		UWidgetTree* Tree = WBP->WidgetTree;
		if (!Tree)
		{
			return FAgentMcpToolResult::Error(TEXT("WidgetBlueprint has no WidgetTree. Use add_widget first."));
		}
		const FName WidgetFName(*WidgetNameStr);
		UWidget* TargetWidget = nullptr;
		bool bIsSelf = false;

		// Check if widget_name refers to the blueprint itself (root/self context).
		if (WidgetFName == WBP->GetFName())
		{
			bIsSelf = true;
		}
		else
		{
			TargetWidget = Tree->FindWidget(WidgetFName);
			if (!TargetWidget)
			{
				return FAgentMcpToolResult::Error(FString::Printf(
					TEXT("Widget '%s' not found in the WidgetTree. Use list_widgets to enumerate available widgets."),
					*WidgetNameStr));
			}
		}

		// ── Validate widget property ────────────────────────────────────────
		// Resolve on the widget's own class (not the generated BP class).
		const FProperty* WidgetField = nullptr;
		const UFunction* WidgetFunc = nullptr;
		if (bIsSelf)
		{
			// Self-context: resolve on the BP's generated class or skeleton.
			UClass* SkelClass = WBP->SkeletonGeneratedClass;
			if (SkelClass)
			{
				WidgetField = SkelClass->FindPropertyByName(FName(*WidgetPropName));
				if (!WidgetField)
				{
					WidgetFunc = SkelClass->FindFunctionByName(FName(*WidgetPropName));
				}
			}
		}
		else
		{
			UClass* WidgetClass = TargetWidget->GetClass();
			WidgetField = WidgetClass->FindPropertyByName(FName(*WidgetPropName));
			if (!WidgetField)
			{
				// Fallback: try function (FieldNotify properties may use getter/setter pairs).
				WidgetFunc = WidgetClass->FindFunctionByName(FName(*WidgetPropName));
			}
		}
		// Validate-first invariant — covers BOTH the self-context and widget branches. A null
		// field here would make AppendPropertyPath silently no-op below and leak a blank
		// destination path (T6 review finding: the self branch previously skipped this guard).
		if (!WidgetField && !WidgetFunc)
		{
			if (bIsSelf)
			{
				const FString SkelName = WBP->SkeletonGeneratedClass ? WBP->SkeletonGeneratedClass->GetName() : TEXT("<null skeleton>");
				return FAgentMcpToolResult::Error(FString::Printf(
					TEXT("Self-context property or function '%s' not found on the WidgetBlueprint's class '%s'. The blueprint may need a compile first, or the name is wrong."),
					*WidgetPropName, *SkelName));
			}
			return FAgentMcpToolResult::Error(FString::Printf(
				TEXT("Widget property or function '%s' not found on widget class '%s'. Verify the property name (e.g. 'Text' for TextBlock, 'Value' for SpinBox)."),
				*WidgetPropName, *TargetWidget->GetClass()->GetName()));
		}

		// ── Get MVVM subsystem + view ───────────────────────────────────────
		UMVVMEditorSubsystem* Sub = GEditor ? GEditor->GetEditorSubsystem<UMVVMEditorSubsystem>() : nullptr;
		if (!Sub)
		{
			return FAgentMcpToolResult::Error(TEXT("UMVVMEditorSubsystem not available."));
		}
		UMVVMBlueprintView* View = Sub->GetView(WBP);
		if (!View)
		{
			return FAgentMcpToolResult::Error(
				TEXT("No MVVM view found on this WidgetBlueprint. Call add_viewmodel first to create the view extension."));
		}

		// ── Validate viewmodel exists ───────────────────────────────────────
		const FName VMFName(*VMNameStr);
		const FMVVMBlueprintViewModelContext* VMCtx = View->FindViewModel(VMFName);
		if (!VMCtx)
		{
			return FAgentMcpToolResult::Error(FString::Printf(
				TEXT("Viewmodel named '%s' not found on this WidgetBlueprint. Use add_viewmodel first."), *VMNameStr));
		}
		const FGuid VMId = VMCtx->GetViewModelId();
		UClass* VMClass = VMCtx->GetViewModelClass();
		if (!VMClass)
		{
			return FAgentMcpToolResult::Error(FString::Printf(
				TEXT("Viewmodel '%s' has no associated class. The viewmodel context may be corrupt."), *VMNameStr));
		}

		// ── Validate viewmodel property ─────────────────────────────────────
		// THE TRAP: AppendPropertyPath/SetPropertyPath silently no-ops on empty FieldVariant.
		// We MUST resolve and null-check the FProperty ourselves.
		const FProperty* VMField = VMClass->FindPropertyByName(FName(*VMPropName));
		const UFunction* VMFunc = nullptr;
		if (!VMField)
		{
			// Fallback: try function (FieldNotify VMs may expose properties via getter/setter).
			VMFunc = VMClass->FindFunctionByName(FName(*VMPropName));
		}
		if (!VMField && !VMFunc)
		{
			return FAgentMcpToolResult::Error(FString::Printf(
				TEXT("Viewmodel property or function '%s' not found on viewmodel class '%s'. "
					"The binding would silently no-op without this check. "
					"Verify the property name matches a FieldNotify property on the viewmodel."),
				*VMPropName, *VMClass->GetName()));
		}

		// ── Pre-resolve conversion functions for type-mismatched pairs ──────
		// The binding compiler requires an explicit conversion function when the two
		// property types differ (e.g. int32 VM property <-> float SpinBox.Value). The
		// View Bindings panel discovers these interactively; replicate that here —
		// BEFORE AddBinding, preserving the validate-first invariant.
		const UFunction* ConvSrcToDst = nullptr;
		const UFunction* ConvDstToSrc = nullptr;
		if (VMField && WidgetField && !VMField->SameType(WidgetField))
		{
			// Engine rule (verified live): two-way bindings cannot use conversion functions.
			if (BindingMode == EMVVMBindingMode::TwoWay)
			{
				return FAgentMcpToolResult::Error(FString::Printf(
					TEXT("'%s' (%s) and '%s' (%s) differ in type, and the engine forbids conversion functions on two_way bindings. "
						"Create TWO bindings instead: direction=one_way (VM->widget) plus direction=one_way_to_source (widget->VM)."),
					*VMPropName, *VMField->GetCPPType(), *WidgetPropName, *WidgetField->GetCPPType()));
			}
			if (BindingMode == EMVVMBindingMode::OneWayToSource)
			{
				ConvDstToSrc = FindConversionFunction(Sub, WBP, WidgetField, VMField);
				if (!ConvDstToSrc)
				{
					return FAgentMcpToolResult::Error(FString::Printf(
						TEXT("Types differ and no conversion function was found for %s -> %s (widget -> viewmodel)."),
						*WidgetField->GetCPPType(), *VMField->GetCPPType()));
				}
			}
			else // OneWayToDestination / OneTimeToDestination
			{
				ConvSrcToDst = FindConversionFunction(Sub, WBP, VMField, WidgetField);
				if (!ConvSrcToDst)
				{
					return FAgentMcpToolResult::Error(FString::Printf(
						TEXT("Types differ ('%s' is %s, '%s' is %s) and no conversion function was found for %s -> %s."),
						*VMPropName, *VMField->GetCPPType(), *WidgetPropName, *WidgetField->GetCPPType(),
						*VMField->GetCPPType(), *WidgetField->GetCPPType()));
				}
			}
		}

		// ── ALL inputs validated — NOW call AddBinding ──────────────────────
		// Using validate-first design: we never call AddBinding before this point,
		// so there is no dangling blank binding to clean up on error.
		FMVVMBlueprintViewBinding& Binding = Sub->AddBinding(WBP);
		// Capture the ID immediately — AddBinding returns a reference into a TArray;
		// any subsequent TArray mutation (including inside Set* calls that may call AddBinding
		// internally) could invalidate the reference. We capture the ID so we can re-fetch later.
		const FGuid BindingId = Binding.BindingId;

		// ── Build paths (kept in scope: conversion pin wiring re-uses them) ─
		FMVVMBlueprintPropertyPath SrcPath;
		SrcPath.SetViewModelId(VMId);
		if (VMField)
		{
			SrcPath.SetPropertyPath(WBP, UE::MVVM::FMVVMConstFieldVariant(VMField));
		}
		else
		{
			SrcPath.SetPropertyPath(WBP, UE::MVVM::FMVVMConstFieldVariant(VMFunc));
		}

		FMVVMBlueprintPropertyPath DstPath;
		DstPath.ResetPropertyPath();
		if (WidgetField)
		{
			DstPath.AppendPropertyPath(WBP, UE::MVVM::FMVVMConstFieldVariant(WidgetField));
		}
		else if (WidgetFunc)
		{
			DstPath.AppendPropertyPath(WBP, UE::MVVM::FMVVMConstFieldVariant(WidgetFunc));
		}
		if (bIsSelf)
		{
			DstPath.SetSelfContext();
		}
		else
		{
			DstPath.SetWidgetName(WidgetFName);
		}

		// SetSourcePathForBinding opens its own FScopedTransaction internally.
		Sub->SetSourcePathForBinding(WBP, Binding, SrcPath);

		// Re-fetch binding by ID — the TArray may have been re-allocated by SetSourcePathForBinding.
		{
			FMVVMBlueprintViewBinding* BindingPtr = View->GetBinding(BindingId);
			if (!BindingPtr)
			{
				// Should never happen, but guard defensively. A null lookup means the row with
				// this id no longer exists (the engine removed it internally) — there is no
				// dangling binding left to clean up (gate-review note).
				return FAgentMcpToolResult::Error(TEXT("Binding was lost after SetSourcePath call. This is an engine-internal error."));
			}
			Sub->SetDestinationPathForBinding(WBP, *BindingPtr, DstPath, /*bAllowEventConversion=*/false);
		}

		// Re-fetch again after destination path set.
		{
			FMVVMBlueprintViewBinding* BindingPtr = View->GetBinding(BindingId);
			if (BindingPtr)
			{
				Sub->SetBindingTypeForBinding(WBP, *BindingPtr, BindingMode);
				Sub->SetEnabledForBinding(WBP, *BindingPtr, true);
			}
		}

		// ── Apply pre-resolved conversion function + wire its data pin ──────
		// Engine behavior (MVVMEditorSubsystem.cpp:416-468): SetXxxConversionFunction first
		// VALIDATES against the binding's current paths (so it must run AFTER both paths are
		// set), then CLEARS the corresponding path and creates a wrapper graph. The data path
		// must then be re-supplied INTO the wrapper's first argument pin — without that, the
		// argument stays a literal default and the binding compiles against a constant.
		if (ConvSrcToDst || ConvDstToSrc)
		{
			const UFunction* ConvFn = ConvSrcToDst ? ConvSrcToDst : ConvDstToSrc;
			const bool bSourceToDestination = ConvSrcToDst != nullptr;
			FMVVMBlueprintViewBinding* BindingPtr = View->GetBinding(BindingId);
			if (BindingPtr)
			{
				if (bSourceToDestination)
				{
					Sub->SetSourceToDestinationConversionFunction(WBP, *BindingPtr,
						FMVVMBlueprintFunctionReference(WBP, ConvFn));
				}
				else
				{
					Sub->SetDestinationToSourceConversionFunction(WBP, *BindingPtr,
						FMVVMBlueprintFunctionReference(WBP, ConvFn));
				}

				BindingPtr = View->GetBinding(BindingId);
				const bool bConversionApplied = BindingPtr &&
					(bSourceToDestination
						? BindingPtr->Conversion.SourceToDestinationConversion != nullptr
						: BindingPtr->Conversion.DestinationToSourceConversion != nullptr);
				if (!bConversionApplied)
				{
					// The subsystem silently nulls invalid conversions — surface it and clean up.
					if (BindingPtr) { View->RemoveBinding(BindingPtr); }
					return FAgentMcpToolResult::Error(FString::Printf(
						TEXT("Engine rejected conversion function '%s' for this binding (incompatible with the resolved paths). Binding removed."),
						*ConvFn->GetName()));
				}

				// Wire the data path into the conversion's first input parameter pin.
				const FName ParamName = FirstInputParamName(ConvFn);
				if (ParamName.IsNone())
				{
					View->RemoveBinding(BindingPtr);
					return FAgentMcpToolResult::Error(FString::Printf(
						TEXT("Conversion function '%s' has no input parameter to wire. Binding removed."), *ConvFn->GetName()));
				}
				TArray<FName> PinNames;
				PinNames.Add(ParamName);
				// For VM->widget conversions the data input is the VIEWMODEL path; for
				// widget->VM conversions it is the WIDGET path.
				Sub->SetPathForConversionFunctionArgument(WBP, *BindingPtr,
					FMVVMBlueprintPinId(MoveTemp(PinNames)),
					bSourceToDestination ? SrcPath : DstPath,
					bSourceToDestination);
			}
		}

		// ── Compile ─────────────────────────────────────────────────────────
		TSharedRef<FJsonObject> CompileObj = RunCompileAndCollect(WBP);

		// ── Result ──────────────────────────────────────────────────────────
		TSharedRef<FJsonObject> Result = MakeShared<FJsonObject>();
		Result->SetBoolField(TEXT("bound"), true);
		Result->SetStringField(TEXT("binding_id"), BindingId.ToString());
		if (ConvSrcToDst) { Result->SetStringField(TEXT("conversion_source_to_dest"), ConvSrcToDst->GetName()); }
		if (ConvDstToSrc) { Result->SetStringField(TEXT("conversion_dest_to_source"), ConvDstToSrc->GetName()); }
		Result->SetObjectField(TEXT("compile"), CompileObj);
		return FAgentMcpToolResult::Success(ToolUtils::SerializeObject(Result));
	}

	// ─────────────────────────────────────────────────────────────────────────
	// list_view_bindings handler
	// ─────────────────────────────────────────────────────────────────────────
	FAgentMcpToolResult HandleListViewBindings(const TSharedPtr<FJsonObject>& Args)
	{
		FString BlueprintPath;
		if (!Args.IsValid() || !Args->TryGetStringField(TEXT("blueprint_path"), BlueprintPath))
		{
			return FAgentMcpToolResult::Error(TEXT("Missing required string argument 'blueprint_path'."));
		}
		FString Error;
		UWidgetBlueprint* WBP = ResolveWBP(BlueprintPath, Error);
		if (!WBP)
		{
			return FAgentMcpToolResult::Error(Error);
		}
		UMVVMEditorSubsystem* Sub = GEditor ? GEditor->GetEditorSubsystem<UMVVMEditorSubsystem>() : nullptr;
		UMVVMBlueprintView* View = Sub ? Sub->GetView(WBP) : nullptr;

		TArray<TSharedPtr<FJsonValue>> Rows;
		if (View)
		{
			for (const FMVVMBlueprintViewBinding& B : View->GetBindings())
			{
				TSharedRef<FJsonObject> Row = MakeShared<FJsonObject>();
				Row->SetStringField(TEXT("binding_id"), B.BindingId.ToString());
				Row->SetStringField(TEXT("name"), B.GetDisplayNameString(WBP));
				Row->SetStringField(TEXT("mode"), BindingModeToToken(B.BindingType));
				Row->SetBoolField(TEXT("enabled"), B.bEnabled);
				Row->SetBoolField(TEXT("compile"), B.bCompile);
				Rows.Add(MakeShared<FJsonValueObject>(Row));
			}
		}
		TSharedRef<FJsonObject> Result = MakeShared<FJsonObject>();
		Result->SetNumberField(TEXT("count"), Rows.Num());
		Result->SetArrayField(TEXT("bindings"), Rows);
		return FAgentMcpToolResult::Success(ToolUtils::SerializeObject(Result));
	}

	// ─────────────────────────────────────────────────────────────────────────
	// remove_view_binding handler
	// ─────────────────────────────────────────────────────────────────────────
	FAgentMcpToolResult HandleRemoveViewBinding(const TSharedPtr<FJsonObject>& Args)
	{
		FString BlueprintPath, BindingIdStr;
		if (!Args.IsValid() || !Args->TryGetStringField(TEXT("blueprint_path"), BlueprintPath) ||
			!Args->TryGetStringField(TEXT("binding_id"), BindingIdStr))
		{
			return FAgentMcpToolResult::Error(TEXT("remove_view_binding requires blueprint_path and binding_id."));
		}
		FString Error;
		UWidgetBlueprint* WBP = ResolveWBP(BlueprintPath, Error);
		if (!WBP)
		{
			return FAgentMcpToolResult::Error(Error);
		}
		FGuid BindingId;
		if (!FGuid::Parse(BindingIdStr, BindingId))
		{
			return FAgentMcpToolResult::Error(FString::Printf(TEXT("'%s' is not a valid binding GUID."), *BindingIdStr));
		}
		UMVVMEditorSubsystem* Sub = GEditor ? GEditor->GetEditorSubsystem<UMVVMEditorSubsystem>() : nullptr;
		UMVVMBlueprintView* View = Sub ? Sub->GetView(WBP) : nullptr;
		if (!View)
		{
			return FAgentMcpToolResult::Error(TEXT("No MVVM view on this WidgetBlueprint."));
		}
		FMVVMBlueprintViewBinding* Binding = View->GetBinding(BindingId);
		if (!Binding)
		{
			return FAgentMcpToolResult::Error(FString::Printf(
				TEXT("Binding '%s' not found. Use list_view_bindings to enumerate."), *BindingIdStr));
		}

		const FString RemovedName = Binding->GetDisplayNameString(WBP);
		{
			const FScopedTransaction Transaction(NSLOCTEXT("AgentMcp", "RemoveViewBinding", "MCP: Remove View Binding"));
			WBP->Modify();
			View->Modify();
			View->RemoveBinding(Binding);
		}

		TSharedRef<FJsonObject> CompileObj = RunCompileAndCollect(WBP);
		TSharedRef<FJsonObject> Result = MakeShared<FJsonObject>();
		Result->SetBoolField(TEXT("removed"), true);
		Result->SetStringField(TEXT("name"), RemovedName);
		Result->SetObjectField(TEXT("compile"), CompileObj);
		return FAgentMcpToolResult::Success(ToolUtils::SerializeObject(Result));
	}

} // anonymous namespace

// ─────────────────────────────────────────────────────────────────────────────
// Registration
// ─────────────────────────────────────────────────────────────────────────────

void AgentMcp::Tools::RegisterMvvmBindingTools()
{
	// add_view_binding
	{
		FAgentMcpToolDef Def;
		Def.Name = TEXT("add_view_binding");
		Def.Description = TEXT(
			"Adds an MVVM view binding between a viewmodel property and a widget property in a WidgetBlueprint. "
			"Uses validate-first design: all inputs are validated before any binding row is created, so no dangling "
			"blank bindings are left behind on error. "
			"NOTE: bound:true means the binding ROW was created — check the embedded compile object (num_errors) for functional success. "
			"IMPORTANT: The viewmodel must be added first with add_viewmodel. "
			"IMPORTANT: viewmodel_property must be a FieldNotify-enabled property — the engine silently no-ops on invalid paths; this tool surfaces that as an explicit error. "
			"Args: blueprint_path (req), widget_name (req — use list_widgets), widget_property (req — e.g. 'Text'), "
			"viewmodel_name (req — name used in add_viewmodel), viewmodel_property (req — property name on the VM class), "
			"direction (opt: one_way (default, VM->widget), two_way, one_time, one_way_to_source (widget->VM)). "
			"Type-mismatched property pairs get a conversion function auto-discovered and wired (e.g. int<->float, bool->ESlateVisibility). "
			"ENGINE RULE: two_way cannot use conversions — for mismatched two-way data, create one_way + one_way_to_source as two bindings. "
			"Returns {bound, binding_id, conversion_source_to_dest?, conversion_dest_to_source?, compile:{status, num_errors, num_warnings}}.");
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
			WName->SetStringField(TEXT("description"), TEXT("Name of the widget to bind to (use list_widgets to discover names)"));
			Properties->SetObjectField(TEXT("widget_name"), WName);

			TSharedRef<FJsonObject> WProp = MakeShared<FJsonObject>();
			WProp->SetStringField(TEXT("type"), TEXT("string"));
			WProp->SetStringField(TEXT("description"), TEXT("Property name on the widget, e.g. Text (TextBlock), Value (SpinBox), ColorAndOpacity"));
			Properties->SetObjectField(TEXT("widget_property"), WProp);

			TSharedRef<FJsonObject> VMName = MakeShared<FJsonObject>();
			VMName->SetStringField(TEXT("type"), TEXT("string"));
			VMName->SetStringField(TEXT("description"), TEXT("Name of the viewmodel as registered via add_viewmodel"));
			Properties->SetObjectField(TEXT("viewmodel_name"), VMName);

			TSharedRef<FJsonObject> VMProp = MakeShared<FJsonObject>();
			VMProp->SetStringField(TEXT("type"), TEXT("string"));
			VMProp->SetStringField(TEXT("description"), TEXT("Property name on the viewmodel class (must be a FieldNotify property)"));
			Properties->SetObjectField(TEXT("viewmodel_property"), VMProp);

			TSharedRef<FJsonObject> Direction = MakeShared<FJsonObject>();
			Direction->SetStringField(TEXT("type"), TEXT("string"));
			Direction->SetStringField(TEXT("description"), TEXT("Binding direction: one_way (VM->widget, default), two_way, one_time."));
			Properties->SetObjectField(TEXT("direction"), Direction);

			Def.InputSchema->SetObjectField(TEXT("properties"), Properties);

			TArray<TSharedPtr<FJsonValue>> Required;
			Required.Add(MakeShared<FJsonValueString>(TEXT("blueprint_path")));
			Required.Add(MakeShared<FJsonValueString>(TEXT("widget_name")));
			Required.Add(MakeShared<FJsonValueString>(TEXT("widget_property")));
			Required.Add(MakeShared<FJsonValueString>(TEXT("viewmodel_name")));
			Required.Add(MakeShared<FJsonValueString>(TEXT("viewmodel_property")));
			Def.InputSchema->SetArrayField(TEXT("required"), Required);
		}
		Def.Tier = EAgentMcpTier::SafeWrite;
		Def.Handler = FAgentMcpToolHandler::CreateStatic(&HandleAddViewBinding);
		FAgentMcpToolRegistry::Get().Register(MoveTemp(Def));
	}

	// list_view_bindings
	{
		FAgentMcpToolDef Def;
		Def.Name = TEXT("list_view_bindings");
		Def.Description = TEXT(
			"Lists all MVVM view bindings on a WidgetBlueprint: binding_id, display name "
			"(Widget.Property <- ViewModel.Property), direction, enabled/compile flags. "
			"Use the binding_id with remove_view_binding. "
			"Args: blueprint_path (req). Returns {count, bindings:[...]}.");
		Def.InputSchema = MakeShared<FJsonObject>();
		Def.InputSchema->SetStringField(TEXT("type"), TEXT("object"));
		{
			TSharedRef<FJsonObject> Properties = MakeShared<FJsonObject>();
			TSharedRef<FJsonObject> BpPath = MakeShared<FJsonObject>();
			BpPath->SetStringField(TEXT("type"), TEXT("string"));
			BpPath->SetStringField(TEXT("description"), TEXT("Absolute asset path to the WidgetBlueprint"));
			Properties->SetObjectField(TEXT("blueprint_path"), BpPath);
			Def.InputSchema->SetObjectField(TEXT("properties"), Properties);
			TArray<TSharedPtr<FJsonValue>> Required;
			Required.Add(MakeShared<FJsonValueString>(TEXT("blueprint_path")));
			Def.InputSchema->SetArrayField(TEXT("required"), Required);
		}
		Def.Tier = EAgentMcpTier::ReadOnly;
		Def.Handler = FAgentMcpToolHandler::CreateStatic(&HandleListViewBindings);
		FAgentMcpToolRegistry::Get().Register(MoveTemp(Def));
	}

	// remove_view_binding
	{
		FAgentMcpToolDef Def;
		Def.Name = TEXT("remove_view_binding");
		Def.Description = TEXT(
			"Removes one MVVM view binding from a WidgetBlueprint by its binding_id "
			"(from list_view_bindings or the add_view_binding response). Recompiles and "
			"reports the result. Args: blueprint_path (req), binding_id (req). "
			"Returns {removed, name, compile:{...}}.");
		Def.InputSchema = MakeShared<FJsonObject>();
		Def.InputSchema->SetStringField(TEXT("type"), TEXT("object"));
		{
			TSharedRef<FJsonObject> Properties = MakeShared<FJsonObject>();
			TSharedRef<FJsonObject> BpPath = MakeShared<FJsonObject>();
			BpPath->SetStringField(TEXT("type"), TEXT("string"));
			BpPath->SetStringField(TEXT("description"), TEXT("Absolute asset path to the WidgetBlueprint"));
			Properties->SetObjectField(TEXT("blueprint_path"), BpPath);
			TSharedRef<FJsonObject> BindingId = MakeShared<FJsonObject>();
			BindingId->SetStringField(TEXT("type"), TEXT("string"));
			BindingId->SetStringField(TEXT("description"), TEXT("GUID of the binding row to remove"));
			Properties->SetObjectField(TEXT("binding_id"), BindingId);
			Def.InputSchema->SetObjectField(TEXT("properties"), Properties);
			TArray<TSharedPtr<FJsonValue>> Required;
			Required.Add(MakeShared<FJsonValueString>(TEXT("blueprint_path")));
			Required.Add(MakeShared<FJsonValueString>(TEXT("binding_id")));
			Def.InputSchema->SetArrayField(TEXT("required"), Required);
		}
		Def.Tier = EAgentMcpTier::SafeWrite;
		Def.Handler = FAgentMcpToolHandler::CreateStatic(&HandleRemoveViewBinding);
		FAgentMcpToolRegistry::Get().Register(MoveTemp(Def));
	}
}
