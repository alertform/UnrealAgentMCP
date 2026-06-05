#include "Tools/MvvmTools.h"

#include "Blueprint/WidgetTree.h"
#include "Core/AgentMcpToolRegistry.h"
#include "Core/McpTypes.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "INotifyFieldValueChanged.h"
#include "Kismet2/CompilerResultsLog.h"
#include "Kismet2/KismetEditorUtilities.h"
#include "Logging/TokenizedMessage.h"
#include "MVVMBlueprintView.h"
#include "MVVMBlueprintViewBinding.h"
#include "MVVMBlueprintViewModelContext.h"
#include "MVVMEditorSubsystem.h"
#include "MVVMPropertyPath.h"
#include "ScopedTransaction.h"
#include "Tools/McpToolUtils.h"
#include "Tools/NodeGraphUtils.h"
#include "Types/MVVMBindingMode.h"
#include "Types/MVVMFieldVariant.h"
#include "WidgetBlueprint.h"

namespace
{
	using namespace AgentMcp;

	// ─────────────────────────────────────────────────────────────────────────
	// Shared helpers
	// ─────────────────────────────────────────────────────────────────────────

	/** Resolve a WidgetBlueprint by path. */
	UWidgetBlueprint* ResolveWBP(const FString& Path, FString& OutError)
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

	/** Resolve a UClass from a short name or full /Script/ path. */
	UClass* ResolveClass(const FString& Token, FString& OutError)
	{
		UClass* Class = nullptr;
		if (Token.Contains(TEXT(".")))
		{
			Class = FindObject<UClass>(nullptr, *Token);
			if (!Class)
			{
				Class = LoadObject<UClass>(nullptr, *Token);
			}
		}
		if (!Class)
		{
			Class = UClass::TryFindTypeSlow<UClass>(Token);
		}
		if (!Class)
		{
			OutError = FString::Printf(TEXT("Class '%s' not found. Use a full path like /Script/ModelViewViewModel.MVVMViewModelBase."), *Token);
			return nullptr;
		}
		return Class;
	}

	/** Collect compile result fields — mirrors BlueprintTools.cpp HandleCompileBlueprint output. */
	TSharedRef<FJsonObject> RunCompileAndCollect(UBlueprint* BP)
	{
		FCompilerResultsLog Results;
		FKismetEditorUtilities::CompileBlueprint(BP, EBlueprintCompileOptions::None, &Results);

		TArray<TSharedPtr<FJsonValue>> Messages;
		for (const TSharedRef<FTokenizedMessage>& Msg : Results.Messages)
		{
			TSharedRef<FJsonObject> Entry = MakeShared<FJsonObject>();
			Entry->SetStringField(TEXT("severity"),
				Msg->GetSeverity() == EMessageSeverity::Error ? TEXT("error") : TEXT("warning"));
			Entry->SetStringField(TEXT("text"), Msg->ToText().ToString());
			Messages.Add(MakeShared<FJsonValueObject>(Entry));
		}

		TSharedRef<FJsonObject> CompileObj = MakeShared<FJsonObject>();
		CompileObj->SetStringField(TEXT("status"), Results.NumErrors > 0 ? TEXT("error") : TEXT("ok"));
		CompileObj->SetNumberField(TEXT("num_errors"), Results.NumErrors);
		CompileObj->SetNumberField(TEXT("num_warnings"), Results.NumWarnings);
		CompileObj->SetArrayField(TEXT("messages"), Messages);
		return CompileObj;
	}

	/** Parse binding direction token. */
	bool ParseBindingMode(const FString& Token, EMVVMBindingMode& OutMode, FString& OutError)
	{
		const FString Lower = Token.ToLower();
		if (Lower == TEXT("one_way") || Lower == TEXT("oneway"))
		{
			OutMode = EMVVMBindingMode::OneWayToDestination;
			return true;
		}
		if (Lower == TEXT("two_way") || Lower == TEXT("twoway"))
		{
			OutMode = EMVVMBindingMode::TwoWay;
			return true;
		}
		if (Lower == TEXT("one_time") || Lower == TEXT("onetime"))
		{
			OutMode = EMVVMBindingMode::OneTimeToDestination;
			return true;
		}
		OutError = FString::Printf(
			TEXT("Unknown direction '%s'. Valid values: one_way (viewmodel→widget), two_way, one_time."), *Token);
		return false;
	}

	/** Parse creation_type token. */
	bool ParseCreationType(const FString& Token, EMVVMBlueprintViewModelContextCreationType& OutType, FString& OutError)
	{
		const FString Lower = Token.ToLower();
		if (Lower == TEXT("manual") || Lower.IsEmpty()) { OutType = EMVVMBlueprintViewModelContextCreationType::Manual; return true; }
		if (Lower == TEXT("create_instance"))            { OutType = EMVVMBlueprintViewModelContextCreationType::CreateInstance; return true; }
		if (Lower == TEXT("global"))                     { OutType = EMVVMBlueprintViewModelContextCreationType::GlobalViewModelCollection; return true; }
		if (Lower == TEXT("property_path"))              { OutType = EMVVMBlueprintViewModelContextCreationType::PropertyPath; return true; }
		if (Lower == TEXT("resolver"))                   { OutType = EMVVMBlueprintViewModelContextCreationType::Resolver; return true; }
		OutError = FString::Printf(
			TEXT("Unknown creation_type '%s'. Valid values: manual (default), create_instance, global, property_path, resolver."), *Token);
		return false;
	}

	// ─────────────────────────────────────────────────────────────────────────
	// add_viewmodel
	// ─────────────────────────────────────────────────────────────────────────

	FAgentMcpToolResult HandleAddViewModel(const TSharedPtr<FJsonObject>& Args)
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
		FString VMClassToken;
		if (!Args->TryGetStringField(TEXT("viewmodel_class"), VMClassToken) || VMClassToken.IsEmpty())
		{
			return FAgentMcpToolResult::Error(TEXT("Missing required string argument 'viewmodel_class'."));
		}
		FString VMName;
		if (!Args->TryGetStringField(TEXT("name"), VMName) || VMName.IsEmpty())
		{
			return FAgentMcpToolResult::Error(TEXT("Missing required string argument 'name'."));
		}

		// ── Optional args ───────────────────────────────────────────────────
		FString CreationTypeToken;
		Args->TryGetStringField(TEXT("creation_type"), CreationTypeToken);

		FString Error;
		EMVVMBlueprintViewModelContextCreationType CreationType;
		if (!ParseCreationType(CreationTypeToken, CreationType, Error))
		{
			return FAgentMcpToolResult::Error(Error);
		}

		// ── Resolve blueprint ───────────────────────────────────────────────
		UWidgetBlueprint* WBP = ResolveWBP(BlueprintPath, Error);
		if (!WBP)
		{
			return FAgentMcpToolResult::Error(Error);
		}

		// ── Resolve viewmodel class ─────────────────────────────────────────
		UClass* VMClass = ResolveClass(VMClassToken, Error);
		if (!VMClass)
		{
			return FAgentMcpToolResult::Error(Error);
		}

		// ── Validate: class must implement INotifyFieldValueChanged ─────────
		if (!VMClass->ImplementsInterface(UNotifyFieldValueChanged::StaticClass()))
		{
			return FAgentMcpToolResult::Error(FString::Printf(
				TEXT("Class '%s' does not implement INotifyFieldValueChanged. "
					"Only classes that implement this interface can be used as viewmodels. "
					"Use a subclass of UMVVMViewModelBase (e.g. /Script/ModelViewViewModel.MVVMViewModelBase)."),
				*VMClass->GetName()));
		}

		// ── Get-or-create the MVVM view extension ──────────────────────────
		UMVVMEditorSubsystem* Sub = GEditor ? GEditor->GetEditorSubsystem<UMVVMEditorSubsystem>() : nullptr;
		if (!Sub)
		{
			return FAgentMcpToolResult::Error(TEXT("UMVVMEditorSubsystem not available. Is the ModelViewViewModel plugin loaded?"));
		}
		UMVVMBlueprintView* View = Sub->RequestView(WBP);
		if (!View)
		{
			return FAgentMcpToolResult::Error(TEXT("RequestView returned null — unable to create MVVM view extension on this WidgetBlueprint."));
		}

		// ── Duplicate name check ────────────────────────────────────────────
		const FName VMFName(*VMName);
		if (View->FindViewModel(VMFName) != nullptr)
		{
			return FAgentMcpToolResult::Error(FString::Printf(
				TEXT("A viewmodel named '%s' already exists on this WidgetBlueprint. Use a unique name."), *VMName));
		}

		// ── Build context: ctor sets CreationType from GetAllowedContextCreationType[0];
		//    we MUST override it after construction. ─────────────────────────
		FMVVMBlueprintViewModelContext Ctx(VMClass, VMFName);
		if (!Ctx.IsValid())
		{
			// IsValid() returns false when NotifyFieldValueClass is null — i.e., the interface
			// check inside the ctor failed (belt-and-suspenders, the explicit check above covers this).
			return FAgentMcpToolResult::Error(FString::Printf(
				TEXT("FMVVMBlueprintViewModelContext construction failed for class '%s'. "
					"The class must implement INotifyFieldValueChanged."), *VMClass->GetName()));
		}
		// Override creation type — the ctor sets its own default from project settings.
		Ctx.CreationType = CreationType;

		// ── Transactional add ───────────────────────────────────────────────
		{
			const FScopedTransaction Transaction(NSLOCTEXT("AgentMcp", "AddViewModel", "MCP: Add ViewModel"));
			WBP->Modify();
			View->Modify();
			View->AddViewModel(Ctx);
		}

