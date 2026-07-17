#include "Misc/AutomationTest.h"

#if WITH_DEV_AUTOMATION_TESTS

#include "AgentMcpSettings.h"
#include "Core/AgentMcpTier.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "Editor.h"
#include "Engine/World.h"
#include "GameFramework/Actor.h"
#include "HAL/FileManager.h"
#include "Misc/PackageName.h"
#include "Misc/PackagePath.h"
#include "Misc/Paths.h"
#include "Misc/ScopeExit.h"
#include "NiagaraComponent.h"
#include "NiagaraEmitter.h"
#include "NiagaraEmitterHandle.h"
#include "NiagaraScript.h"
#include "NiagaraSystem.h"
#include "NiagaraTypes.h"
#include "Tests/AgentMcpTestHelpers.h"
#include "UObject/GarbageCollection.h"
#include "UObject/Linker.h"
#include "UObject/LinkerInstancingContext.h"
#include "UObject/Package.h"
#include "UObject/UObjectHash.h"

namespace
{
	// Distinct names from MaterialToolsTests' helpers — same-named anonymous-namespace
	// symbols collide once the full unity build merges the test TUs (P6 C2084 lesson).
	constexpr const TCHAR* KNiaRTPath        = TEXT("/Game/__McpTests/NS_McpNiagaraRT");
	constexpr const TCHAR* KNiaRTDiffPkgPath = TEXT("/Temp/McpNiagaraRT_ReloadCheck");
	constexpr const TCHAR* KNiaPlacePath     = TEXT("/Game/__McpTests/NS_McpNiagaraPlace");
	constexpr const TCHAR* KNiaEmitPath      = TEXT("/Game/__McpTests/NS_McpEmitterRT");
	constexpr const TCHAR* KNiaEmitDiffPkgPath = TEXT("/Temp/McpEmitterRT_ReloadCheck");
	constexpr const TCHAR* KNiaStockEmitter  = TEXT("/Niagara/DefaultAssets/Templates/Emitters/Fountain");
	constexpr const TCHAR* KNiaModPath       = TEXT("/Game/__McpTests/NS_McpModuleRT");
	constexpr const TCHAR* KNiaModDiffPkgPath = TEXT("/Temp/McpModuleRT_ReloadCheck");
	constexpr const TCHAR* KNiaBurstEmitter  = TEXT("/Niagara/DefaultAssets/Templates/Emitters/SimpleSpriteBurst");

	void NiaEvictDiffPackage(const TCHAR* DiffPkgPath)
	{
		if (UPackage* DiffPkg = FindPackage(nullptr, DiffPkgPath))
		{
			ResetLoaders(DiffPkg);
			TArray<UObject*> Inners;
			GetObjectsWithOuter(DiffPkg, Inners, /*bIncludeNestedObjects=*/true);
			for (UObject* Obj : Inners)
			{
				Obj->ClearFlags(RF_Standalone | RF_Public);
				Obj->MarkAsGarbage();
			}
			DiffPkg->ClearFlags(RF_Standalone);
			DiffPkg->MarkAsGarbage();
			CollectGarbage(GARBAGE_COLLECTION_KEEPFLAGS);
		}
	}

	void NiaDeleteGameAsset(FAutomationTestBase& Test, const TCHAR* Path)
	{
		UAgentMcpSettings* Settings = GetMutableDefault<UAgentMcpSettings>();
		const EAgentMcpTier Saved = Settings->PermissionTier;
		Settings->PermissionTier = EAgentMcpTier::Destructive;
		ON_SCOPE_EXIT { Settings->PermissionTier = Saved; };
		bool bErr = false;
		AgentMcpTestUtils::CallTool(Test, TEXT("delete_asset"),
			FString::Printf(TEXT("{\"asset_path\":\"%s\"}"), Path), bErr);
	}
}

