#include "Tools/MvvmTools.h"

#include "Core/AgentMcpToolRegistry.h"
#include "Core/McpTypes.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "INotifyFieldValueChanged.h"
#include "MVVMBlueprintView.h"
#include "MVVMBlueprintViewModelContext.h"
#include "MVVMEditorSubsystem.h"
#include "ScopedTransaction.h"
#include "Tools/McpToolUtils.h"
#include "Tools/MvvmToolsShared.h"

namespace
{
	using namespace AgentMcp;
	using namespace AgentMcp::MvvmShared;

	// ─────────────────────────────────────────────────────────────────────────
	// File-local helpers (used only by add_viewmodel)
	// ─────────────────────────────────────────────────────────────────────────

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
}