		// Re-fetch to get the stored ID (the Ctx we built is a copy; AddViewModel stores it).
		const FMVVMBlueprintViewModelContext* Stored = View->FindViewModel(VMFName);
		FGuid ViewModelId = Stored ? Stored->GetViewModelId() : Ctx.GetViewModelId();

		// ── Compile ─────────────────────────────────────────────────────────
		TSharedRef<FJsonObject> CompileObj = RunCompileAndCollect(WBP);

		// ── Result ──────────────────────────────────────────────────────────
		const FString CreationTypeName =
			CreationTypeToken.IsEmpty() ? TEXT("manual") : CreationTypeToken.ToLower();

		TSharedRef<FJsonObject> Result = MakeShared<FJsonObject>();
		Result->SetBoolField(TEXT("added"), true);
		Result->SetStringField(TEXT("viewmodel_id"), ViewModelId.ToString());
		Result->SetStringField(TEXT("name"), VMName);
		Result->SetStringField(TEXT("creation_type"), CreationTypeName);
		Result->SetObjectField(TEXT("compile"), CompileObj);
		return FAgentMcpToolResult::Success(ToolUtils::SerializeObject(Result));
	}

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
			if (!WidgetField && !WidgetFunc)
			{
				return FAgentMcpToolResult::Error(FString::Printf(
					TEXT("Widget property or function '%s' not found on widget class '%s'. "
						"Verify the property name (e.g. 'Text' for TextBlock, 'Value' for SpinBox)."),
					*WidgetPropName, *WidgetClass->GetName()));
			}
		}
		if (!bIsSelf && !WidgetField && !WidgetFunc)
		{
			// Already handled above, but be safe.
			return FAgentMcpToolResult::Error(FString::Printf(
				TEXT("Widget property or function '%s' not found on widget '%s'."), *WidgetPropName, *WidgetNameStr));
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

		// ── ALL inputs validated — NOW call AddBinding ──────────────────────
		// Using validate-first design: we never call AddBinding before this point,
		// so there is no dangling blank binding to clean up on error.
		FMVVMBlueprintViewBinding& Binding = Sub->AddBinding(WBP);
		// Capture the ID immediately — AddBinding returns a reference into a TArray;
		// any subsequent TArray mutation (including inside Set* calls that may call AddBinding
		// internally) could invalidate the reference. We capture the ID so we can re-fetch later.
		const FGuid BindingId = Binding.BindingId;

		// ── Build source path (viewmodel → widget direction) ────────────────
		{
			FMVVMBlueprintPropertyPath Src;
			Src.SetViewModelId(VMId);
			if (VMField)
			{
				Src.SetPropertyPath(WBP, UE::MVVM::FMVVMConstFieldVariant(VMField));
			}
			else
			{
				Src.SetPropertyPath(WBP, UE::MVVM::FMVVMConstFieldVariant(VMFunc));
			}
			// SetSourcePathForBinding opens its own FScopedTransaction internally.
			Sub->SetSourcePathForBinding(WBP, Binding, Src);
		}

		// Re-fetch binding by ID — the TArray may have been re-allocated by SetSourcePathForBinding.
		{
			FMVVMBlueprintViewBinding* BindingPtr = View->GetBinding(BindingId);
			if (!BindingPtr)
			{
				// Should never happen, but guard defensively.
				return FAgentMcpToolResult::Error(TEXT("Binding was lost after SetSourcePath call. This is an engine-internal error."));
			}

			// ── Build destination path (widget side) ────────────────────────
			FMVVMBlueprintPropertyPath Dst;
			Dst.ResetPropertyPath();
			if (WidgetField)
			{
				Dst.AppendPropertyPath(WBP, UE::MVVM::FMVVMConstFieldVariant(WidgetField));
			}
			else if (WidgetFunc)
			{
				Dst.AppendPropertyPath(WBP, UE::MVVM::FMVVMConstFieldVariant(WidgetFunc));
			}

			if (bIsSelf)
			{
				Dst.SetSelfContext();
			}
			else
			{
				Dst.SetWidgetName(WidgetFName);
			}

			Sub->SetDestinationPathForBinding(WBP, *BindingPtr, Dst, /*bAllowEventConversion=*/false);
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

		// ── Compile ─────────────────────────────────────────────────────────
		TSharedRef<FJsonObject> CompileObj = RunCompileAndCollect(WBP);

		// ── Result ──────────────────────────────────────────────────────────
		TSharedRef<FJsonObject> Result = MakeShared<FJsonObject>();
		Result->SetBoolField(TEXT("bound"), true);
		Result->SetStringField(TEXT("binding_id"), BindingId.ToString());
		Result->SetObjectField(TEXT("compile"), CompileObj);
		return FAgentMcpToolResult::Success(ToolUtils::SerializeObject(Result));
	}

} // anonymous namespace

// ─────────────────────────────────────────────────────────────────────────────
// Registration
// ─────────────────────────────────────────────────────────────────────────────

void AgentMcp::Tools::RegisterMvvmTools()
{
	// add_viewmodel
	{
		FAgentMcpToolDef Def;
		Def.Name = TEXT("add_viewmodel");
		Def.Description = TEXT(
			"Adds a viewmodel context to a WidgetBlueprint's MVVM view extension. "
			"The viewmodel_class MUST implement INotifyFieldValueChanged (use a UMVVMViewModelBase subclass). "
			"Args: blueprint_path (req), viewmodel_class (req — full path e.g. /Script/ModelViewViewModel.MVVMViewModelBase), "
			"name (req — unique name for this VM instance), creation_type (opt: manual|create_instance|global|property_path|resolver, default manual). "
			"Returns {added, viewmodel_id, name, creation_type, compile:{status, num_errors, num_warnings}}.");
		Def.InputSchema = MakeShared<FJsonObject>();
		Def.InputSchema->SetStringField(TEXT("type"), TEXT("object"));
		{
			TSharedRef<FJsonObject> Properties = MakeShared<FJsonObject>();

			TSharedRef<FJsonObject> BpPath = MakeShared<FJsonObject>();
			BpPath->SetStringField(TEXT("type"), TEXT("string"));
			BpPath->SetStringField(TEXT("description"), TEXT("Absolute asset path to the WidgetBlueprint, e.g. /Game/UI/WBP_MainMenu"));
			Properties->SetObjectField(TEXT("blueprint_path"), BpPath);

			TSharedRef<FJsonObject> VMClass = MakeShared<FJsonObject>();
			VMClass->SetStringField(TEXT("type"), TEXT("string"));
			VMClass->SetStringField(TEXT("description"), TEXT("Full /Script/ path to the viewmodel class (must implement INotifyFieldValueChanged), e.g. /Script/ModelViewViewModel.MVVMViewModelBase"));
			Properties->SetObjectField(TEXT("viewmodel_class"), VMClass);

			TSharedRef<FJsonObject> VMName = MakeShared<FJsonObject>();
			VMName->SetStringField(TEXT("type"), TEXT("string"));
			VMName->SetStringField(TEXT("description"), TEXT("Unique name for this viewmodel instance within the WidgetBlueprint, e.g. MainMenuVM"));
			Properties->SetObjectField(TEXT("name"), VMName);

			TSharedRef<FJsonObject> CreationType = MakeShared<FJsonObject>();
			CreationType->SetStringField(TEXT("type"), TEXT("string"));
			CreationType->SetStringField(TEXT("description"), TEXT("How the viewmodel instance is obtained at runtime: manual (default), create_instance, global, property_path, resolver."));
			Properties->SetObjectField(TEXT("creation_type"), CreationType);

			Def.InputSchema->SetObjectField(TEXT("properties"), Properties);

			TArray<TSharedPtr<FJsonValue>> Required;
			Required.Add(MakeShared<FJsonValueString>(TEXT("blueprint_path")));
			Required.Add(MakeShared<FJsonValueString>(TEXT("viewmodel_class")));
			Required.Add(MakeShared<FJsonValueString>(TEXT("name")));
			Def.InputSchema->SetArrayField(TEXT("required"), Required);
		}
		Def.Tier = EAgentMcpTier::SafeWrite;
		Def.Handler = FAgentMcpToolHandler::CreateStatic(&HandleAddViewModel);
		FAgentMcpToolRegistry::Get().Register(MoveTemp(Def));
	}

	// add_view_binding
	{
		FAgentMcpToolDef Def;
		Def.Name = TEXT("add_view_binding");
		Def.Description = TEXT(
			"Adds an MVVM view binding between a viewmodel property and a widget property in a WidgetBlueprint. "
			"Uses validate-first design: all inputs are validated before any binding row is created, so no dangling "
			"blank bindings are left behind on error. "
			"IMPORTANT: The viewmodel must be added first with add_viewmodel. "
			"IMPORTANT: viewmodel_property must be a FieldNotify-enabled property — the engine silently no-ops on invalid paths; this tool surfaces that as an explicit error. "
			"Args: blueprint_path (req), widget_name (req — use list_widgets), widget_property (req — e.g. 'Text'), "
			"viewmodel_name (req — name used in add_viewmodel), viewmodel_property (req — property name on the VM class), "
			"direction (opt: one_way (default, VM→widget), two_way, one_time). "
			"Returns {bound, binding_id, compile:{status, num_errors, num_warnings}}.");
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
			Direction->SetStringField(TEXT("description"), TEXT("Binding direction: one_way (VM→widget, default), two_way, one_time."));
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
}
