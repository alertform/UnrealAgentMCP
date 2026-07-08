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

#endif // WITH_DEV_AUTOMATION_TESTS