// ---------------------------------------------------------------------------
// Create a system, expose a float + a color user param, verify via describe,
// save, then re-read the saved bytes (LOAD_ForDiff) and assert both parameter
// values survived — the persistence gate for the N1 authoring surface.
// ---------------------------------------------------------------------------
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FNiagaraSystemParamRoundTripTest,
	"UnrealAgentMCP.NiagaraTools.SystemParamRoundTrip",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FNiagaraSystemParamRoundTripTest::RunTest(const FString& Parameters)
{
	ON_SCOPE_EXIT
	{
		NiaEvictDiffPackage(KNiaRTDiffPkgPath);
		NiaDeleteGameAsset(*this, KNiaRTPath);
	};

	bool bErr = false;
	const FString Sys = FString(KNiaRTPath);

	AgentMcpTestUtils::CallTool(*this, TEXT("create_niagara_system"),
		TEXT("{\"name\":\"NS_McpNiagaraRT\",\"destination_path\":\"/Game/__McpTests\"}"), bErr);
	if (!TestFalse(TEXT("create_niagara_system not error"), bErr)) { return false; }

	AgentMcpTestUtils::CallTool(*this, TEXT("set_niagara_user_parameter"),
		FString::Printf(TEXT("{\"system\":\"%s\",\"name\":\"Glow\",\"type\":\"float\",\"value\":\"2.5\"}"), *Sys), bErr);
	if (!TestFalse(TEXT("set float param not error"), bErr)) { return false; }
	AgentMcpTestUtils::CallTool(*this, TEXT("set_niagara_user_parameter"),
		FString::Printf(TEXT("{\"system\":\"%s\",\"name\":\"Tint\",\"type\":\"linearcolor\",\"value\":\"(R=1.0,G=0.25,B=0.0,A=1.0)\"}"), *Sys), bErr);
	if (!TestFalse(TEXT("set color param not error"), bErr)) { return false; }

	// describe reports both (User.-prefixed names, live values)
	const TSharedPtr<FJsonObject> Desc = AgentMcpTestUtils::CallTool(*this, TEXT("describe_niagara_system"),
		FString::Printf(TEXT("{\"system\":\"%s\"}"), *Sys), bErr);
	if (TestTrue(TEXT("describe returned JSON"), Desc.IsValid()))
	{
		const TArray<TSharedPtr<FJsonValue>>* Params = nullptr;
		if (TestTrue(TEXT("user_parameters present"), Desc->TryGetArrayField(TEXT("user_parameters"), Params)))
		{
			bool bGlowSeen = false, bTintSeen = false;
			for (const TSharedPtr<FJsonValue>& V : *Params)
			{
				const TSharedPtr<FJsonObject> P = V->AsObject();
				if (!P.IsValid()) { continue; }
				const FString PName = P->GetStringField(TEXT("name"));
				if (PName.Contains(TEXT("Glow")))
				{
					bGlowSeen = true;
					double Val = 0.0;
					TestTrue(TEXT("Glow == 2.5"), P->TryGetNumberField(TEXT("value"), Val) && FMath::IsNearlyEqual(static_cast<float>(Val), 2.5f));
				}
				if (PName.Contains(TEXT("Tint"))) { bTintSeen = true; }
			}
			TestTrue(TEXT("Glow parameter listed"), bGlowSeen);
			TestTrue(TEXT("Tint parameter listed"), bTintSeen);
		}
	}

	// create_niagara_system blocks on WaitForCompilationComplete before returning,
	// so the asset is quiescent here — safe to save (and later delete) immediately.
	AgentMcpTestUtils::CallTool(*this, TEXT("save_asset"),
		FString::Printf(TEXT("{\"asset_path\":\"%s\"}"), *Sys), bErr);
	if (!TestFalse(TEXT("save_asset not error"), bErr)) { return false; }

	// --- re-read the saved bytes into a /Temp diff package ---
	const FString SrcFileName = FPackageName::LongPackageNameToFilename(KNiaRTPath, FPackageName::GetAssetPackageExtension());
	const FString TmpFileName = FPaths::ProjectSavedDir() + TEXT("McpNiagaraRT_ReloadCheck.uasset");
	if (!TestTrue(TEXT("copy saved package to /Temp"), IFileManager::Get().Copy(*TmpFileName, *SrcFileName) == COPY_OK)) { return false; }
	ON_SCOPE_EXIT { IFileManager::Get().Delete(*TmpFileName, false, true, true); };

	const FPackagePath TempPath = FPackagePath::FromLocalPath(TmpFileName);
	const FPackagePath OrigPath = FPackagePath::FromLocalPath(SrcFileName);
	FLinkerInstancingContext Ctx;
	Ctx.AddPackageMapping(OrigPath.GetPackageFName(), TempPath.GetPackageFName());
	UPackage* LoadedPkg = LoadPackage(nullptr, *TempPath.GetPackageName(),
		LOAD_ForDiff | LOAD_DisableCompileOnLoad | LOAD_DisableEngineVersionChecks, nullptr, &Ctx);
	if (!TestNotNull(TEXT("package re-loaded from disk bytes"), LoadedPkg)) { return false; }

	UNiagaraSystem* Reloaded = FindObject<UNiagaraSystem>(LoadedPkg, *FPackageName::GetShortName(KNiaRTPath));
	if (!TestNotNull(TEXT("system found in re-loaded package"), Reloaded)) { return false; }

	FNiagaraUserRedirectionParameterStore& Store = Reloaded->GetExposedParameters();
	const float Glow = Store.GetParameterValue<float>(
		FNiagaraVariable(FNiagaraTypeDefinition::GetFloatDef(), TEXT("User.Glow")));
	TestTrue(TEXT("Glow 2.5 survives save->reload"), FMath::IsNearlyEqual(Glow, 2.5f));
	const FLinearColor Tint = Store.GetParameterValue<FLinearColor>(
		FNiagaraVariable(FNiagaraTypeDefinition::GetColorDef(), TEXT("User.Tint")));
	TestTrue(TEXT("Tint color survives save->reload"),
		Tint.Equals(FLinearColor(1.0f, 0.25f, 0.0f, 1.0f), 0.001f));

	return true;
}

// ---------------------------------------------------------------------------
// place_niagara_component adds a registered UNiagaraComponent carrying the system.
// ---------------------------------------------------------------------------
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FPlaceNiagaraComponentTest,
	"UnrealAgentMCP.NiagaraTools.PlaceComponentOnActor",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FPlaceNiagaraComponentTest::RunTest(const FString& Parameters)
{
	ON_SCOPE_EXIT { NiaDeleteGameAsset(*this, KNiaPlacePath); };
	bool bErr = false;

	AgentMcpTestUtils::CallTool(*this, TEXT("create_niagara_system"),
		TEXT("{\"name\":\"NS_McpNiagaraPlace\",\"destination_path\":\"/Game/__McpTests\"}"), bErr);
	if (!TestFalse(TEXT("create system not error"), bErr)) { return false; }
	UNiagaraSystem* Sys = FindObject<UNiagaraSystem>(nullptr, TEXT("/Game/__McpTests/NS_McpNiagaraPlace.NS_McpNiagaraPlace"));
	if (!TestNotNull(TEXT("system loaded"), Sys)) { return false; }

	UWorld* World = GEditor ? GEditor->GetEditorWorldContext().World() : nullptr;
	if (!TestNotNull(TEXT("editor world"), World)) { return false; }
	AActor* Actor = World->SpawnActor<AActor>();
	if (!TestNotNull(TEXT("spawned actor"), Actor)) { return false; }
	ON_SCOPE_EXIT { if (IsValid(Actor)) { Actor->Destroy(); } };

	const TSharedPtr<FJsonObject> Res = AgentMcpTestUtils::CallTool(*this, TEXT("place_niagara_component"),
		FString::Printf(TEXT("{\"actor_path\":\"%s\",\"system\":\"%s\"}"), *Actor->GetPathName(), KNiaPlacePath), bErr);
	if (!TestFalse(TEXT("place_niagara_component not error"), bErr)) { return false; }
	bool bOk = false;
	TestTrue(TEXT("place returns ok"), Res.IsValid() && Res->TryGetBoolField(TEXT("ok"), bOk) && bOk);

	UNiagaraComponent* Comp = Actor->FindComponentByClass<UNiagaraComponent>();
	if (TestNotNull(TEXT("NiagaraComponent on actor"), Comp))
	{
		TestTrue(TEXT("component registered"), Comp->IsRegistered());
		TestTrue(TEXT("component carries the system"), Comp->GetAsset() == Sys);
	}
	return true;
}

