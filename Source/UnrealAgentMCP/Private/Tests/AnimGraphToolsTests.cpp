#include "Misc/AutomationTest.h"

#if WITH_DEV_AUTOMATION_TESTS

#include "AgentMcpSettings.h"
#include "Animation/AnimBlueprint.h"
#include "Animation/AnimBlueprintGeneratedClass.h"
#include "Animation/AnimInstance.h"
#include "Animation/Skeleton.h"
#include "Core/AgentMcpTier.h"
#include "Core/AgentMcpToolRegistry.h"
#include "Core/McpTypes.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "EdGraph/EdGraph.h"
#include "EdGraph/EdGraphNode.h"
#include "HAL/FileManager.h"
#include "Kismet2/KismetEditorUtilities.h"
#include "Misc/PackageName.h"
#include "Misc/PackagePath.h"
#include "Misc/Paths.h"
#include "Misc/ScopeExit.h"
#include "Tests/AgentMcpTestHelpers.h"
#include "UObject/GarbageCollection.h"
#include "UObject/Linker.h"
#include "UObject/LinkerInstancingContext.h"
#include "UObject/Package.h"
#include "UObject/UObjectHash.h"

// ---------------------------------------------------------------------------
// Constants
// ---------------------------------------------------------------------------

namespace
{
	// Transient paths — never touch disk; GC'd after test run.
	constexpr const TCHAR* KAnimBPName = TEXT("ABP_McpAnimGraphTest");
	constexpr const TCHAR* KSkelName   = TEXT("SK_McpAnimGraphTestSkel");

	// Save/reload round-trip test — real /Game packages, deleted on exit.
	constexpr const TCHAR* KAnimRTSkelPath = TEXT("/Game/Dev_Test/SK_McpAnimGraphRT");
	constexpr const TCHAR* KAnimRTABPPath  = TEXT("/Game/Dev_Test/ABP_McpAnimGraphRT");

	// Temp package the saved ABP file is re-loaded into (asset-diff technique) — never saved.
	constexpr const TCHAR* KAnimRTDiffPkgPath = TEXT("/Temp/McpAnimGraphRT_ReloadCheck");
}

// ---------------------------------------------------------------------------
// Helpers: create transient USkeleton and UAnimBlueprint
// ---------------------------------------------------------------------------
namespace
{
	/** Creates a transient USkeleton (no package, never saved). */
	USkeleton* MakeTransientSkeleton(const TCHAR* BaseName)
	{
		const FName UniqueName = MakeUniqueObjectName(
			GetTransientPackage(), USkeleton::StaticClass(), FName(BaseName));
		USkeleton* Skel = NewObject<USkeleton>(GetTransientPackage(), UniqueName, RF_Transient);
		return Skel;
	}

	/**
	 * Creates a transient UAnimBlueprint (UAnimInstance parent).
	 * AnimBlueprint creation requires a non-null skeleton set on it to avoid
	 * compilation warnings, but for graph node tests we only need the graph to exist.
	 */
	UAnimBlueprint* MakeTransientAnimBlueprint(const TCHAR* BaseName, USkeleton* Skeleton)
	{
		const FName UniqueName = MakeUniqueObjectName(
			GetTransientPackage(), UAnimBlueprint::StaticClass(), FName(BaseName));

		// CreateBlueprint with AnimBlueprint class type creates the AnimGraph automatically.
		UAnimBlueprint* AnimBP = Cast<UAnimBlueprint>(
			FKismetEditorUtilities::CreateBlueprint(
				UAnimInstance::StaticClass(),
				GetTransientPackage(),
				UniqueName,
				BPTYPE_Normal,
				UAnimBlueprint::StaticClass(),
				UAnimBlueprintGeneratedClass::StaticClass()));

		if (AnimBP && Skeleton)
		{
			AnimBP->TargetSkeleton = Skeleton;
		}
		return AnimBP;
	}

	/** Build a /Game/… style path string the tool can resolve for a transient object. */
	FString TransientPath(const UObject* Obj)
	{
		// Transient objects have paths like /Engine/Transient.ObjectName — pass the full object path.
		return Obj->GetPathName();
	}

	/** Deletes the save/reload round-trip assets; tier raised to Destructive for delete_asset. */
	void CleanupAnimRTAssets(FAutomationTestBase& Test)
	{
		// Purge the diff-load package first so its loaded copy doesn't pin the /Game assets,
		// and so repeat runs in the same session start from a clean temp package.
		if (UPackage* DiffPkg = FindPackage(nullptr, KAnimRTDiffPkgPath))
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
			FString::Printf(TEXT("{\"asset_path\":\"%s\"}"), KAnimRTABPPath), bErr);
		AgentMcpTestUtils::CallTool(Test, TEXT("delete_asset"),
			FString::Printf(TEXT("{\"asset_path\":\"%s\"}"), KAnimRTSkelPath), bErr);
	}
}

// ---------------------------------------------------------------------------
// FAddAnimGraphNodeTest
// ---------------------------------------------------------------------------
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FAddAnimGraphNodeTest,
	"UnrealAgentMCP.P8.AddAnimGraphNode",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FAddAnimGraphNodeTest::RunTest(const FString& Parameters)
{
	bool bIsError = false;

	// --- Setup: transient skeleton + AnimBlueprint ---
	USkeleton* Skel = MakeTransientSkeleton(KSkelName);
	if (!TestNotNull(TEXT("transient skeleton created"), Skel)) { return true; }

	UAnimBlueprint* AnimBP = MakeTransientAnimBlueprint(KAnimBPName, Skel);
	if (!TestNotNull(TEXT("transient AnimBlueprint created"), AnimBP)) { return true; }

	const FString AnimBPPath = TransientPath(AnimBP);

	// --- Happy path: add AnimGraphNode_Slot with Node.SlotName property ---
	const TSharedPtr<FJsonObject> AddResult = AgentMcpTestUtils::CallTool(*this,
		TEXT("add_anim_graph_node"),
		FString::Printf(
			TEXT("{\"blueprint_path\":\"%s\",\"node_class\":\"AnimGraphNode_Slot\","
			     "\"properties\":{\"Node.SlotName\":\"TestSlot\"},"
			     "\"pos_x\":200,\"pos_y\":100}"),
			*AnimBPPath),
		bIsError);

	TestFalse(TEXT("add_anim_graph_node succeeds"), bIsError);
	if (TestNotNull(TEXT("add result parses"), AddResult.Get()))
	{
		TestTrue(TEXT("added:true"), AddResult->GetBoolField(TEXT("added")));

		// node_id must be a non-empty GUID string
		FString NodeId;
		TestTrue(TEXT("node_id present"), AddResult->TryGetStringField(TEXT("node_id"), NodeId));
		TestFalse(TEXT("node_id non-empty"), NodeId.IsEmpty());

		// class should reflect AnimGraphNode_Slot
		FString NodeClass;
		if (AddResult->TryGetStringField(TEXT("class"), NodeClass))
		{
			TestTrue(TEXT("class contains Slot"),
				NodeClass.Contains(TEXT("Slot"), ESearchCase::IgnoreCase));
		}

		// pins array must be present and non-empty (Slot node has Source in + Pose out)
		const TArray<TSharedPtr<FJsonValue>>* PinsArr = nullptr;
		if (TestTrue(TEXT("pins array present"), AddResult->TryGetArrayField(TEXT("pins"), PinsArr)) && PinsArr)
		{
			TestTrue(TEXT("pins non-empty"), PinsArr->Num() > 0);

			// Check for at least one pin named "Result" or containing "pose" (case-insensitive)
			bool bHasPosePin = false;
			for (const TSharedPtr<FJsonValue>& PinVal : *PinsArr)
			{
				if (!PinVal.IsValid()) { continue; }
				TSharedPtr<FJsonObject> PinObj = PinVal->AsObject();
				if (!PinObj.IsValid()) { continue; }
				FString PinName;
				PinObj->TryGetStringField(TEXT("name"), PinName);
				if (PinName.Contains(TEXT("Result"), ESearchCase::IgnoreCase) ||
					PinName.Contains(TEXT("Source"), ESearchCase::IgnoreCase) ||
					PinName.Contains(TEXT("Pose"),   ESearchCase::IgnoreCase))
				{
					bHasPosePin = true;
					break;
				}
			}
			TestTrue(TEXT("at least one pose-related pin"), bHasPosePin);
		}
	}

	// --- Regression: spawned node must NOT be RF_Transient ---
	// (An RF_Transient template's flag survives CreateNode's DuplicateObject; SavePackage then
	//  silently skips the node — "saved fine", vanished on the next editor load. ABP_Manny bug.)
	if (AddResult.IsValid())
	{
		UEdGraph* AnimGraph = nullptr;
		for (UEdGraph* G : AnimBP->FunctionGraphs)
		{
			if (G && G->GetName() == TEXT("AnimGraph")) { AnimGraph = G; break; }
		}
		if (TestNotNull(TEXT("AnimGraph found on test ABP"), AnimGraph))
		{
			FString NodeId;
			AddResult->TryGetStringField(TEXT("node_id"), NodeId);
			FGuid NodeGuid;
			FGuid::Parse(NodeId, NodeGuid);

			const UEdGraphNode* SpawnedNode = nullptr;
			for (const UEdGraphNode* N : AnimGraph->Nodes)
			{
				if (N && N->NodeGuid == NodeGuid) { SpawnedNode = N; break; }
			}
			if (TestNotNull(TEXT("spawned node present in AnimGraph"), SpawnedNode))
			{
				TestFalse(TEXT("spawned node not RF_Transient (transient nodes are dropped by SavePackage)"),
					SpawnedNode->HasAnyFlags(RF_Transient));
			}
		}
	}

	// --- Error path 1: non-AnimBlueprint (pass a non-ABP path) ---
	// Use a deliberately bad path that won't load as a Blueprint at all.
	const FString BadBPErr = AgentMcpTestUtils::CallToolRawText(*this,
		TEXT("add_anim_graph_node"),
		TEXT("{\"blueprint_path\":\"/Game/NoSuchABP\",\"node_class\":\"AnimGraphNode_Slot\"}"),
		bIsError);
	TestTrue(TEXT("missing blueprint is error"), bIsError);
	TestFalse(TEXT("missing blueprint error non-empty"), BadBPErr.IsEmpty());

	// --- Error path 2: non-AnimGraphNode class ---
	const FString BadClassErr = AgentMcpTestUtils::CallToolRawText(*this,
		TEXT("add_anim_graph_node"),
		FString::Printf(
			TEXT("{\"blueprint_path\":\"%s\",\"node_class\":\"BTTask_MoveTo\"}"),
			*AnimBPPath),
		bIsError);
	TestTrue(TEXT("non-AnimGraphNode class is error"), bIsError);
	TestTrue(TEXT("bad class error mentions subclass"),
		BadClassErr.Contains(TEXT("AnimGraphNode_Base"), ESearchCase::IgnoreCase) ||
		BadClassErr.Contains(TEXT("subclass"),           ESearchCase::IgnoreCase) ||
		BadClassErr.Contains(TEXT("not found"),          ESearchCase::IgnoreCase));

	// --- Error path 3: missing node_class arg ---
	const FString MissingClassErr = AgentMcpTestUtils::CallToolRawText(*this,
		TEXT("add_anim_graph_node"),
		FString::Printf(TEXT("{\"blueprint_path\":\"%s\"}"), *AnimBPPath),
		bIsError);
	TestTrue(TEXT("missing node_class is error"), bIsError);

	return true;
}

// ---------------------------------------------------------------------------
// FRegisterSkeletonSlotTest
// ---------------------------------------------------------------------------
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FRegisterSkeletonSlotTest,
	"UnrealAgentMCP.P8.RegisterSkeletonSlot",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FRegisterSkeletonSlotTest::RunTest(const FString& Parameters)
{
	bool bIsError = false;

	// --- Setup: transient skeleton ---
	USkeleton* Skel = MakeTransientSkeleton(TEXT("SK_McpSlotTest"));
	if (!TestNotNull(TEXT("transient skeleton created"), Skel)) { return true; }

	const FString SkelPath = TransientPath(Skel);

	// --- Happy path: register a new slot ---
	const TSharedPtr<FJsonObject> RegResult = AgentMcpTestUtils::CallTool(*this,
		TEXT("register_skeleton_slot"),
		FString::Printf(
			TEXT("{\"skeleton_path\":\"%s\",\"slot_name\":\"UpperBody\",\"group_name\":\"DefaultGroup\"}"),
			*SkelPath),
		bIsError);

	TestFalse(TEXT("register_skeleton_slot succeeds"), bIsError);
	if (TestNotNull(TEXT("register result parses"), RegResult.Get()))
	{
		TestTrue(TEXT("registered:true"),          RegResult->GetBoolField(TEXT("registered")));
		TestFalse(TEXT("already_registered:false"), RegResult->GetBoolField(TEXT("already_registered")));

		FString RetSlotName;
		RegResult->TryGetStringField(TEXT("slot_name"), RetSlotName);
		TestEqual(TEXT("slot_name echoed"), RetSlotName, FString(TEXT("UpperBody")));
	}

	// Verify the slot is now actually in the skeleton
	TestTrue(TEXT("slot exists in skeleton after register"),
		Skel->ContainsSlotName(TEXT("UpperBody")));

	// --- Idempotency: call again → already_registered ---
	const TSharedPtr<FJsonObject> IdemResult = AgentMcpTestUtils::CallTool(*this,
		TEXT("register_skeleton_slot"),
		FString::Printf(
			TEXT("{\"skeleton_path\":\"%s\",\"slot_name\":\"UpperBody\"}"),
			*SkelPath),
		bIsError);

	TestFalse(TEXT("second register_skeleton_slot succeeds (no error)"), bIsError);
	if (TestNotNull(TEXT("idempotent result parses"), IdemResult.Get()))
	{
		TestFalse(TEXT("registered:false on second call"),  IdemResult->GetBoolField(TEXT("registered")));
		TestTrue(TEXT("already_registered:true on second"), IdemResult->GetBoolField(TEXT("already_registered")));
	}

	// --- Error path: missing skeleton ---
	const FString MissingSkelErr = AgentMcpTestUtils::CallToolRawText(*this,
		TEXT("register_skeleton_slot"),
		TEXT("{\"skeleton_path\":\"/Game/NoSuchSkeleton\",\"slot_name\":\"UpperBody\"}"),
		bIsError);
	TestTrue(TEXT("missing skeleton is error"), bIsError);
	TestFalse(TEXT("missing skeleton error non-empty"), MissingSkelErr.IsEmpty());

	// --- Error path: missing slot_name ---
	const FString MissingSlotErr = AgentMcpTestUtils::CallToolRawText(*this,
		TEXT("register_skeleton_slot"),
		FString::Printf(TEXT("{\"skeleton_path\":\"%s\"}"), *SkelPath),
		bIsError);
	TestTrue(TEXT("missing slot_name is error"), bIsError);

	return true;
}

// ---------------------------------------------------------------------------
// FAnimGraphSaveReloadTest
// Regression lock for the "AnimGraph nodes lost on editor reload" bug
// (ABP_Manny): a node spawned from an RF_Transient template inherited the flag
// through CreateNode's DuplicateObject; SavePackage silently skipped it, so
// the package saved "fine" but the graph loaded back without the node.
// Saves a real /Game package, then re-reads the file bytes into a separate
// /Temp diff package (LOAD_ForDiff — the editor's asset-diff technique; GC
// eviction is defeated by undo-buffer refs) and asserts the node is in the
// loaded copy.
// ---------------------------------------------------------------------------
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FAnimGraphSaveReloadTest,
	"UnrealAgentMCP.P8.AnimGraphSaveReload",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FAnimGraphSaveReloadTest::RunTest(const FString& Parameters)
{
	ON_SCOPE_EXIT { CleanupAnimRTAssets(*this); };

	bool bIsError = false;

	// --- Setup: real (non-transient) skeleton + AnimBlueprint packages ---
	UPackage* SkelPkg = CreatePackage(KAnimRTSkelPath);
	if (!TestNotNull(TEXT("RT: skeleton package"), SkelPkg)) { return false; }
	USkeleton* Skel = NewObject<USkeleton>(SkelPkg,
		FName(*FPackageName::GetShortName(KAnimRTSkelPath)),
		RF_Public | RF_Standalone | RF_Transactional);
	if (!TestNotNull(TEXT("RT: skeleton created"), Skel)) { return false; }
	Skel->MarkPackageDirty();

	UPackage* ABPPkg = CreatePackage(KAnimRTABPPath);
	if (!TestNotNull(TEXT("RT: ABP package"), ABPPkg)) { return false; }
	UAnimBlueprint* AnimBP = Cast<UAnimBlueprint>(
		FKismetEditorUtilities::CreateBlueprint(
			UAnimInstance::StaticClass(), ABPPkg,
			FName(*FPackageName::GetShortName(KAnimRTABPPath)),
			BPTYPE_Normal, UAnimBlueprint::StaticClass(),
			UAnimBlueprintGeneratedClass::StaticClass()));
	if (!TestNotNull(TEXT("RT: AnimBlueprint created"), AnimBP)) { return false; }
	AnimBP->TargetSkeleton = Skel;
	AnimBP->MarkPackageDirty();

	// Register the slot the node will reference, so compile-on-load stays quiet.
	AgentMcpTestUtils::CallTool(*this, TEXT("register_skeleton_slot"),
		FString::Printf(TEXT("{\"skeleton_path\":\"%s\",\"slot_name\":\"RTSlot\"}"), KAnimRTSkelPath),
		bIsError);
	TestFalse(TEXT("RT: register_skeleton_slot"), bIsError);

	// Baseline node count of the freshly created AnimGraph.
	auto FindAnimGraph = [](UAnimBlueprint* BP) -> UEdGraph*
	{
		for (UEdGraph* G : BP->FunctionGraphs)
		{
			if (G && G->GetName() == TEXT("AnimGraph")) { return G; }
		}
		return nullptr;
	};
	UEdGraph* Graph = FindAnimGraph(AnimBP);
	if (!TestNotNull(TEXT("RT: AnimGraph found"), Graph)) { return false; }
	const int32 BaselineCount = Graph->Nodes.Num();

	// --- Add a Slot node via the tool (full class path — avoids TryFindType short-name warning) ---
	const TSharedPtr<FJsonObject> AddResult = AgentMcpTestUtils::CallTool(*this,
		TEXT("add_anim_graph_node"),
		FString::Printf(
			TEXT("{\"blueprint_path\":\"%s\",\"node_class\":\"/Script/AnimGraph.AnimGraphNode_Slot\","
			     "\"properties\":{\"Node.SlotName\":\"RTSlot\"},\"pos_x\":300,\"pos_y\":200}"),
			KAnimRTABPPath),
		bIsError);
	if (!TestFalse(TEXT("RT: add_anim_graph_node succeeds"), bIsError)) { return false; }

	FString NodeId;
	if (AddResult.IsValid()) { AddResult->TryGetStringField(TEXT("node_id"), NodeId); }
	FGuid NodeGuid;
	FGuid::Parse(NodeId, NodeGuid);
	TestTrue(TEXT("RT: node_id is a valid GUID"), NodeGuid.IsValid());

	// --- Save both packages to disk ---
	AgentMcpTestUtils::CallTool(*this, TEXT("save_asset"),
		FString::Printf(TEXT("{\"asset_path\":\"%s\"}"), KAnimRTSkelPath), bIsError);
	if (!TestFalse(TEXT("RT: save skeleton"), bIsError)) { return false; }
	AgentMcpTestUtils::CallTool(*this, TEXT("save_asset"),
		FString::Printf(TEXT("{\"asset_path\":\"%s\"}"), KAnimRTABPPath), bIsError);
	if (!TestFalse(TEXT("RT: save ABP"), bIsError)) { return false; }

	// --- Re-read the saved file bytes into a separate /Temp diff package ---
	// GC eviction of the live ABP is unreliable here (the tool's FScopedTransaction leaves
	// undo-buffer references), so we read the file back the way the editor's asset diff does
	// (DiffUtils::LoadPackageForDiff): copy the file under Saved/ (the /Temp mount) and
	// LOAD_ForDiff it into its own package while the original stays in memory. With the
	// RF_Transient template bug the node is missing from these bytes — deterministic lock.
	const FString SrcFileName = FPackageName::LongPackageNameToFilename(
		KAnimRTABPPath, FPackageName::GetAssetPackageExtension());
	const FString TmpFileName = FPaths::ProjectSavedDir() + TEXT("McpAnimGraphRT_ReloadCheck.uasset");
	if (!TestTrue(TEXT("RT: copy saved package file to /Temp mount"),
		IFileManager::Get().Copy(*TmpFileName, *SrcFileName) == COPY_OK))
	{
		return false;
	}
	ON_SCOPE_EXIT
	{
		IFileManager::Get().Delete(*TmpFileName, /*RequireExists*/false, /*EvenReadOnly*/true, /*Quiet*/true);
	};

	const FPackagePath TempPackagePath     = FPackagePath::FromLocalPath(TmpFileName);
	const FPackagePath OriginalPackagePath = FPackagePath::FromLocalPath(SrcFileName);
	if (!TestTrue(TEXT("RT: temp package path resolves under /Temp"),
		FPackageName::IsTempPackage(TempPackagePath.GetPackageName())))
	{
		return false;
	}

	// Remap self-references from the original package name to the temp package name.
	FLinkerInstancingContext InstancingContext;
	InstancingContext.AddPackageMapping(
		OriginalPackagePath.GetPackageFName(), TempPackagePath.GetPackageFName());

	UPackage* LoadedPkg = LoadPackage(nullptr, *TempPackagePath.GetPackageName(),
		LOAD_ForDiff | LOAD_DisableCompileOnLoad | LOAD_DisableEngineVersionChecks,
		nullptr, &InstancingContext);
	if (!TestNotNull(TEXT("RT: package re-loaded from disk bytes"), LoadedPkg)) { return false; }

	UAnimBlueprint* Reloaded = FindObject<UAnimBlueprint>(LoadedPkg,
		*FPackageName::GetShortName(KAnimRTABPPath));
	if (!TestNotNull(TEXT("RT: ABP found in re-loaded package"), Reloaded)) { return false; }

	UEdGraph* ReloadedGraph = FindAnimGraph(Reloaded);
	if (!TestNotNull(TEXT("RT: reloaded AnimGraph found"), ReloadedGraph)) { return false; }

	TestEqual(TEXT("RT: node count survives save->reload"),
		ReloadedGraph->Nodes.Num(), BaselineCount + 1);

	const UEdGraphNode* Survivor = nullptr;
	for (const UEdGraphNode* N : ReloadedGraph->Nodes)
	{
		if (N && N->NodeGuid == NodeGuid) { Survivor = N; break; }
	}
	if (TestNotNull(TEXT("RT: added node (by GUID) survives save->reload"), Survivor))
	{
		TestTrue(TEXT("RT: survivor is a Slot node"),
			Survivor->GetClass()->GetName().Contains(TEXT("Slot"), ESearchCase::IgnoreCase));
	}

	return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS
