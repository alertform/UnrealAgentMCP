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
#include "NiagaraComponent.h"
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
	}
}