// ---------------------------------------------------------------------------
// Error path: create_niagara_system outside /Game is a tool error naming /Game.
// ---------------------------------------------------------------------------
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FNiagaraCreateOutsideGameTest,
	"UnrealAgentMCP.NiagaraTools.CreateOutsideGameIsError",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FNiagaraCreateOutsideGameTest::RunTest(const FString& Parameters)
{
	bool bIsError = false;
	const FString RawText = AgentMcpTestUtils::CallToolRawText(*this, TEXT("create_niagara_system"),
		TEXT("{\"name\":\"NS_Sneaky\",\"destination_path\":\"/Engine/Sneaky\"}"), bIsError);

	TestTrue(TEXT("create_niagara_system into /Engine is a tool error"), bIsError);
	TestTrue(TEXT("error names /Game"), RawText.Contains(TEXT("/Game")));
	return true;
}

// ---------------------------------------------------------------------------
// M4 persistence gate: add the stock Fountain emitter to a fresh system, save,
// re-read the bytes (LOAD_ForDiff) and assert the emitter handle survived.
// Green is the ship condition for add_niagara_emitter (design §6.2).
// ---------------------------------------------------------------------------
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FAddNiagaraEmitterRoundTripTest,
	"UnrealAgentMCP.NiagaraTools.AddEmitterRoundTrip",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FAddNiagaraEmitterRoundTripTest::RunTest(const FString& Parameters)
{
	ON_SCOPE_EXIT
	{
		NiaEvictDiffPackage(KNiaEmitDiffPkgPath);
		NiaDeleteGameAsset(*this, KNiaEmitPath);
	};

	bool bErr = false;
	const FString Sys = FString(KNiaEmitPath);

	AgentMcpTestUtils::CallTool(*this, TEXT("create_niagara_system"),
		TEXT("{\"name\":\"NS_McpEmitterRT\",\"destination_path\":\"/Game/__McpTests\"}"), bErr);
	if (!TestFalse(TEXT("create_niagara_system not error"), bErr)) { return false; }

	const TSharedPtr<FJsonObject> AddRes = AgentMcpTestUtils::CallTool(*this, TEXT("add_niagara_emitter"),
		FString::Printf(TEXT("{\"system\":\"%s\",\"source_emitter\":\"%s\"}"), *Sys, KNiaStockEmitter), bErr);
	if (!TestFalse(TEXT("add_niagara_emitter not error"), bErr)) { return false; }

	FString HandleName;
	if (TestTrue(TEXT("add returned JSON"), AddRes.IsValid()))
	{
		AddRes->TryGetStringField(TEXT("emitter_handle"), HandleName);
		TestTrue(TEXT("emitter_handle named"), !HandleName.IsEmpty());
		double Count = 0.0;
		TestTrue(TEXT("emitter_count == 1"),
			AddRes->TryGetNumberField(TEXT("emitter_count"), Count) && static_cast<int32>(Count) == 1);
	}

	AgentMcpTestUtils::CallTool(*this, TEXT("save_asset"),
		FString::Printf(TEXT("{\"asset_path\":\"%s\"}"), *Sys), bErr);
	if (!TestFalse(TEXT("save_asset not error"), bErr)) { return false; }

	// --- re-read the saved bytes into a /Temp diff package ---
	const FString SrcFileName = FPackageName::LongPackageNameToFilename(KNiaEmitPath, FPackageName::GetAssetPackageExtension());
	const FString TmpFileName = FPaths::ProjectSavedDir() + TEXT("McpEmitterRT_ReloadCheck.uasset");
	if (!TestTrue(TEXT("copy saved package to /Temp"), IFileManager::Get().Copy(*TmpFileName, *SrcFileName) == COPY_OK)) { return false; }
	ON_SCOPE_EXIT { IFileManager::Get().Delete(*TmpFileName, false, true, true); };

	const FPackagePath TempPath = FPackagePath::FromLocalPath(TmpFileName);
	const FPackagePath OrigPath = FPackagePath::FromLocalPath(SrcFileName);
	FLinkerInstancingContext Ctx;
	Ctx.AddPackageMapping(OrigPath.GetPackageFName(), TempPath.GetPackageFName());
	UPackage* LoadedPkg = LoadPackage(nullptr, *TempPath.GetPackageName(),
		LOAD_ForDiff | LOAD_DisableCompileOnLoad | LOAD_DisableEngineVersionChecks, nullptr, &Ctx);
	if (!TestNotNull(TEXT("package re-loaded from disk bytes"), LoadedPkg)) { return false; }

	UNiagaraSystem* Reloaded = FindObject<UNiagaraSystem>(LoadedPkg, *FPackageName::GetShortName(KNiaEmitPath));
	if (!TestNotNull(TEXT("system found in re-loaded package"), Reloaded)) { return false; }

	TestEqual(TEXT("emitter handle survives save->reload"), Reloaded->GetEmitterHandles().Num(), 1);
	if (Reloaded->GetEmitterHandles().Num() == 1)
	{
		TestEqual(TEXT("handle keeps the source emitter's name"),
			Reloaded->GetEmitterHandles()[0].GetName().ToString(), HandleName);
	}

	return true;
}

// ---------------------------------------------------------------------------
// N2 persistence gate: list a stock emitter's rapid-iteration module inputs,
// set the first LinearColor one (fallback: first float), verify via re-list,
// save, then re-read the bytes and assert the value survived in the scripts'
// RapidIterationParameters stores.
// ---------------------------------------------------------------------------
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FSetNiagaraModuleInputRoundTripTest,
	"UnrealAgentMCP.NiagaraTools.ModuleInputRoundTrip",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FSetNiagaraModuleInputRoundTripTest::RunTest(const FString& Parameters)
{
	ON_SCOPE_EXIT
	{
		NiaEvictDiffPackage(KNiaModDiffPkgPath);
		NiaDeleteGameAsset(*this, KNiaModPath);
	};

	bool bErr = false;
	const FString Sys = FString(KNiaModPath);

	AgentMcpTestUtils::CallTool(*this, TEXT("create_niagara_system"),
		TEXT("{\"name\":\"NS_McpModuleRT\",\"destination_path\":\"/Game/__McpTests\"}"), bErr);
	if (!TestFalse(TEXT("create system not error"), bErr)) { return false; }
	AgentMcpTestUtils::CallTool(*this, TEXT("add_niagara_emitter"),
		FString::Printf(TEXT("{\"system\":\"%s\",\"source_emitter\":\"%s\"}"), *Sys, KNiaBurstEmitter), bErr);
	if (!TestFalse(TEXT("add emitter not error"), bErr)) { return false; }

	// --- list & pick an editable input (prefer LinearColor, fallback float) ---
	const TSharedPtr<FJsonObject> ListRes = AgentMcpTestUtils::CallTool(*this, TEXT("list_niagara_module_inputs"),
		FString::Printf(TEXT("{\"system\":\"%s\"}"), *Sys), bErr);
	if (!TestFalse(TEXT("list not error"), bErr) || !TestTrue(TEXT("list returned JSON"), ListRes.IsValid())) { return false; }

	const TArray<TSharedPtr<FJsonValue>>* Inputs = nullptr;
	if (!TestTrue(TEXT("module_inputs present"), ListRes->TryGetArrayField(TEXT("module_inputs"), Inputs))) { return false; }
	if (!TestTrue(TEXT("template exposes at least one editable input"), Inputs->Num() > 0)) { return false; }

	FString ColorName, FloatName;
	for (const TSharedPtr<FJsonValue>& V : *Inputs)
	{
		const TSharedPtr<FJsonObject> P = V->AsObject();
		if (!P.IsValid()) { continue; }
		const FString TypeName = P->GetStringField(TEXT("type"));
		if (ColorName.IsEmpty() && TypeName.Contains(TEXT("LinearColor"))) { ColorName = P->GetStringField(TEXT("name")); }
		if (FloatName.IsEmpty() && TypeName.Equals(TEXT("float"), ESearchCase::IgnoreCase)) { FloatName = P->GetStringField(TEXT("name")); }
	}
	const bool bUseColor = !ColorName.IsEmpty();
	const FString TargetName = bUseColor ? ColorName : FloatName;
	if (!TestTrue(TEXT("found a color or float input to edit"), !TargetName.IsEmpty())) { return false; }
	const FString NewValue = bUseColor ? TEXT("(R=0.25,G=0.01,B=0.01,A=1.0)") : TEXT("123.5");

	// --- set by exact name ---
	const TSharedPtr<FJsonObject> SetRes = AgentMcpTestUtils::CallTool(*this, TEXT("set_niagara_module_input"),
		FString::Printf(TEXT("{\"system\":\"%s\",\"parameter\":\"%s\",\"value\":\"%s\"}"), *Sys, *TargetName, *NewValue), bErr);
	if (!TestFalse(TEXT("set not error"), bErr) || !TestTrue(TEXT("set returned JSON"), SetRes.IsValid())) { return false; }
	double Updated = 0.0;
	TestTrue(TEXT("at least one script updated"),
		SetRes->TryGetNumberField(TEXT("scripts_updated"), Updated) && Updated >= 1.0);

	// --- save + reload bytes, then read the store directly ---
	AgentMcpTestUtils::CallTool(*this, TEXT("save_asset"),
		FString::Printf(TEXT("{\"asset_path\":\"%s\"}"), *Sys), bErr);
	if (!TestFalse(TEXT("save_asset not error"), bErr)) { return false; }

	const FString SrcFileName = FPackageName::LongPackageNameToFilename(KNiaModPath, FPackageName::GetAssetPackageExtension());
	const FString TmpFileName = FPaths::ProjectSavedDir() + TEXT("McpModuleRT_ReloadCheck.uasset");
	if (!TestTrue(TEXT("copy saved package to /Temp"), IFileManager::Get().Copy(*TmpFileName, *SrcFileName) == COPY_OK)) { return false; }
	ON_SCOPE_EXIT { IFileManager::Get().Delete(*TmpFileName, false, true, true); };

	const FPackagePath TempPath = FPackagePath::FromLocalPath(TmpFileName);
	const FPackagePath OrigPath = FPackagePath::FromLocalPath(SrcFileName);
	FLinkerInstancingContext Ctx;
	Ctx.AddPackageMapping(OrigPath.GetPackageFName(), TempPath.GetPackageFName());
	UPackage* LoadedPkg = LoadPackage(nullptr, *TempPath.GetPackageName(),
		LOAD_ForDiff | LOAD_DisableCompileOnLoad | LOAD_DisableEngineVersionChecks, nullptr, &Ctx);
	if (!TestNotNull(TEXT("package re-loaded from disk bytes"), LoadedPkg)) { return false; }
	UNiagaraSystem* Reloaded = FindObject<UNiagaraSystem>(LoadedPkg, *FPackageName::GetShortName(KNiaModPath));
	if (!TestNotNull(TEXT("system found in re-loaded package"), Reloaded)) { return false; }

	// Sweep every script store for the edited parameter and assert the new value.
	TArray<UNiagaraScript*> AllScripts;
	if (UNiagaraScript* S = Reloaded->GetSystemSpawnScript()) { AllScripts.Add(S); }
	if (UNiagaraScript* S = Reloaded->GetSystemUpdateScript()) { AllScripts.Add(S); }
	for (const FNiagaraEmitterHandle& Handle : Reloaded->GetEmitterHandles())
	{
		if (FVersionedNiagaraEmitterData* Data = Handle.GetEmitterData())
		{
			TArray<UNiagaraScript*> EmitterScripts;
			Data->GetScripts(EmitterScripts, /*bCompilableOnly=*/false);
			AllScripts.Append(EmitterScripts);
		}
	}

	bool bFoundPersisted = false;
	for (UNiagaraScript* Script : AllScripts)
	{
		if (!Script) { continue; }
		TArray<FNiagaraVariable> Vars;
		Script->RapidIterationParameters.GetParameters(Vars);
		for (const FNiagaraVariable& Var : Vars)
		{
			if (!Var.GetName().ToString().Equals(TargetName, ESearchCase::IgnoreCase)) { continue; }
			if (bUseColor)
			{
				const FLinearColor V = Script->RapidIterationParameters.GetParameterValue<FLinearColor>(Var);
				if (V.Equals(FLinearColor(0.25f, 0.01f, 0.01f, 1.0f), 0.001f)) { bFoundPersisted = true; }
			}
			else
			{
				const float V = Script->RapidIterationParameters.GetParameterValue<float>(Var);
				if (FMath::IsNearlyEqual(V, 123.5f, 0.001f)) { bFoundPersisted = true; }
			}
		}
	}
	TestTrue(TEXT("module input value survives save->reload"), bFoundPersisted);

	return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS
