#include "Tools/NiagaraTools.h"

#include "AssetToolsModule.h"
#include "Core/McpTypes.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "Editor.h"
#include "GameFramework/Actor.h"
#include "IAssetTools.h"
#include "Factories/Factory.h"
#include "Misc/PackageName.h"
#include "Modules/ModuleManager.h"
#include "Materials/MaterialInterface.h"
#include "NiagaraComponent.h"
#include "NiagaraEditorUtilities.h"
#include "NiagaraEmitter.h"
#include "NiagaraEmitterHandle.h"
#include "NiagaraScript.h"
#include "NiagaraSpriteRendererProperties.h"
#include "NiagaraSystem.h"
#include "NiagaraTypes.h"
#include "Tools/McpToolUtils.h"
#include "UObject/UObjectGlobals.h"

// M3 Niagara N1 family: system creation (UNiagaraSystemFactoryNew, empty-system branch),
// exposed user parameters (FNiagaraUserRedirectionParameterStore — the officially scriptable
// authoring surface), and component placement. Emitter/module graph authoring is deliberately
// NOT here (no stable public API; see the design spec §6.3) — M4 gates add_niagara_emitter
// behind its own persistence spike.

namespace
{
	using namespace AgentMcp;

	UNiagaraSystem* ResolveNiagaraSystem(const FString& Path, FString& OutError)
	{
		UNiagaraSystem* Sys = FindObject<UNiagaraSystem>(nullptr, *Path);
		if (!Sys)
		{
			FString Pkg = Path;
			int32 Dot = INDEX_NONE;
			if (Pkg.FindChar(TEXT('.'), Dot)) { Pkg = Pkg.Left(Dot); }
			const FString ObjPath = Pkg + TEXT(".") + FPackageName::GetShortName(Pkg);
			Sys = FindObject<UNiagaraSystem>(nullptr, *ObjPath);
			if (!Sys) { Sys = LoadObject<UNiagaraSystem>(nullptr, *ObjPath); }
		}
		if (!Sys)
		{
			OutError = FString::Printf(
				TEXT("NiagaraSystem not found: '%s'. Use search_assets with class filter NiagaraSystem."), *Path);
		}
		return Sys;
	}

	/** "Glow" → "User.Glow"; leaves an existing "User." prefix alone. */
	FString NormalizeUserParamName(const FString& InName)
	{
		return InName.StartsWith(TEXT("User.")) ? InName : TEXT("User.") + InName;
	}

	UNiagaraEmitter* ResolveNiagaraEmitter(const FString& Path, FString& OutError)
	{
		UNiagaraEmitter* Em = FindObject<UNiagaraEmitter>(nullptr, *Path);
		if (!Em)
		{
			FString Pkg = Path;
			int32 Dot = INDEX_NONE;
			if (Pkg.FindChar(TEXT('.'), Dot)) { Pkg = Pkg.Left(Dot); }
			const FString ObjPath = Pkg + TEXT(".") + FPackageName::GetShortName(Pkg);
			Em = FindObject<UNiagaraEmitter>(nullptr, *ObjPath);
			if (!Em) { Em = LoadObject<UNiagaraEmitter>(nullptr, *ObjPath); }
		}
		if (!Em)
		{
			OutError = FString::Printf(
				TEXT("NiagaraEmitter not found: '%s'. Engine templates live under /Niagara/DefaultAssets/Templates/Emitters (e.g. Fountain)."), *Path);
		}
		return Em;
	}

	FAgentMcpToolResult HandleCreateNiagaraSystem(const TSharedPtr<FJsonObject>& Args)
	{
		if (!Args) { return FAgentMcpToolResult::Error(TEXT("Missing arguments.")); }
		FString Name, Dest;
		if (!Args->TryGetStringField(TEXT("name"), Name))
			{ return FAgentMcpToolResult::Error(TEXT("Missing required 'name'.")); }
		if (!Args->TryGetStringField(TEXT("destination_path"), Dest))
			{ return FAgentMcpToolResult::Error(TEXT("Missing required 'destination_path'.")); }
		if (!Dest.StartsWith(TEXT("/Game")))
			{ return FAgentMcpToolResult::Error(TEXT("destination_path must be under /Game.")); }

		// UNiagaraSystemFactoryNew carries no NIAGARAEDITOR_API export — construct it through
		// the reflection registry instead of linking GetPrivateStaticClass directly.
		UClass* FactoryClass = FindObject<UClass>(nullptr, TEXT("/Script/NiagaraEditor.NiagaraSystemFactoryNew"));
		if (!FactoryClass) { FactoryClass = LoadObject<UClass>(nullptr, TEXT("/Script/NiagaraEditor.NiagaraSystemFactoryNew")); }
		if (!FactoryClass)
			{ return FAgentMcpToolResult::Error(TEXT("NiagaraSystemFactoryNew class not found — is the NiagaraEditor module loaded?")); }

		IAssetTools& AssetTools = FModuleManager::LoadModuleChecked<FAssetToolsModule>(TEXT("AssetTools")).Get();
		UFactory* Factory = NewObject<UFactory>(GetTransientPackage(), FactoryClass);
		UObject* NewAsset = AssetTools.CreateAsset(Name, Dest, UNiagaraSystem::StaticClass(), Factory);
		UNiagaraSystem* Sys = Cast<UNiagaraSystem>(NewAsset);
		if (!Sys)
			{ return FAgentMcpToolResult::Error(FString::Printf(TEXT("CreateAsset failed for '%s' in '%s' (name taken or invalid path?)."), *Name, *Dest)); }

		// The factory kicks an ASYNC RequestCompile; a background compile task outliving the
		// tool call crashed a headless run when a later delete_asset freed the system under it
		// (SparseArray !AllocationFlags assert on a TargetPlatform worker). Block until settled
		// so the tool hands back a quiescent asset — agent tools must not leak background work.
		Sys->WaitForCompilationComplete(/*bIncludingGPUShaders=*/true, /*bShowProgress=*/false);

		const TSharedRef<FJsonObject> Out = MakeShared<FJsonObject>();
		Out->SetStringField(TEXT("object_path"), Sys->GetPathName());
		Out->SetStringField(TEXT("package_path"), Dest / Name);
		Out->SetStringField(TEXT("note"), TEXT("Created empty (no emitters). Expose parameters with set_niagara_user_parameter; place with place_niagara_component."));
		return FAgentMcpToolResult::Success(ToolUtils::SerializeObject(Out));
	}

	FAgentMcpToolResult HandleSetNiagaraUserParameter(const TSharedPtr<FJsonObject>& Args)
	{
		if (!Args) { return FAgentMcpToolResult::Error(TEXT("Missing arguments.")); }
		FString SysPath, ParamName, Type, Value;
		if (!Args->TryGetStringField(TEXT("system"), SysPath))
			{ return FAgentMcpToolResult::Error(TEXT("Missing required 'system'.")); }
		if (!Args->TryGetStringField(TEXT("name"), ParamName))
			{ return FAgentMcpToolResult::Error(TEXT("Missing required 'name'.")); }
		if (!Args->TryGetStringField(TEXT("type"), Type))
			{ return FAgentMcpToolResult::Error(TEXT("Missing required 'type' (float/vec2/vec3/linearcolor/bool/int).")); }
		if (!Args->TryGetStringField(TEXT("value"), Value))
			{ return FAgentMcpToolResult::Error(TEXT("Missing required 'value'.")); }

		FString Err;
		UNiagaraSystem* Sys = ResolveNiagaraSystem(SysPath, Err);
		if (!Sys) { return FAgentMcpToolResult::Error(Err); }

		const FName FullName(*NormalizeUserParamName(ParamName));
		FNiagaraUserRedirectionParameterStore& Store = Sys->GetExposedParameters();
		bool bSet = false;

		if (Type.Equals(TEXT("float"), ESearchCase::IgnoreCase))
		{
			bSet = Store.SetParameterValue(FCString::Atof(*Value),
				FNiagaraVariable(FNiagaraTypeDefinition::GetFloatDef(), FullName), /*bAdd=*/true);
		}
		else if (Type.Equals(TEXT("int"), ESearchCase::IgnoreCase))
		{
			bSet = Store.SetParameterValue(static_cast<int32>(FCString::Atoi(*Value)),
				FNiagaraVariable(FNiagaraTypeDefinition::GetIntDef(), FullName), /*bAdd=*/true);
		}
		else if (Type.Equals(TEXT("bool"), ESearchCase::IgnoreCase))
		{
			FNiagaraBool B;
			B.SetValue(Value.ToBool());
			bSet = Store.SetParameterValue(B,
				FNiagaraVariable(FNiagaraTypeDefinition::GetBoolDef(), FullName), /*bAdd=*/true);
		}
		else if (Type.Equals(TEXT("linearcolor"), ESearchCase::IgnoreCase))
		{
			FLinearColor Color;
			if (!Color.InitFromString(Value))
				{ return FAgentMcpToolResult::Error(FString::Printf(TEXT("Could not parse linearcolor '%s'. Use (R=..,G=..,B=..,A=..)."), *Value)); }
			bSet = Store.SetParameterValue(Color,
				FNiagaraVariable(FNiagaraTypeDefinition::GetColorDef(), FullName), /*bAdd=*/true);
		}
		else if (Type.Equals(TEXT("vec3"), ESearchCase::IgnoreCase))
		{
			FVector V;
			if (!V.InitFromString(Value))
				{ return FAgentMcpToolResult::Error(FString::Printf(TEXT("Could not parse vec3 '%s'. Use (X=..,Y=..,Z=..)."), *Value)); }
			bSet = Store.SetParameterValue(FVector3f(V),
				FNiagaraVariable(FNiagaraTypeDefinition::GetVec3Def(), FullName), /*bAdd=*/true);
		}
		else if (Type.Equals(TEXT("vec2"), ESearchCase::IgnoreCase))
		{
			FVector2D V;
			if (!V.InitFromString(Value))
				{ return FAgentMcpToolResult::Error(FString::Printf(TEXT("Could not parse vec2 '%s'. Use (X=..,Y=..)."), *Value)); }
			bSet = Store.SetParameterValue(FVector2f(V),
				FNiagaraVariable(FNiagaraTypeDefinition::GetVec2Def(), FullName), /*bAdd=*/true);
		}
		else
		{
			return FAgentMcpToolResult::Error(FString::Printf(TEXT("Unknown type '%s'. Use float/vec2/vec3/linearcolor/bool/int."), *Type));
		}

		if (!bSet)
			{ return FAgentMcpToolResult::Error(FString::Printf(TEXT("SetParameterValue failed for '%s' (type mismatch with an existing parameter?)."), *FullName.ToString())); }
		Sys->MarkPackageDirty();

		const TSharedRef<FJsonObject> Out = MakeShared<FJsonObject>();
		Out->SetBoolField(TEXT("ok"), true);
		Out->SetStringField(TEXT("parameter"), FullName.ToString());
		return FAgentMcpToolResult::Success(ToolUtils::SerializeObject(Out));
	}

	FAgentMcpToolResult HandleDescribeNiagaraSystem(const TSharedPtr<FJsonObject>& Args)
	{
		if (!Args) { return FAgentMcpToolResult::Error(TEXT("Missing arguments.")); }
		FString SysPath;
		if (!Args->TryGetStringField(TEXT("system"), SysPath))
			{ return FAgentMcpToolResult::Error(TEXT("Missing required 'system'.")); }
		FString Err;
		UNiagaraSystem* Sys = ResolveNiagaraSystem(SysPath, Err);
		if (!Sys) { return FAgentMcpToolResult::Error(Err); }

		FNiagaraUserRedirectionParameterStore& Store = Sys->GetExposedParameters();
		TArray<FNiagaraVariable> Vars;
		Store.GetUserParameters(Vars);

		TArray<TSharedPtr<FJsonValue>> Params;
		for (const FNiagaraVariable& Var : Vars)
		{
			const TSharedRef<FJsonObject> P = MakeShared<FJsonObject>();
			P->SetStringField(TEXT("name"), Var.GetName().ToString());
			P->SetStringField(TEXT("type"), Var.GetType().GetName());

			// Stale-value warning in the store header: read through GetParameterValue, never Var's data.
			const FNiagaraTypeDefinition& T = Var.GetType();
			if (T == FNiagaraTypeDefinition::GetFloatDef())
			{
				P->SetNumberField(TEXT("value"), Store.GetParameterValue<float>(Var));
			}
			else if (T == FNiagaraTypeDefinition::GetIntDef())
			{
				P->SetNumberField(TEXT("value"), Store.GetParameterValue<int32>(Var));
			}
			else if (T == FNiagaraTypeDefinition::GetBoolDef())
			{
				P->SetBoolField(TEXT("value"), Store.GetParameterValue<FNiagaraBool>(Var).GetValue());
			}
			else if (T == FNiagaraTypeDefinition::GetColorDef())
			{
				P->SetStringField(TEXT("value"), Store.GetParameterValue<FLinearColor>(Var).ToString());
			}
			else if (T == FNiagaraTypeDefinition::GetVec3Def())
			{
				P->SetStringField(TEXT("value"), FVector(Store.GetParameterValue<FVector3f>(Var)).ToString());
			}
			else if (T == FNiagaraTypeDefinition::GetVec2Def())
			{
				P->SetStringField(TEXT("value"), FVector2D(Store.GetParameterValue<FVector2f>(Var)).ToString());
			}
			Params.Add(MakeShared<FJsonValueObject>(P));
		}

		// Emitter handles (names only — authoring them is out of N1 scope).
		TArray<TSharedPtr<FJsonValue>> Emitters;
		for (const FNiagaraEmitterHandle& Handle : Sys->GetEmitterHandles())
		{
			Emitters.Add(MakeShared<FJsonValueString>(Handle.GetName().ToString()));
		}

		const TSharedRef<FJsonObject> Out = MakeShared<FJsonObject>();
		Out->SetStringField(TEXT("system"), Sys->GetPathName());
		Out->SetArrayField(TEXT("user_parameters"), Params);
		Out->SetArrayField(TEXT("emitters"), Emitters);
		return FAgentMcpToolResult::Success(ToolUtils::SerializeObject(Out));
	}

	FAgentMcpToolResult HandlePlaceNiagaraComponent(const TSharedPtr<FJsonObject>& Args)
	{
		if (!Args) { return FAgentMcpToolResult::Error(TEXT("Missing arguments.")); }
		FString ActorPath, SysPath, CompName;
		if (!Args->TryGetStringField(TEXT("actor_path"), ActorPath))
			{ return FAgentMcpToolResult::Error(TEXT("Missing required 'actor_path' (from spawn_actor/query_actors).")); }
		if (!Args->TryGetStringField(TEXT("system"), SysPath))
			{ return FAgentMcpToolResult::Error(TEXT("Missing required 'system'.")); }
		Args->TryGetStringField(TEXT("component_name"), CompName);

		AActor* Actor = FindObject<AActor>(nullptr, *ActorPath);
		if (!Actor || !IsValid(Actor))
			{ return FAgentMcpToolResult::Error(FString::Printf(TEXT("Actor not found: '%s'. Call query_actors for live paths."), *ActorPath)); }
		UWorld* EditorWorld = GEditor ? GEditor->GetEditorWorldContext().World() : nullptr;
		if (!EditorWorld || Actor->GetWorld() != EditorWorld)
			{ return FAgentMcpToolResult::Error(FString::Printf(TEXT("Actor '%s' is not in the editor world (PIE/preview actor?)."), *ActorPath)); }

		FString Err;
		UNiagaraSystem* Sys = ResolveNiagaraSystem(SysPath, Err);
		if (!Sys) { return FAgentMcpToolResult::Error(Err); }

		const FName NewName = CompName.IsEmpty()
			? MakeUniqueObjectName(Actor, UNiagaraComponent::StaticClass(), TEXT("NiagaraComponent"))
			: FName(*CompName);
		UNiagaraComponent* Comp = NewObject<UNiagaraComponent>(Actor, NewName);
		if (!Comp) { return FAgentMcpToolResult::Error(TEXT("Failed to create UNiagaraComponent.")); }

		Actor->AddInstanceComponent(Comp);
		if (USceneComponent* Root = Actor->GetRootComponent())
		{
			Comp->AttachToComponent(Root, FAttachmentTransformRules::KeepRelativeTransform);
		}
		else
		{
			Actor->SetRootComponent(Comp);
		}
		Comp->RegisterComponent();
		Comp->SetAsset(Sys);
		Actor->MarkPackageDirty();

		const TSharedRef<FJsonObject> Out = MakeShared<FJsonObject>();
		Out->SetBoolField(TEXT("ok"), true);
		Out->SetStringField(TEXT("component"), Comp->GetName());
		Out->SetStringField(TEXT("system"), Sys->GetPathName());
		return FAgentMcpToolResult::Success(ToolUtils::SerializeObject(Out));
	}

	// M4: emitter assembly from a source emitter asset. Uses the exported static
	// FNiagaraEditorUtilities::AddEmitterToSystem (the same call UNiagaraSystemFactoryNew
	// uses to seed systems from emitters) — NOT the editor ViewModel path the design
	// flagged as fragile, so this landed as a real API call rather than best-effort.
	FAgentMcpToolResult HandleAddNiagaraEmitter(const TSharedPtr<FJsonObject>& Args)
	{
		if (!Args) { return FAgentMcpToolResult::Error(TEXT("Missing arguments.")); }
		FString SysPath, EmitterPath;
		if (!Args->TryGetStringField(TEXT("system"), SysPath))
			{ return FAgentMcpToolResult::Error(TEXT("Missing required 'system'.")); }
		if (!Args->TryGetStringField(TEXT("source_emitter"), EmitterPath))
			{ return FAgentMcpToolResult::Error(TEXT("Missing required 'source_emitter' (a UNiagaraEmitter asset, e.g. /Niagara/DefaultAssets/Templates/Emitters/Fountain).")); }

		FString Err;
		UNiagaraSystem* Sys = ResolveNiagaraSystem(SysPath, Err);
		if (!Sys) { return FAgentMcpToolResult::Error(Err); }
		UNiagaraEmitter* Emitter = ResolveNiagaraEmitter(EmitterPath, Err);
		if (!Emitter) { return FAgentMcpToolResult::Error(Err); }

		const FGuid Version = Emitter->GetExposedVersion().VersionGuid;
		const FGuid HandleId = FNiagaraEditorUtilities::AddEmitterToSystem(
			*Sys, *Emitter, Version, /*bCreateCopy=*/true);

		// Same quiescence contract as create_niagara_system: the add re-requests
		// compilation — block so no background task outlives the tool call.
		Sys->WaitForCompilationComplete(/*bIncludingGPUShaders=*/true, /*bShowProgress=*/false);
		Sys->MarkPackageDirty();

		FString HandleName;
		for (const FNiagaraEmitterHandle& Handle : Sys->GetEmitterHandles())
		{
			if (Handle.GetId() == HandleId) { HandleName = Handle.GetName().ToString(); break; }
		}
		if (HandleName.IsEmpty())
			{ return FAgentMcpToolResult::Error(TEXT("AddEmitterToSystem returned no matching emitter handle — the add did not take.")); }

		const TSharedRef<FJsonObject> Out = MakeShared<FJsonObject>();
		Out->SetBoolField(TEXT("ok"), true);
		Out->SetStringField(TEXT("emitter_handle"), HandleName);
		Out->SetStringField(TEXT("handle_id"), HandleId.ToString());
		Out->SetNumberField(TEXT("emitter_count"), Sys->GetEmitterHandles().Num());
		return FAgentMcpToolResult::Success(ToolUtils::SerializeObject(Out));
	}

	// -----------------------------------------------------------------------
	// N2: module-input editing via Rapid Iteration parameters. Module inputs a
	// template author touched live as "Constants.<...>.<Module>.<Input>" entries
	// in the owning script's RapidIterationParameters store — runtime reads the
	// store directly (that is what makes them "rapid"), so writing the store +
	// saving the asset changes the effect with NO graph surgery and NO recompile.
	// Inputs still at script default have no store entry: list shows what IS
	// editable; set edits existing entries only (bAdd would desync the graph).
	// -----------------------------------------------------------------------

	void GatherRapidIterationScripts(UNiagaraSystem* Sys,
		TArray<TPair<FString, UNiagaraScript*>>& Out)
	{
		if (UNiagaraScript* S = Sys->GetSystemSpawnScript()) { Out.Emplace(TEXT("System"), S); }
		if (UNiagaraScript* S = Sys->GetSystemUpdateScript()) { Out.Emplace(TEXT("System"), S); }
		for (const FNiagaraEmitterHandle& Handle : Sys->GetEmitterHandles())
		{
			if (FVersionedNiagaraEmitterData* Data = Handle.GetEmitterData())
			{
				TArray<UNiagaraScript*> Scripts;
				Data->GetScripts(Scripts, /*bCompilableOnly=*/false);
				for (UNiagaraScript* S : Scripts)
				{
					if (S) { Out.Emplace(Handle.GetName().ToString(), S); }
				}
			}
		}
	}

	/** Storage bytes of float/int/bool/linearcolor/vec3/vec2 -> JSON value; other types marked unsupported. */
	void WriteRapidIterationValue(const FNiagaraTypeDefinition& Type, const uint8* Data,
		const TSharedRef<FJsonObject>& Obj)
	{
		if (!Data) { Obj->SetStringField(TEXT("value"), TEXT("<no data>")); return; }
		if (Type == FNiagaraTypeDefinition::GetFloatDef())
		{
			float V; FMemory::Memcpy(&V, Data, sizeof(V));
			Obj->SetNumberField(TEXT("value"), V);
		}
		else if (Type == FNiagaraTypeDefinition::GetIntDef())
		{
			int32 V; FMemory::Memcpy(&V, Data, sizeof(V));
			Obj->SetNumberField(TEXT("value"), V);
		}
		else if (Type == FNiagaraTypeDefinition::GetBoolDef())
		{
			FNiagaraBool V; FMemory::Memcpy(&V, Data, sizeof(V));
			Obj->SetBoolField(TEXT("value"), V.GetValue());
		}
		else if (Type == FNiagaraTypeDefinition::GetColorDef())
		{
			FLinearColor V; FMemory::Memcpy(&V, Data, sizeof(V));
			Obj->SetStringField(TEXT("value"), V.ToString());
		}
		else if (Type == FNiagaraTypeDefinition::GetVec3Def())
		{
			FVector3f V; FMemory::Memcpy(&V, Data, sizeof(V));
			Obj->SetStringField(TEXT("value"), FVector(V).ToString());
		}
		else if (Type == FNiagaraTypeDefinition::GetVec2Def())
		{
			FVector2f V; FMemory::Memcpy(&V, Data, sizeof(V));
			Obj->SetStringField(TEXT("value"), FVector2D(V).ToString());
		}
		else
		{
			Obj->SetStringField(TEXT("value"), FString::Printf(TEXT("<unsupported:%s>"), *Type.GetName()));
		}
	}

	FAgentMcpToolResult HandleListNiagaraModuleInputs(const TSharedPtr<FJsonObject>& Args)
	{
		if (!Args) { return FAgentMcpToolResult::Error(TEXT("Missing arguments.")); }
		FString SysPath;
		if (!Args->TryGetStringField(TEXT("system"), SysPath))
			{ return FAgentMcpToolResult::Error(TEXT("Missing required 'system'.")); }
		FString Err;
		UNiagaraSystem* Sys = ResolveNiagaraSystem(SysPath, Err);
		if (!Sys) { return FAgentMcpToolResult::Error(Err); }

		TArray<TPair<FString, UNiagaraScript*>> Scripts;
		GatherRapidIterationScripts(Sys, Scripts);

		TArray<TSharedPtr<FJsonValue>> Inputs;
		for (const TPair<FString, UNiagaraScript*>& Entry : Scripts)
		{
			FNiagaraParameterStore& Store = Entry.Value->RapidIterationParameters;
			TArray<FNiagaraVariable> Vars;
			Store.GetParameters(Vars);
			for (const FNiagaraVariable& Var : Vars)
			{
				const TSharedRef<FJsonObject> P = MakeShared<FJsonObject>();
				P->SetStringField(TEXT("emitter"), Entry.Key);
				P->SetStringField(TEXT("name"), Var.GetName().ToString());
				P->SetStringField(TEXT("type"), Var.GetType().GetName());
				WriteRapidIterationValue(Var.GetType(), Store.GetParameterData(Var), P);
				Inputs.Add(MakeShared<FJsonValueObject>(P));
			}
		}

		const TSharedRef<FJsonObject> Out = MakeShared<FJsonObject>();
		Out->SetStringField(TEXT("system"), Sys->GetPathName());
		Out->SetNumberField(TEXT("count"), Inputs.Num());
		Out->SetArrayField(TEXT("module_inputs"), Inputs);
		return FAgentMcpToolResult::Success(ToolUtils::SerializeObject(Out));
	}

	FAgentMcpToolResult HandleSetNiagaraModuleInput(const TSharedPtr<FJsonObject>& Args)
	{
		if (!Args) { return FAgentMcpToolResult::Error(TEXT("Missing arguments.")); }
		FString SysPath, ParamQuery, Value;
		if (!Args->TryGetStringField(TEXT("system"), SysPath))
			{ return FAgentMcpToolResult::Error(TEXT("Missing required 'system'.")); }
		if (!Args->TryGetStringField(TEXT("parameter"), ParamQuery))
			{ return FAgentMcpToolResult::Error(TEXT("Missing required 'parameter' (name from list_niagara_module_inputs; suffix match allowed).")); }
		if (!Args->TryGetStringField(TEXT("value"), Value))
			{ return FAgentMcpToolResult::Error(TEXT("Missing required 'value'.")); }

		FString Err;
		UNiagaraSystem* Sys = ResolveNiagaraSystem(SysPath, Err);
		if (!Sys) { return FAgentMcpToolResult::Error(Err); }

		TArray<TPair<FString, UNiagaraScript*>> Scripts;
		GatherRapidIterationScripts(Sys, Scripts);

		// Match ladder: exact name → ".query" suffix → substring. The same input may
		// legitimately live in several scripts (spawn+update copies) — that is ONE
		// logical input; genuinely different names matching the query is ambiguity.
		TSet<FString> MatchedNames;
		TArray<TPair<UNiagaraScript*, FNiagaraVariable>> Targets;
		auto CollectMatches = [&](auto Predicate)
		{
			MatchedNames.Reset();
			Targets.Reset();
			for (const TPair<FString, UNiagaraScript*>& Entry : Scripts)
			{
				TArray<FNiagaraVariable> Vars;
				Entry.Value->RapidIterationParameters.GetParameters(Vars);
				for (const FNiagaraVariable& Var : Vars)
				{
					const FString VarName = Var.GetName().ToString();
					if (Predicate(VarName))
					{
						MatchedNames.Add(VarName);
						Targets.Emplace(Entry.Value, Var);
					}
				}
			}
		};

		CollectMatches([&](const FString& N) { return N.Equals(ParamQuery, ESearchCase::IgnoreCase); });
		if (MatchedNames.Num() == 0)
		{
			CollectMatches([&](const FString& N) { return N.EndsWith(TEXT(".") + ParamQuery, ESearchCase::IgnoreCase); });
		}
		if (MatchedNames.Num() == 0)
		{
			CollectMatches([&](const FString& N) { return N.Contains(ParamQuery, ESearchCase::IgnoreCase); });
		}

		if (MatchedNames.Num() == 0)
		{
			return FAgentMcpToolResult::Error(FString::Printf(
				TEXT("No rapid-iteration parameter matches '%s'. Call list_niagara_module_inputs for editable names (inputs still at script default have no entry)."), *ParamQuery));
		}
		if (MatchedNames.Num() > 1)
		{
			return FAgentMcpToolResult::Error(FString::Printf(
				TEXT("Ambiguous parameter '%s' — candidates: %s. Pass a longer/exact name."),
				*ParamQuery, *FString::Join(MatchedNames.Array(), TEXT(" | "))));
		}

		// Single logical input — write it in every script that carries a copy.
		int32 NumSet = 0;
		FString TypeName;
		for (const TPair<UNiagaraScript*, FNiagaraVariable>& Target : Targets)
		{
			const FNiagaraTypeDefinition& Type = Target.Value.GetType();
			TypeName = Type.GetName();
			FNiagaraParameterStore& Store = Target.Key->RapidIterationParameters;
			bool bSet = false;

			if (Type == FNiagaraTypeDefinition::GetFloatDef())
			{
				const float V = FCString::Atof(*Value);
				bSet = Store.SetParameterData(reinterpret_cast<const uint8*>(&V), Target.Value);
			}
			else if (Type == FNiagaraTypeDefinition::GetIntDef())
			{
				const int32 V = FCString::Atoi(*Value);
				bSet = Store.SetParameterData(reinterpret_cast<const uint8*>(&V), Target.Value);
			}
			else if (Type == FNiagaraTypeDefinition::GetBoolDef())
			{
				FNiagaraBool V; V.SetValue(Value.ToBool());
				bSet = Store.SetParameterData(reinterpret_cast<const uint8*>(&V), Target.Value);
			}
			else if (Type == FNiagaraTypeDefinition::GetColorDef())
			{
				FLinearColor V;
				if (!V.InitFromString(Value))
					{ return FAgentMcpToolResult::Error(FString::Printf(TEXT("Could not parse linearcolor '%s'. Use (R=..,G=..,B=..,A=..)."), *Value)); }
				bSet = Store.SetParameterData(reinterpret_cast<const uint8*>(&V), Target.Value);
			}
			else if (Type == FNiagaraTypeDefinition::GetVec3Def())
			{
				FVector Parsed;
				if (!Parsed.InitFromString(Value))
					{ return FAgentMcpToolResult::Error(FString::Printf(TEXT("Could not parse vec3 '%s'. Use (X=..,Y=..,Z=..)."), *Value)); }
				const FVector3f V(Parsed);
				bSet = Store.SetParameterData(reinterpret_cast<const uint8*>(&V), Target.Value);
			}
			else if (Type == FNiagaraTypeDefinition::GetVec2Def())
			{
				FVector2D Parsed;
				if (!Parsed.InitFromString(Value))
					{ return FAgentMcpToolResult::Error(FString::Printf(TEXT("Could not parse vec2 '%s'. Use (X=..,Y=..)."), *Value)); }
				const FVector2f V(Parsed);
				bSet = Store.SetParameterData(reinterpret_cast<const uint8*>(&V), Target.Value);
			}
			else
			{
				return FAgentMcpToolResult::Error(FString::Printf(
					TEXT("Parameter type '%s' is not editable by this tool (float/int/bool/linearcolor/vec3/vec2 only)."), *TypeName));
			}

			if (bSet) { ++NumSet; }
		}

		if (NumSet == 0)
			{ return FAgentMcpToolResult::Error(TEXT("SetParameterData failed on every matching script — store rejected the write.")); }
		Sys->MarkPackageDirty();

		const TSharedRef<FJsonObject> Out = MakeShared<FJsonObject>();
		Out->SetBoolField(TEXT("ok"), true);
		Out->SetStringField(TEXT("parameter"), MatchedNames.Array()[0]);
		Out->SetStringField(TEXT("type"), TypeName);
		Out->SetNumberField(TEXT("scripts_updated"), NumSet);
		Out->SetStringField(TEXT("note"), TEXT("Rapid-iteration value written; persist with save_asset."));
		return FAgentMcpToolResult::Success(ToolUtils::SerializeObject(Out));
	}

	// N2b: sprite-renderer material assignment. Doubles as diagnosis — reports what
	// each renderer carried BEFORE the write (renderer material is the layer that
	// silently defeats every particle/RI color edit when it is not what you assumed).
	FAgentMcpToolResult HandleSetNiagaraRendererMaterial(const TSharedPtr<FJsonObject>& Args)
	{
		if (!Args) { return FAgentMcpToolResult::Error(TEXT("Missing arguments.")); }
		FString SysPath, MatPath;
		if (!Args->TryGetStringField(TEXT("system"), SysPath))
			{ return FAgentMcpToolResult::Error(TEXT("Missing required 'system'.")); }
		if (!Args->TryGetStringField(TEXT("material"), MatPath))
			{ return FAgentMcpToolResult::Error(TEXT("Missing required 'material' (/Game path of a MaterialInterface).")); }

		FString Err;
		UNiagaraSystem* Sys = ResolveNiagaraSystem(SysPath, Err);
		if (!Sys) { return FAgentMcpToolResult::Error(Err); }

		UMaterialInterface* Mat = LoadObject<UMaterialInterface>(nullptr, *MatPath);
		if (!Mat)
		{
			FString Pkg = MatPath;
			int32 Dot = INDEX_NONE;
			if (Pkg.FindChar(TEXT('.'), Dot)) { Pkg = Pkg.Left(Dot); }
			Mat = LoadObject<UMaterialInterface>(nullptr, *(Pkg + TEXT(".") + FPackageName::GetShortName(Pkg)));
		}
		if (!Mat)
			{ return FAgentMcpToolResult::Error(FString::Printf(TEXT("MaterialInterface not found: '%s'."), *MatPath)); }

		TArray<TSharedPtr<FJsonValue>> Renderers;
		int32 NumSet = 0;
		for (const FNiagaraEmitterHandle& Handle : Sys->GetEmitterHandles())
		{
			FVersionedNiagaraEmitterData* Data = Handle.GetEmitterData();
			if (!Data) { continue; }
			for (UNiagaraRendererProperties* Props : Data->GetRenderers())
			{
				UNiagaraSpriteRendererProperties* Sprite = Cast<UNiagaraSpriteRendererProperties>(Props);
				if (!Sprite) { continue; }
				const TSharedRef<FJsonObject> R = MakeShared<FJsonObject>();
				R->SetStringField(TEXT("emitter"), Handle.GetName().ToString());
				R->SetStringField(TEXT("previous_material"),
					Sprite->Material ? Sprite->Material->GetPathName() : TEXT("None (engine default)"));
				Sprite->Modify();
				Sprite->Material = Mat;
				Sprite->PostEditChange();
				R->SetStringField(TEXT("new_material"), Mat->GetPathName());
				Renderers.Add(MakeShared<FJsonValueObject>(R));
				++NumSet;
			}
		}
		if (NumSet == 0)
			{ return FAgentMcpToolResult::Error(TEXT("No sprite renderers found on this system's emitters.")); }

		// Renderer swaps re-request compilation — same quiescence contract as the other tools.
		Sys->WaitForCompilationComplete(/*bIncludingGPUShaders=*/true, /*bShowProgress=*/false);
		Sys->MarkPackageDirty();

		const TSharedRef<FJsonObject> Out = MakeShared<FJsonObject>();
		Out->SetBoolField(TEXT("ok"), true);
		Out->SetNumberField(TEXT("renderers_updated"), NumSet);
		Out->SetArrayField(TEXT("renderers"), Renderers);
		return FAgentMcpToolResult::Success(ToolUtils::SerializeObject(Out));
	}
}

namespace AgentMcp::Tools
{
	void RegisterNiagaraTools()
	{
		using ToolUtils::RegisterOne;
		using ToolUtils::TypedProp;

		RegisterOne(TEXT("create_niagara_system"),
			TEXT("Creates an empty UNiagaraSystem asset under /Game (UNiagaraSystemFactoryNew; no emitters — emitter graph authoring has no stable public API). Returns {object_path, package_path, note}. Expose parameters with set_niagara_user_parameter; save with save_asset."),
			{ { TEXT("name"), TypedProp(TEXT("string"), TEXT("Asset name, e.g. NS_HitSparks.")) },
			  { TEXT("destination_path"), TypedProp(TEXT("string"), TEXT("/Game folder.")) } },
			{ TEXT("name"), TEXT("destination_path") }, &HandleCreateNiagaraSystem);

		RegisterOne(TEXT("set_niagara_user_parameter"),
			TEXT("Sets (adds if missing) a user-exposed parameter on a NiagaraSystem — the 'User Parameters' panel. name may omit the 'User.' prefix. type = float | int | bool | linearcolor '(R=..,G=..,B=..,A=..)' | vec3 '(X=..,Y=..,Z=..)' | vec2 '(X=..,Y=..)'. Returns {ok, parameter}."),
			{ { TEXT("system"), TypedProp(TEXT("string"), TEXT("/Game path of the NiagaraSystem.")) },
			  { TEXT("name"), TypedProp(TEXT("string"), TEXT("Parameter name, with or without the User. prefix.")) },
			  { TEXT("type"), TypedProp(TEXT("string"), TEXT("float | int | bool | linearcolor | vec3 | vec2.")) },
			  { TEXT("value"), TypedProp(TEXT("string"), TEXT("Value string; format depends on type.")) } },
			{ TEXT("system"), TEXT("name"), TEXT("type"), TEXT("value") }, &HandleSetNiagaraUserParameter);

		RegisterOne(TEXT("describe_niagara_system"),
			TEXT("Reads a NiagaraSystem's user-exposed parameters (name/type/value) and emitter handle names: {user_parameters:[{name,type,value}], emitters:[..]}. Use for verification."),
			{ { TEXT("system"), TypedProp(TEXT("string"), TEXT("/Game path of the NiagaraSystem.")) } },
			{ TEXT("system") }, &HandleDescribeNiagaraSystem);

		RegisterOne(TEXT("place_niagara_component"),
			TEXT("Adds a UNiagaraComponent to a placed editor-world actor (attached to its root, or becomes the root) and assigns the given NiagaraSystem. Returns {ok, component, system}."),
			{ { TEXT("actor_path"), TypedProp(TEXT("string"), TEXT("Actor object path from spawn_actor/query_actors.")) },
			  { TEXT("system"), TypedProp(TEXT("string"), TEXT("/Game path of the NiagaraSystem to assign.")) },
			  { TEXT("component_name"), TypedProp(TEXT("string"), TEXT("Component name (optional; auto-generated).")) } },
			{ TEXT("actor_path"), TEXT("system") }, &HandlePlaceNiagaraComponent);

		RegisterOne(TEXT("add_niagara_emitter"),
			TEXT("Adds an emitter to a NiagaraSystem by copying a source UNiagaraEmitter asset (engine templates: /Niagara/DefaultAssets/Templates/Emitters/Fountain, SimpleSpriteBurst, UpwardMeshBurst). The handle keeps the source emitter's name (dedup-suffixed). Blocks until the system recompiles. Returns {ok, emitter_handle, handle_id, emitter_count}."),
			{ { TEXT("system"), TypedProp(TEXT("string"), TEXT("/Game path of the NiagaraSystem.")) },
			  { TEXT("source_emitter"), TypedProp(TEXT("string"), TEXT("Path of the source UNiagaraEmitter asset to copy in.")) } },
			{ TEXT("system"), TEXT("source_emitter") }, &HandleAddNiagaraEmitter);

		RegisterOne(TEXT("list_niagara_module_inputs"),
			TEXT("Lists a NiagaraSystem's editable module inputs (rapid-iteration parameters) across system and emitter scripts: {module_inputs:[{emitter,name,type,value}]}. Names look like 'Constants.<Emitter>.<Module>.<Input>'. Inputs still at script default have no entry and are not editable via set_niagara_module_input."),
			{ { TEXT("system"), TypedProp(TEXT("string"), TEXT("/Game path of the NiagaraSystem.")) } },
			{ TEXT("system") }, &HandleListNiagaraModuleInputs);

		RegisterOne(TEXT("set_niagara_module_input"),
			TEXT("Sets an existing module input (rapid-iteration parameter) on a NiagaraSystem — e.g. a Color module's color or InitializeParticle sprite size. parameter accepts the exact name from list_niagara_module_inputs or an unambiguous suffix/substring. Value type is inferred from the parameter: float | int | bool | linearcolor '(R=..,G=..,B=..,A=..)' | vec3 '(X=..,Y=..,Z=..)' | vec2 '(X=..,Y=..)'. No recompile needed; persist with save_asset. Returns {ok, parameter, type, scripts_updated}."),
			{ { TEXT("system"), TypedProp(TEXT("string"), TEXT("/Game path of the NiagaraSystem.")) },
			  { TEXT("parameter"), TypedProp(TEXT("string"), TEXT("Name from list_niagara_module_inputs (suffix/substring OK if unambiguous).")) },
			  { TEXT("value"), TypedProp(TEXT("string"), TEXT("Value string; format follows the parameter's type.")) } },
			{ TEXT("system"), TEXT("parameter"), TEXT("value") }, &HandleSetNiagaraModuleInput);

		RegisterOne(TEXT("set_niagara_renderer_material"),
			TEXT("Assigns a MaterialInterface to every sprite renderer on a NiagaraSystem's emitters and reports each renderer's PREVIOUS material — the layer that silently defeats particle-color edits when a template/default material ignores vertex color. Blocks until recompiled; persist with save_asset. Returns {ok, renderers_updated, renderers:[{emitter, previous_material, new_material}]}."),
			{ { TEXT("system"), TypedProp(TEXT("string"), TEXT("/Game path of the NiagaraSystem.")) },
			  { TEXT("material"), TypedProp(TEXT("string"), TEXT("/Game path of the MaterialInterface to assign.")) } },
			{ TEXT("system"), TEXT("material") }, &HandleSetNiagaraRendererMaterial);
	}
}
