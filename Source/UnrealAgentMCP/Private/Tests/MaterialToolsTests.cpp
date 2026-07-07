#include "Misc/AutomationTest.h"

#if WITH_DEV_AUTOMATION_TESTS

#include "AgentMcpSettings.h"
#include "Core/AgentMcpTier.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "HAL/FileManager.h"
#include "MaterialEditingLibrary.h"
#include "MaterialExpressionIO.h"
#include "Materials/Material.h"
#include "Materials/MaterialExpression.h"
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
	constexpr const TCHAR* KMatRTPath        = TEXT("/Game/__McpTests/M_McpMaterialRT");
	constexpr const TCHAR* KMatRTDiffPkgPath = TEXT("/Temp/McpMaterialRT_ReloadCheck");

	/** Purges the diff-load package and deletes the /Game round-trip material. */
	void CleanupMatRTAssets(FAutomationTestBase& Test)
	{
		if (UPackage* DiffPkg = FindPackage(nullptr, KMatRTDiffPkgPath))
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

		UAgentMcpSettings* Settings = GetMutableDefault<UAgentMcpSettings>();
		const EAgentMcpTier Saved = Settings->PermissionTier;
		Settings->PermissionTier = EAgentMcpTier::Destructive;
		ON_SCOPE_EXIT { Settings->PermissionTier = Saved; };

		bool bErr = false;
		AgentMcpTestUtils::CallTool(Test, TEXT("delete_asset"),
			FString::Printf(TEXT("{\"asset_path\":\"%s\"}"), KMatRTPath), bErr);
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

#endif // WITH_DEV_AUTOMATION_TESTS
