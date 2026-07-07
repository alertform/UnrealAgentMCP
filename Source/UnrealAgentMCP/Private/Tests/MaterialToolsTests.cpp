#include "Misc/AutomationTest.h"

#if WITH_DEV_AUTOMATION_TESTS

#include "AgentMcpSettings.h"
#include "Components/StaticMeshComponent.h"
#include "Core/AgentMcpTier.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "Editor.h"
#include "Engine/StaticMeshActor.h"
#include "Engine/World.h"
#include "HAL/FileManager.h"
#include "MaterialEditingLibrary.h"
#include "MaterialExpressionIO.h"
#include "Materials/Material.h"
#include "Materials/MaterialExpression.h"
#include "Materials/MaterialInstanceConstant.h"
#include "Materials/MaterialInterface.h"
#include "Misc/PackageName.h"
#include "Misc/PackagePath.h"
#include "Misc/Paths.h"
#include "Misc/ScopeExit.h"
#include "SceneTypes.h"
#include "Tests/AgentMcpTestHelpers.h"
#include "UObject/GarbageCollection.h"
#include "UObject/Linker.h"
#include "UObject/LinkerInstancingContext.h"
#include "UObject/Package.h"
#include "UObject/UObjectHash.h"

namespace
{
	constexpr const TCHAR* KMatRTPath         = TEXT("/Game/__McpTests/M_McpMaterialRT");
	constexpr const TCHAR* KMatRTDiffPkgPath  = TEXT("/Temp/McpMaterialRT_ReloadCheck");
	constexpr const TCHAR* KMatInstParentPath = TEXT("/Game/__McpTests/M_McpMatInstParentRT");
	constexpr const TCHAR* KMatInstPath       = TEXT("/Game/__McpTests/MI_McpMatInstRT");
	constexpr const TCHAR* KMatInstDiffPkgPath= TEXT("/Temp/McpMatInstRT_ReloadCheck");
	constexpr const TCHAR* KAssignMatPath     = TEXT("/Game/__McpTests/M_McpAssignRT");

	/** Evicts a LOAD_ForDiff temp package so it stops pinning the /Game source and repeat runs are clean. */
	void EvictDiffPackage(const TCHAR* DiffPkgPath)
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

	/** Raises the permission tier to Destructive and deletes a /Game asset (test teardown). */
	void DeleteGameAsset(FAutomationTestBase& Test, const TCHAR* Path)
	{
		UAgentMcpSettings* Settings = GetMutableDefault<UAgentMcpSettings>();
		const EAgentMcpTier Saved = Settings->PermissionTier;
		Settings->PermissionTier = EAgentMcpTier::Destructive;
		ON_SCOPE_EXIT { Settings->PermissionTier = Saved; };
		bool bErr = false;
		AgentMcpTestUtils::CallTool(Test, TEXT("delete_asset"),
			FString::Printf(TEXT("{\"asset_path\":\"%s\"}"), Path), bErr);
	}

	void CleanupMatRTAssets(FAutomationTestBase& Test)
	{
		EvictDiffPackage(KMatRTDiffPkgPath);
		DeleteGameAsset(Test, KMatRTPath);
	}
}

// ---------------------------------------------------------------------------
// Author an emissive material end-to-end, then re-read the saved bytes and
// assert the graph survived the save (the RF_Transient / node-loss gate — same
// LOAD_ForDiff technique as the AnimGraph save/reload test).
// ---------------------------------------------------------------------------
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FMaterialAuthorRoundTripTest,
	"UnrealAgentMCP.MaterialTools.AuthorEmissiveRoundTrip",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FMaterialAuthorRoundTripTest::RunTest(const FString& Parameters)
{
	ON_SCOPE_EXIT { CleanupMatRTAssets(*this); };

	bool bErr = false;
	const FString Mat = FString(KMatRTPath);

	// create
	AgentMcpTestUtils::CallTool(*this, TEXT("create_material"),
		TEXT("{\"name\":\"M_McpMaterialRT\",\"destination_path\":\"/Game/__McpTests\"}"), bErr);
	if (!TestFalse(TEXT("create_material not error"), bErr)) { return false; }

	// three nodes
	auto AddNode = [&](const TCHAR* Class) -> FString
	{
		const TSharedPtr<FJsonObject> R = AgentMcpTestUtils::CallTool(*this, TEXT("add_material_expression"),
			FString::Printf(TEXT("{\"material\":\"%s\",\"expression_class\":\"%s\"}"), *Mat, Class), bErr);
		FString Id;
		if (R.IsValid()) { R->TryGetStringField(TEXT("node_id"), Id); }
		return Id;
	};
	const FString C3 = AddNode(TEXT("Constant3Vector"));
	const FString SP = AddNode(TEXT("ScalarParameter"));
	const FString ML = AddNode(TEXT("Multiply"));
	if (!TestFalse(TEXT("add_material_expression not error"), bErr)) { return false; }
	TestTrue(TEXT("node ids returned"), !C3.IsEmpty() && !SP.IsEmpty() && !ML.IsEmpty());

	// set node properties (exercises set_material_expression_property / ImportText)
	AgentMcpTestUtils::CallTool(*this, TEXT("set_material_expression_property"),
		FString::Printf(TEXT("{\"material\":\"%s\",\"node_id\":\"%s\",\"property\":\"Constant\",\"value\":\"(R=0.1,G=0.6,B=1.0)\"}"), *Mat, *C3), bErr);
	TestFalse(TEXT("set Constant not error"), bErr);
	AgentMcpTestUtils::CallTool(*this, TEXT("set_material_expression_property"),
		FString::Printf(TEXT("{\"material\":\"%s\",\"node_id\":\"%s\",\"property\":\"ParameterName\",\"value\":\"Glow\"}"), *Mat, *SP), bErr);
	TestFalse(TEXT("set ParameterName not error"), bErr);
	AgentMcpTestUtils::CallTool(*this, TEXT("set_material_expression_property"),
		FString::Printf(TEXT("{\"material\":\"%s\",\"node_id\":\"%s\",\"property\":\"DefaultValue\",\"value\":\"4.0\"}"), *Mat, *SP), bErr);
	TestFalse(TEXT("set DefaultValue not error"), bErr);

	// wire: C3 -> Multiply.A, SP -> Multiply.B, Multiply -> EmissiveColor
	AgentMcpTestUtils::CallTool(*this, TEXT("connect_material_expression"),
		FString::Printf(TEXT("{\"material\":\"%s\",\"from_node\":\"%s\",\"to_node\":\"%s\",\"to_input\":\"A\"}"), *Mat, *C3, *ML), bErr);
	AgentMcpTestUtils::CallTool(*this, TEXT("connect_material_expression"),
		FString::Printf(TEXT("{\"material\":\"%s\",\"from_node\":\"%s\",\"to_node\":\"%s\",\"to_input\":\"B\"}"), *Mat, *SP, *ML), bErr);
	const TSharedPtr<FJsonObject> PropConn = AgentMcpTestUtils::CallTool(*this, TEXT("connect_material_property"),
		FString::Printf(TEXT("{\"material\":\"%s\",\"from_node\":\"%s\",\"property\":\"EmissiveColor\"}"), *Mat, *ML), bErr);
	if (!TestFalse(TEXT("connect not error"), bErr)) { return false; }
	bool bConnected = false;
	TestTrue(TEXT("EmissiveColor connected"), PropConn.IsValid() && PropConn->TryGetBoolField(TEXT("connected"), bConnected) && bConnected);

	// describe (asset-level, headless) reports the graph we just built
	const TSharedPtr<FJsonObject> Desc = AgentMcpTestUtils::CallTool(*this, TEXT("describe_material"),
		FString::Printf(TEXT("{\"material\":\"%s\"}"), *Mat), bErr);
	if (TestTrue(TEXT("describe_material returned JSON"), Desc.IsValid()))
	{
		double Count = 0.0;
		TestTrue(TEXT("describe expression_count == 3"),
			Desc->TryGetNumberField(TEXT("expression_count"), Count) && static_cast<int32>(Count) == 3);
		const TSharedPtr<FJsonObject>* Props = nullptr;
		if (TestTrue(TEXT("describe has properties"), Desc->TryGetObjectField(TEXT("properties"), Props)))
		{
			FString EmissiveNode;
			TestTrue(TEXT("EmissiveColor property maps to the Multiply node"),
				(*Props)->TryGetStringField(TEXT("EmissiveColor"), EmissiveNode) && EmissiveNode == ML);
		}
	}

	// recompile (smoke — must not error)
	AgentMcpTestUtils::CallTool(*this, TEXT("recompile_material"),
		FString::Printf(TEXT("{\"material\":\"%s\"}"), *Mat), bErr);
	TestFalse(TEXT("recompile_material not error"), bErr);

	// save to disk
	AgentMcpTestUtils::CallTool(*this, TEXT("save_asset"),
		FString::Printf(TEXT("{\"asset_path\":\"%s\"}"), *Mat), bErr);
	if (!TestFalse(TEXT("save_asset not error"), bErr)) { return false; }

	// --- re-read the saved bytes into a /Temp diff package (LOAD_ForDiff) ---
	const FString SrcFileName = FPackageName::LongPackageNameToFilename(
		KMatRTPath, FPackageName::GetAssetPackageExtension());
	const FString TmpFileName = FPaths::ProjectSavedDir() + TEXT("McpMaterialRT_ReloadCheck.uasset");
	if (!TestTrue(TEXT("copy saved package to /Temp mount"),
		IFileManager::Get().Copy(*TmpFileName, *SrcFileName) == COPY_OK))
	{
		return false;
	}
	ON_SCOPE_EXIT { IFileManager::Get().Delete(*TmpFileName, false, true, true); };

	const FPackagePath TempPackagePath     = FPackagePath::FromLocalPath(TmpFileName);
	const FPackagePath OriginalPackagePath = FPackagePath::FromLocalPath(SrcFileName);
	FLinkerInstancingContext InstancingContext;
	InstancingContext.AddPackageMapping(
		OriginalPackagePath.GetPackageFName(), TempPackagePath.GetPackageFName());

	UPackage* LoadedPkg = LoadPackage(nullptr, *TempPackagePath.GetPackageName(),
		LOAD_ForDiff | LOAD_DisableCompileOnLoad | LOAD_DisableEngineVersionChecks,
		nullptr, &InstancingContext);
	if (!TestNotNull(TEXT("package re-loaded from disk bytes"), LoadedPkg)) { return false; }

	UMaterial* Reloaded = FindObject<UMaterial>(LoadedPkg, *FPackageName::GetShortName(KMatRTPath));
	if (!TestNotNull(TEXT("material found in re-loaded package"), Reloaded)) { return false; }

	TestEqual(TEXT("3 expressions survive save->reload"),
		UMaterialEditingLibrary::GetNumMaterialExpressions(Reloaded), 3);

	const FExpressionInput* EmissiveIn = Reloaded->GetExpressionInputForProperty(MP_EmissiveColor);
	TestTrue(TEXT("EmissiveColor connection survives save->reload"),
		EmissiveIn != nullptr && EmissiveIn->Expression != nullptr);

	return true;
}

// ---------------------------------------------------------------------------
// Error path: create_material outside /Game is a tool error naming /Game.
// ---------------------------------------------------------------------------
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FMaterialCreateOutsideGameTest,
	"UnrealAgentMCP.MaterialTools.CreateOutsideGameIsError",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FMaterialCreateOutsideGameTest::RunTest(const FString& Parameters)
{
	bool bIsError = false;
	const FString RawText = AgentMcpTestUtils::CallToolRawText(*this, TEXT("create_material"),
		TEXT("{\"name\":\"M_Sneaky\",\"destination_path\":\"/Engine/Sneaky\"}"), bIsError);

	TestTrue(TEXT("create_material into /Engine is a tool error"), bIsError);
	TestTrue(TEXT("error names /Game"), RawText.Contains(TEXT("/Game")));
	return true;
}

// ---------------------------------------------------------------------------
// Create a MIC over a parent material that exposes a scalar param, override it,
// then re-read the saved bytes and assert the override survived (LOAD_ForDiff).
// ---------------------------------------------------------------------------
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FMaterialInstanceRoundTripTest,
	"UnrealAgentMCP.MaterialTools.InstanceRoundTrip",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FMaterialInstanceRoundTripTest::RunTest(const FString& Parameters)
{
	ON_SCOPE_EXIT
	{
		EvictDiffPackage(KMatInstDiffPkgPath);
		DeleteGameAsset(*this, KMatInstPath);
		DeleteGameAsset(*this, KMatInstParentPath);
	};

	bool bErr = false;
	const FString Parent = FString(KMatInstParentPath);
	const FString Inst   = FString(KMatInstPath);

	// Parent material exposing ScalarParameter "Glow" (connected so it's collected).
	AgentMcpTestUtils::CallTool(*this, TEXT("create_material"),
		TEXT("{\"name\":\"M_McpMatInstParentRT\",\"destination_path\":\"/Game/__McpTests\"}"), bErr);
	if (!TestFalse(TEXT("create parent material"), bErr)) { return false; }
	const TSharedPtr<FJsonObject> SPRes = AgentMcpTestUtils::CallTool(*this, TEXT("add_material_expression"),
		FString::Printf(TEXT("{\"material\":\"%s\",\"expression_class\":\"ScalarParameter\"}"), *Parent), bErr);
	FString SP;
	if (SPRes.IsValid()) { SPRes->TryGetStringField(TEXT("node_id"), SP); }
	AgentMcpTestUtils::CallTool(*this, TEXT("set_material_expression_property"),
		FString::Printf(TEXT("{\"material\":\"%s\",\"node_id\":\"%s\",\"property\":\"ParameterName\",\"value\":\"Glow\"}"), *Parent, *SP), bErr);
	AgentMcpTestUtils::CallTool(*this, TEXT("connect_material_property"),
		FString::Printf(TEXT("{\"material\":\"%s\",\"from_node\":\"%s\",\"property\":\"Roughness\"}"), *Parent, *SP), bErr);
	AgentMcpTestUtils::CallTool(*this, TEXT("recompile_material"),
		FString::Printf(TEXT("{\"material\":\"%s\"}"), *Parent), bErr);
	AgentMcpTestUtils::CallTool(*this, TEXT("save_asset"),
		FString::Printf(TEXT("{\"asset_path\":\"%s\"}"), *Parent), bErr);
	if (!TestFalse(TEXT("parent authoring not error"), bErr)) { return false; }

	// MIC parented to it, override Glow = 4.0.
	AgentMcpTestUtils::CallTool(*this, TEXT("create_material_instance"),
		FString::Printf(TEXT("{\"name\":\"MI_McpMatInstRT\",\"destination_path\":\"/Game/__McpTests\",\"parent\":\"%s\"}"), *Parent), bErr);
	if (!TestFalse(TEXT("create_material_instance not error"), bErr)) { return false; }
	AgentMcpTestUtils::CallTool(*this, TEXT("set_material_instance_parameter"),
		FString::Printf(TEXT("{\"material_instance\":\"%s\",\"parameter\":\"Glow\",\"type\":\"scalar\",\"value\":\"4.0\"}"), *Inst), bErr);
	if (!TestFalse(TEXT("set scalar not error"), bErr)) { return false; }

	// describe reports parent + override
	const TSharedPtr<FJsonObject> Desc = AgentMcpTestUtils::CallTool(*this, TEXT("describe_material_instance"),
		FString::Printf(TEXT("{\"material_instance\":\"%s\"}"), *Inst), bErr);
	if (TestTrue(TEXT("describe_material_instance returned JSON"), Desc.IsValid()))
	{
		FString ParentPath;
		Desc->TryGetStringField(TEXT("parent"), ParentPath);
		TestTrue(TEXT("parent path reported"), ParentPath.Contains(TEXT("M_McpMatInstParentRT")));
		const TSharedPtr<FJsonObject>* Scalars = nullptr;
		if (TestTrue(TEXT("has scalars"), Desc->TryGetObjectField(TEXT("scalars"), Scalars)))
		{
			double Glow = 0.0;
			TestTrue(TEXT("Glow override == 4.0"),
				(*Scalars)->TryGetNumberField(TEXT("Glow"), Glow) && FMath::IsNearlyEqual(static_cast<float>(Glow), 4.0f));
		}
	}

	// save MIC, re-read the bytes, assert the override survived
	AgentMcpTestUtils::CallTool(*this, TEXT("save_asset"),
		FString::Printf(TEXT("{\"asset_path\":\"%s\"}"), *Inst), bErr);
	if (!TestFalse(TEXT("save MIC not error"), bErr)) { return false; }

	const FString SrcFileName = FPackageName::LongPackageNameToFilename(KMatInstPath, FPackageName::GetAssetPackageExtension());
	const FString TmpFileName = FPaths::ProjectSavedDir() + TEXT("McpMatInstRT_ReloadCheck.uasset");
	if (!TestTrue(TEXT("copy saved MIC to /Temp"), IFileManager::Get().Copy(*TmpFileName, *SrcFileName) == COPY_OK)) { return false; }
	ON_SCOPE_EXIT { IFileManager::Get().Delete(*TmpFileName, false, true, true); };

	const FPackagePath TempPath = FPackagePath::FromLocalPath(TmpFileName);
	const FPackagePath OrigPath = FPackagePath::FromLocalPath(SrcFileName);
	FLinkerInstancingContext Ctx;
	Ctx.AddPackageMapping(OrigPath.GetPackageFName(), TempPath.GetPackageFName());
	UPackage* LoadedPkg = LoadPackage(nullptr, *TempPath.GetPackageName(),
		LOAD_ForDiff | LOAD_DisableCompileOnLoad | LOAD_DisableEngineVersionChecks, nullptr, &Ctx);
	if (!TestNotNull(TEXT("MIC re-loaded from disk"), LoadedPkg)) { return false; }

	UMaterialInstanceConstant* Reloaded = FindObject<UMaterialInstanceConstant>(LoadedPkg, *FPackageName::GetShortName(KMatInstPath));
	if (!TestNotNull(TEXT("MIC found in re-loaded package"), Reloaded)) { return false; }
	TestTrue(TEXT("Glow override 4.0 survives save->reload"),
		FMath::IsNearlyEqual(UMaterialEditingLibrary::GetMaterialInstanceScalarParameterValue(Reloaded, TEXT("Glow")), 4.0f));

	return true;
}

// ---------------------------------------------------------------------------
// assign_material puts a material on a spawned editor-world actor's mesh slot 0.
// ---------------------------------------------------------------------------
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FAssignMaterialTest,
	"UnrealAgentMCP.MaterialTools.AssignToActor",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FAssignMaterialTest::RunTest(const FString& Parameters)
{
	ON_SCOPE_EXIT { DeleteGameAsset(*this, KAssignMatPath); };
	bool bErr = false;

	AgentMcpTestUtils::CallTool(*this, TEXT("create_material"),
		TEXT("{\"name\":\"M_McpAssignRT\",\"destination_path\":\"/Game/__McpTests\"}"), bErr);
	if (!TestFalse(TEXT("create material not error"), bErr)) { return false; }
	UMaterial* Mat = LoadObject<UMaterial>(nullptr, TEXT("/Game/__McpTests/M_McpAssignRT.M_McpAssignRT"));
	if (!TestNotNull(TEXT("material loaded"), Mat)) { return false; }

	UWorld* World = GEditor ? GEditor->GetEditorWorldContext().World() : nullptr;
	if (!TestNotNull(TEXT("editor world"), World)) { return false; }
	AStaticMeshActor* Actor = World->SpawnActor<AStaticMeshActor>();
	if (!TestNotNull(TEXT("spawned StaticMeshActor"), Actor)) { return false; }
	ON_SCOPE_EXIT { if (IsValid(Actor)) { Actor->Destroy(); } };

	const TSharedPtr<FJsonObject> Res = AgentMcpTestUtils::CallTool(*this, TEXT("assign_material"),
		FString::Printf(TEXT("{\"actor_path\":\"%s\",\"material\":\"%s\"}"), *Actor->GetPathName(), KAssignMatPath), bErr);
	if (!TestFalse(TEXT("assign_material not error"), bErr)) { return false; }
	bool bOk = false;
	TestTrue(TEXT("assign returns ok"), Res.IsValid() && Res->TryGetBoolField(TEXT("ok"), bOk) && bOk);

	UStaticMeshComponent* SMC = Actor->GetStaticMeshComponent();
	if (TestNotNull(TEXT("static mesh component"), SMC))
	{
		TestTrue(TEXT("slot 0 material is the assigned material"), SMC->GetMaterial(0) == Mat);
	}
	return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS
