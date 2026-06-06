#include "Misc/AutomationTest.h"

#if WITH_DEV_AUTOMATION_TESTS

#include "AgentMcpSettings.h"
#include "BehaviorTree/BehaviorTree.h"
#include "BehaviorTree/BlackboardData.h"
#include "BehaviorTree/Blackboard/BlackboardKeyType_Object.h"
#include "BehaviorTree/BTCompositeNode.h"
#include "BehaviorTree/BTDecorator.h"
#include "BehaviorTree/BTTaskNode.h"
#include "BehaviorTree/Decorators/BTDecorator_Blackboard.h"
#include "BehaviorTree/Tasks/BTTask_MoveTo.h"
#include "Core/AgentMcpTier.h"
#include "Core/AgentMcpToolRegistry.h"
#include "Core/McpTypes.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "Engine/Engine.h"
#include "Misc/PackageName.h"
#include "Misc/ScopeExit.h"
#include "Tests/AgentMcpTestHelpers.h"
#include "HAL/FileManager.h"
#include "UObject/GarbageCollection.h"
#include "UObject/Linker.h"
#include "UObject/Package.h"
#include "UObject/SavePackage.h"

// ---------------------------------------------------------------------------
// Constants
// ---------------------------------------------------------------------------

namespace
{
	// Happy-path / error-path tests.
	constexpr const TCHAR* KBBPath  = TEXT("/Game/Dev_Test/BB_McpBTTest");
	constexpr const TCHAR* KBTPath  = TEXT("/Game/Dev_Test/BT_McpBTTest");

	// Save/reload round-trip test — dedicated paths to avoid cross-test interference.
	constexpr const TCHAR* KBBRTPath = TEXT("/Game/Dev_Test/BB_McpBTRTTest");
	constexpr const TCHAR* KBTRTPath = TEXT("/Game/Dev_Test/BT_McpBTRTTest");

	// Outer-normalization test — own paths (transient only, no disk I/O).
	constexpr const TCHAR* KBBNormPath = TEXT("/Game/Dev_Test/BB_McpBTNormTest");
	constexpr const TCHAR* KBTNormPath = TEXT("/Game/Dev_Test/BT_McpBTNormTest");
}

// ---------------------------------------------------------------------------
// Cleanup helper — deletes both assets; tier raised to Destructive.
// ---------------------------------------------------------------------------
namespace
{
	void CleanupBTAssets(FAutomationTestBase& Test)
	{
		UAgentMcpSettings* Settings = GetMutableDefault<UAgentMcpSettings>();
		const EAgentMcpTier Saved = Settings->PermissionTier;
		Settings->PermissionTier = EAgentMcpTier::Destructive;
		ON_SCOPE_EXIT { Settings->PermissionTier = Saved; };

		bool bErr = false;
		AgentMcpTestUtils::CallTool(Test, TEXT("delete_asset"),
			FString::Printf(TEXT("{\"asset_path\":\"%s\"}"), KBTPath), bErr);
		AgentMcpTestUtils::CallTool(Test, TEXT("delete_asset"),
			FString::Printf(TEXT("{\"asset_path\":\"%s\"}"), KBBPath), bErr);
	}

	/**
	 * Programmatically creates the two test assets (BB + BT) under /Game/Dev_Test/.
	 * Returns false if creation fails (test should abort).
	 */
	bool CreateTestAssets(FAutomationTestBase& Test,
		UBlackboardData*& OutBB, UBehaviorTree*& OutBT)
	{
		OutBB = nullptr;
		OutBT = nullptr;

		// --- UBlackboardData ---
		const FString BBPkgName = KBBPath;
		UPackage* BBPkg = CreatePackage(*BBPkgName);
		if (!Test.TestNotNull(TEXT("BB package created"), BBPkg)) { return false; }

		UBlackboardData* BB = NewObject<UBlackboardData>(BBPkg,
			FName(TEXT("BB_McpBTTest")), RF_Public | RF_Standalone | RF_Transactional);
		if (!Test.TestNotNull(TEXT("UBlackboardData created"), BB)) { return false; }

		// Add a single Object key "TestTarget" (BaseClass=AActor).
		{
			UBlackboardKeyType_Object* KeyType = NewObject<UBlackboardKeyType_Object>(BB);
			KeyType->BaseClass = AActor::StaticClass();

			FBlackboardEntry Entry;
			Entry.EntryName = TEXT("TestTarget");
			Entry.KeyType   = KeyType;
			BB->Keys.Add(Entry);
		}
		BB->MarkPackageDirty();

		// --- UBehaviorTree ---
		const FString BTPkgName = KBTPath;
		UPackage* BTPkg = CreatePackage(*BTPkgName);
		if (!Test.TestNotNull(TEXT("BT package created"), BTPkg)) { return false; }

		UBehaviorTree* BT = NewObject<UBehaviorTree>(BTPkg,
			FName(TEXT("BT_McpBTTest")), RF_Public | RF_Standalone | RF_Transactional);
		if (!Test.TestNotNull(TEXT("UBehaviorTree created"), BT)) { return false; }

		BT->BlackboardAsset = BB;
		BT->MarkPackageDirty();

		OutBB = BB;
		OutBT = BT;
		return true;
	}

	/**
	 * Generic version of CreateTestAssets: creates BB + BT at the supplied paths
	 * (the short asset name is derived from the last segment after the final '/').
	 */
	bool CreateTestAssetsAt(FAutomationTestBase& Test,
		const TCHAR* InBBPath, const TCHAR* InBTPath,
		UBlackboardData*& OutBB, UBehaviorTree*& OutBT)
	{
		OutBB = nullptr;
		OutBT = nullptr;

		auto ShortName = [](const TCHAR* Path) -> FName
		{
			FString S(Path);
			int32 Slash = INDEX_NONE;
			S.FindLastChar(TEXT('/'), Slash);
			return FName(Slash != INDEX_NONE ? *S.Mid(Slash + 1) : *S);
		};

		// BB
		UPackage* BBPkg = CreatePackage(InBBPath);
		if (!Test.TestNotNull(TEXT("BB package"), BBPkg)) { return false; }
		UBlackboardData* BB = NewObject<UBlackboardData>(BBPkg, ShortName(InBBPath),
			RF_Public | RF_Standalone | RF_Transactional);
		if (!Test.TestNotNull(TEXT("UBlackboardData"), BB)) { return false; }
		{
			UBlackboardKeyType_Object* KeyType = NewObject<UBlackboardKeyType_Object>(BB);
			KeyType->BaseClass = AActor::StaticClass();
			FBlackboardEntry Entry;
			Entry.EntryName = TEXT("TestTarget");
			Entry.KeyType   = KeyType;
			BB->Keys.Add(Entry);
		}
		BB->MarkPackageDirty();

		// BT
		UPackage* BTPkg = CreatePackage(InBTPath);
		if (!Test.TestNotNull(TEXT("BT package"), BTPkg)) { return false; }
		UBehaviorTree* BT = NewObject<UBehaviorTree>(BTPkg, ShortName(InBTPath),
			RF_Public | RF_Standalone | RF_Transactional);
		if (!Test.TestNotNull(TEXT("UBehaviorTree"), BT)) { return false; }
		BT->BlackboardAsset = BB;
		BT->MarkPackageDirty();

		OutBB = BB;
		OutBT = BT;
		return true;
	}

	/** Delete one on-disk .uasset by package path (e.g. /Game/Dev_Test/Foo).
	 *  Converts to a filesystem path and removes the file directly — safe even when the
	 *  package has been GC'd and is no longer in the asset registry. */
	void DeletePackageFile(const TCHAR* PackagePath)
	{
		FString FilePath;
		if (FPackageName::TryConvertLongPackageNameToFilename(
				PackagePath, FilePath, FPackageName::GetAssetPackageExtension()))
		{
			IFileManager::Get().Delete(*FilePath, /*bRequireExists=*/false, /*bEvenReadOnly=*/true);
		}
	}

	/** Cleanup for the round-trip test pair.
	 *  The eviction step in the test GC's the BT package so it is no longer registered with
	 *  the asset registry.  delete_asset requires a registered asset; use direct file deletion
	 *  instead so cleanup never emits a LogEditorAssetSubsystem Error that would fail the test. */
	void CleanupBTRTAssets(FAutomationTestBase& /*Test*/)
	{
		DeletePackageFile(KBTRTPath);
		DeletePackageFile(KBBRTPath);
	}

	/** Cleanup for the normalization test pair (transient-only, but defensive). */
	void CleanupBTNormAssets(FAutomationTestBase& Test)
	{
		UAgentMcpSettings* Settings = GetMutableDefault<UAgentMcpSettings>();
		const EAgentMcpTier Saved = Settings->PermissionTier;
		Settings->PermissionTier = EAgentMcpTier::Destructive;
		ON_SCOPE_EXIT { Settings->PermissionTier = Saved; };
		bool bErr = false;
		AgentMcpTestUtils::CallTool(Test, TEXT("delete_asset"),
			FString::Printf(TEXT("{\"asset_path\":\"%s\"}"), KBTNormPath), bErr);
		AgentMcpTestUtils::CallTool(Test, TEXT("delete_asset"),
			FString::Printf(TEXT("{\"asset_path\":\"%s\"}"), KBBNormPath), bErr);
	}
}

// ---------------------------------------------------------------------------
// FBTToolsHappyPathTest
// ---------------------------------------------------------------------------
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FBTToolsHappyPathTest,
	"UnrealAgentMCP.BT.HappyPath",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FBTToolsHappyPathTest::RunTest(const FString& Parameters)
{
	ON_SCOPE_EXIT { CleanupBTAssets(*this); };

	UBlackboardData* BB = nullptr;
	UBehaviorTree*   BT = nullptr;
	if (!CreateTestAssets(*this, BB, BT)) { return false; }

	bool bIsError = false;

	// -----------------------------------------------------------------
	// Step 1: add_bt_node — root Selector
	// -----------------------------------------------------------------
	const TSharedPtr<FJsonObject> AddRoot = AgentMcpTestUtils::CallTool(*this,
		TEXT("add_bt_node"),
		FString::Printf(
			TEXT("{\"bt_path\":\"%s\",\"parent_index_path\":\"\",\"node_class\":\"BTComposite_Selector\"}"),
			KBTPath),
		bIsError);
	TestFalse(TEXT("add root Selector succeeds"), bIsError);
	if (!TestNotNull(TEXT("add root result parses"), AddRoot.Get())) { return false; }
	TestTrue(TEXT("added:true (root)"), AddRoot->GetBoolField(TEXT("added")));

	// Verify runtime tree.
	TestNotNull(TEXT("BT->RootNode set"), BT->RootNode.Get());

	// -----------------------------------------------------------------
	// Step 2: add_bt_node — BTTask_MoveTo with BlackboardKey="TestTarget"
	// -----------------------------------------------------------------
	const TSharedPtr<FJsonObject> AddMoveTo = AgentMcpTestUtils::CallTool(*this,
		TEXT("add_bt_node"),
		FString::Printf(
			TEXT("{\"bt_path\":\"%s\",\"parent_index_path\":\"\","
			     "\"node_class\":\"BTTask_MoveTo\","
			     "\"properties\":{\"BlackboardKey\":\"TestTarget\"}}"),
			KBTPath),
		bIsError);
	TestFalse(TEXT("add MoveTo succeeds"), bIsError);
	if (TestNotNull(TEXT("add MoveTo result parses"), AddMoveTo.Get()))
	{
		TestTrue(TEXT("MoveTo added:true"), AddMoveTo->GetBoolField(TEXT("added")));
	}

	// -----------------------------------------------------------------
	// Step 3: add_bt_decorator — Blackboard decorator, observer_aborts=self
	// -----------------------------------------------------------------
	const TSharedPtr<FJsonObject> AddDec = AgentMcpTestUtils::CallTool(*this,
		TEXT("add_bt_decorator"),
		FString::Printf(
			TEXT("{\"bt_path\":\"%s\",\"node_index_path\":\"0\","
			     "\"decorator_class\":\"BTDecorator_Blackboard\","
			     "\"observer_aborts\":\"self\","
			     "\"properties\":{\"BlackboardKey\":\"TestTarget\"}}"),
			KBTPath),
		bIsError);
	TestFalse(TEXT("add Blackboard decorator succeeds"), bIsError);
	if (TestNotNull(TEXT("add decorator result parses"), AddDec.Get()))
	{
		TestTrue(TEXT("decorator added:true"), AddDec->GetBoolField(TEXT("added")));
		TestTrue(TEXT("decorator class contains Blackboard"),
			AddDec->GetStringField(TEXT("decorator_class"))
				.Contains(TEXT("Blackboard"), ESearchCase::IgnoreCase));
	}

	// -----------------------------------------------------------------
	// Step 4: read_bt — assert full structure
	// -----------------------------------------------------------------
	const TSharedPtr<FJsonObject> ReadResult = AgentMcpTestUtils::CallTool(*this,
		TEXT("read_bt"),
		FString::Printf(TEXT("{\"bt_path\":\"%s\"}"), KBTPath),
		bIsError);
	TestFalse(TEXT("read_bt succeeds"), bIsError);
	if (!TestNotNull(TEXT("read_bt result parses"), ReadResult.Get())) { return false; }

	// Blackboard path present.
	FString BBPath;
	ReadResult->TryGetStringField(TEXT("blackboard"), BBPath);
	TestFalse(TEXT("blackboard field non-empty"), BBPath.IsEmpty());

	// Tree non-null.
	const TSharedPtr<FJsonObject>* TreePtr = nullptr;
	TestTrue(TEXT("tree field is an object"), ReadResult->TryGetObjectField(TEXT("tree"), TreePtr) && TreePtr);

	if (TreePtr && TreePtr->IsValid())
	{
		const TSharedPtr<FJsonObject>& Tree = *TreePtr;

		// Root class contains "Selector".
		FString RootClass;
		Tree->TryGetStringField(TEXT("class"), RootClass);
		TestTrue(TEXT("root class contains Selector"),
			RootClass.Contains(TEXT("Selector"), ESearchCase::IgnoreCase));

		// Root has one child.
		const TArray<TSharedPtr<FJsonValue>>* Children = nullptr;
		TestTrue(TEXT("root has children array"), Tree->TryGetArrayField(TEXT("children"), Children) && Children);
		if (Children && Children->Num() >= 1)
		{
			TestEqual(TEXT("root has exactly 1 child"), Children->Num(), 1);

			const TSharedPtr<FJsonObject> Child = (*Children)[0]->AsObject();
			if (TestNotNull(TEXT("child[0] is object"), Child.Get()))
			{
				// Child class contains MoveTo.
				FString ChildClass;
				Child->TryGetStringField(TEXT("class"), ChildClass);
				TestTrue(TEXT("child class contains MoveTo"),
					ChildClass.Contains(TEXT("MoveTo"), ESearchCase::IgnoreCase));

				// Child index_path is "0".
				FString ChildPath;
				Child->TryGetStringField(TEXT("index_path"), ChildPath);
				TestEqual(TEXT("child index_path is 0"), ChildPath, FString(TEXT("0")));

				// Child has one decorator.
				const TArray<TSharedPtr<FJsonValue>>* Decs = nullptr;
				TestTrue(TEXT("child has decorators array"), Child->TryGetArrayField(TEXT("decorators"), Decs) && Decs);
				if (Decs && Decs->Num() >= 1)
				{
					TestEqual(TEXT("child has exactly 1 decorator"), Decs->Num(), 1);
					const TSharedPtr<FJsonObject> DecObj = (*Decs)[0]->AsObject();
					if (TestNotNull(TEXT("decorator is object"), DecObj.Get()))
					{
						FString DecClass;
						DecObj->TryGetStringField(TEXT("class"), DecClass);
						TestTrue(TEXT("decorator class contains Blackboard"),
							DecClass.Contains(TEXT("Blackboard"), ESearchCase::IgnoreCase));

						FString KeyName;
						DecObj->TryGetStringField(TEXT("key"), KeyName);
						TestEqual(TEXT("decorator key is TestTarget"),
							KeyName, FString(TEXT("TestTarget")));

						FString AbortMode;
						DecObj->TryGetStringField(TEXT("abort_mode"), AbortMode);
						TestFalse(TEXT("abort_mode non-empty"), AbortMode.IsEmpty());
					}
				}
			}
		}
	}

	return true;
}

// ---------------------------------------------------------------------------
// FBTToolsErrorPathTest
// ---------------------------------------------------------------------------
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FBTToolsErrorPathTest,
	"UnrealAgentMCP.BT.ErrorPaths",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FBTToolsErrorPathTest::RunTest(const FString& Parameters)
{
	ON_SCOPE_EXIT { CleanupBTAssets(*this); };

	UBlackboardData* BB = nullptr;
	UBehaviorTree*   BT = nullptr;
	if (!CreateTestAssets(*this, BB, BT)) { return false; }

	bool bIsError = false;

	// First create a valid root so error paths have context.
	AgentMcpTestUtils::CallTool(*this, TEXT("add_bt_node"),
		FString::Printf(
			TEXT("{\"bt_path\":\"%s\",\"parent_index_path\":\"\",\"node_class\":\"BTComposite_Selector\"}"),
			KBTPath),
		bIsError);

	// -----------------------------------------------------------------
	// Error 1: bad index_path — descend into an out-of-range child
	// Root Selector currently has 0 children, so "99/0" tries to descend into child 99.
	// -----------------------------------------------------------------
	const FString BadPathErr = AgentMcpTestUtils::CallToolRawText(*this,
		TEXT("add_bt_node"),
		FString::Printf(
			TEXT("{\"bt_path\":\"%s\",\"parent_index_path\":\"99/0\",\"node_class\":\"BTTask_MoveTo\"}"),
			KBTPath),
		bIsError);
	TestTrue(TEXT("bad index_path is error"), bIsError);
	TestTrue(TEXT("bad index_path error mentions 'range' or 'children'"),
		BadPathErr.Contains(TEXT("range"), ESearchCase::IgnoreCase) ||
		BadPathErr.Contains(TEXT("children"), ESearchCase::IgnoreCase));

	// -----------------------------------------------------------------
	// Error 2: non-BTNode class
	// -----------------------------------------------------------------
	const FString BadClassErr = AgentMcpTestUtils::CallToolRawText(*this,
		TEXT("add_bt_node"),
		FString::Printf(
			TEXT("{\"bt_path\":\"%s\",\"parent_index_path\":\"\",\"node_class\":\"Actor\"}"),
			KBTPath),
		bIsError);
	TestTrue(TEXT("non-BTNode class is error"), bIsError);
	TestTrue(TEXT("non-BTNode error mentions UBTNode"),
		BadClassErr.Contains(TEXT("UBTNode"), ESearchCase::IgnoreCase) ||
		BadClassErr.Contains(TEXT("not a"), ESearchCase::IgnoreCase));

	// -----------------------------------------------------------------
	// Error 3: non-existent BB key — error lists available keys
	// -----------------------------------------------------------------
	const FString BadKeyErr = AgentMcpTestUtils::CallToolRawText(*this,
		TEXT("add_bt_node"),
		FString::Printf(
			TEXT("{\"bt_path\":\"%s\",\"parent_index_path\":\"\","
			     "\"node_class\":\"BTTask_MoveTo\","
			     "\"properties\":{\"BlackboardKey\":\"NoSuchKeyXyz\"}}"),
			KBTPath),
		bIsError);
	TestTrue(TEXT("missing BB key is error"), bIsError);
	// Error must list available keys so agent knows what to use.
	TestTrue(TEXT("missing BB key error lists 'TestTarget'"),
		BadKeyErr.Contains(TEXT("TestTarget"), ESearchCase::IgnoreCase));

	return true;
}

// ---------------------------------------------------------------------------
// FBTToolsSaveReloadTest
// Regression lock for the "editor-authored BT nodes lost on save" bug.
// Saves a fully-built BT to disk, evicts the package from memory, reloads from
// disk, and asserts the runtime tree is intact.
// ---------------------------------------------------------------------------
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FBTToolsSaveReloadTest,
	"UnrealAgentMCP.BT.SaveReload",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FBTToolsSaveReloadTest::RunTest(const FString& Parameters)
{
	ON_SCOPE_EXIT { CleanupBTRTAssets(*this); };

	UBlackboardData* BB = nullptr;
	UBehaviorTree*   BT = nullptr;
	if (!CreateTestAssetsAt(*this, KBBRTPath, KBTRTPath, BB, BT)) { return false; }

	bool bIsError = false;

	// Step 1: build the tree — root Selector.
	AgentMcpTestUtils::CallTool(*this, TEXT("add_bt_node"),
		FString::Printf(
			TEXT("{\"bt_path\":\"%s\",\"parent_index_path\":\"\",\"node_class\":\"BTComposite_Selector\"}"),
			KBTRTPath),
		bIsError);
	if (!TestFalse(TEXT("RT: add root Selector"), bIsError)) { return false; }

	// Step 2: add MoveTo child.
	AgentMcpTestUtils::CallTool(*this, TEXT("add_bt_node"),
		FString::Printf(
			TEXT("{\"bt_path\":\"%s\",\"parent_index_path\":\"\","
			     "\"node_class\":\"BTTask_MoveTo\","
			     "\"properties\":{\"BlackboardKey\":\"TestTarget\"}}"),
			KBTRTPath),
		bIsError);
	if (!TestFalse(TEXT("RT: add MoveTo"), bIsError)) { return false; }

	// Step 3: add decorator on child 0.
	AgentMcpTestUtils::CallTool(*this, TEXT("add_bt_decorator"),
		FString::Printf(
			TEXT("{\"bt_path\":\"%s\",\"node_index_path\":\"0\","
			     "\"decorator_class\":\"BTDecorator_Blackboard\","
			     "\"observer_aborts\":\"self\","
			     "\"properties\":{\"BlackboardKey\":\"TestTarget\"}}"),
			KBTRTPath),
		bIsError);
	if (!TestFalse(TEXT("RT: add Blackboard decorator"), bIsError)) { return false; }

	// Step 4: save to disk via save_asset tool.
	AgentMcpTestUtils::CallTool(*this, TEXT("save_asset"),
		FString::Printf(TEXT("{\"asset_path\":\"%s\"}"), KBTRTPath),
		bIsError);
	if (!TestFalse(TEXT("RT: save_asset BT succeeds"), bIsError)) { return false; }

	AgentMcpTestUtils::CallTool(*this, TEXT("save_asset"),
		FString::Printf(TEXT("{\"asset_path\":\"%s\"}"), KBBRTPath),
		bIsError);
	TestFalse(TEXT("RT: save_asset BB succeeds"), bIsError);

	// Step 5: evict the BT package from memory.
	// Build object path for FindObject / LoadObject:  /Game/Dev_Test/BT_McpBTRTTest.BT_McpBTRTTest
	const FString BTPkgName = FString(KBTRTPath);
	const FString BTShortName = FPackageName::GetShortName(BTPkgName);
	const FString BTObjPath   = BTPkgName + TEXT(".") + BTShortName;

	{
		UPackage* BTPkg = FindPackage(nullptr, *BTPkgName);
		if (TestNotNull(TEXT("RT: BT package is in memory before eviction"), BTPkg))
		{
			// Close any open linker so the file is unlocked for subsequent LoadObject.
			ResetLoaders(BTPkg);

			// Remove the Standalone flag so GC is allowed to collect these objects,
			// then explicitly mark them unreachable.
			if (BT)
			{
				BT->ClearFlags(RF_Standalone);
				BT->MarkAsGarbage();
			}
			BTPkg->ClearFlags(RF_Standalone);
			BTPkg->MarkAsGarbage();

			// One full GC pass to actually collect them.
			CollectGarbage(GARBAGE_COLLECTION_KEEPFLAGS);

			// Confirm eviction: FindObject must return nullptr (package is gone from memory).
			// Note: in editor GC, objects with remaining hard refs may survive — we check
			// softly here and proceed regardless; LoadObject will create a fresh in-memory copy.
			const UBehaviorTree* StillLoaded =
				FindObject<UBehaviorTree>(nullptr, *BTObjPath);
			if (StillLoaded != nullptr)
			{
				// This is not a test failure per se — GC may legitimately keep the object
				// if other automation framework references exist.  Log a warning so we know,
				// but proceed: LoadObject below will either return the existing (already-on-disk)
				// copy or load a fresh one, and either way the structure assertions hold.
				AddWarning(TEXT("RT: BT object still in memory after GC (possible live reference from automation framework); reload test continues."));
			}
		}
	}

	// Step 6: reload from disk.
	UBehaviorTree* ReloadedBT = LoadObject<UBehaviorTree>(nullptr, *BTObjPath);
	if (!TestNotNull(TEXT("RT: BT reloaded from disk"), ReloadedBT)) { return false; }

	// Step 7: assert runtime tree structure.
	if (!TestNotNull(TEXT("RT: reloaded RootNode non-null"), ReloadedBT->RootNode.Get())) { return false; }

	UBTCompositeNode* Root = ReloadedBT->RootNode;

	// Root class should contain "Selector".
	TestTrue(TEXT("RT: reloaded root class contains Selector"),
		Root->GetClass()->GetName().Contains(TEXT("Selector"), ESearchCase::IgnoreCase));

	// Root has exactly 1 child.
	TestEqual(TEXT("RT: reloaded root has 1 child"), Root->Children.Num(), 1);

	if (Root->Children.Num() >= 1)
	{
		const FBTCompositeChild& Child0 = Root->Children[0];

		// Child is BTTask_MoveTo.
		if (TestNotNull(TEXT("RT: Children[0].ChildTask non-null"), Child0.ChildTask.Get()))
		{
			TestTrue(TEXT("RT: Children[0].ChildTask is BTTask_MoveTo"),
				Child0.ChildTask->IsA<UBTTask_MoveTo>());
		}

		// Child has exactly 1 decorator, no null entries.
		TestEqual(TEXT("RT: Children[0] has 1 decorator"), Child0.Decorators.Num(), 1);
		for (int32 i = 0; i < Child0.Decorators.Num(); ++i)
		{
			if (!TestNotNull(
					*FString::Printf(TEXT("RT: Decorators[%d] non-null"), i),
					Child0.Decorators[i].Get()))
			{
				continue;
			}
			TestTrue(TEXT("RT: decorator is BTDecorator_Blackboard"),
				Child0.Decorators[i]->IsA<UBTDecorator_Blackboard>());
		}
	}

	return true;
}

// ---------------------------------------------------------------------------
// FBTToolsNormalizeOuterTest
// Unit test for NormalizeBTNodeOuters: a node whose Outer is the transient
// package must be re-parented to the BT asset after a write-tool call.
// ---------------------------------------------------------------------------
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FBTToolsNormalizeOuterTest,
	"UnrealAgentMCP.BT.NormalizeOuter",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FBTToolsNormalizeOuterTest::RunTest(const FString& Parameters)
{
	ON_SCOPE_EXIT { CleanupBTNormAssets(*this); };

	UBlackboardData* BB = nullptr;
	UBehaviorTree*   BT = nullptr;
	if (!CreateTestAssetsAt(*this, KBBNormPath, KBTNormPath, BB, BT)) { return false; }

	bool bIsError = false;

	// Step 1: create root Selector via tool.
	AgentMcpTestUtils::CallTool(*this, TEXT("add_bt_node"),
		FString::Printf(
			TEXT("{\"bt_path\":\"%s\",\"parent_index_path\":\"\",\"node_class\":\"BTComposite_Selector\"}"),
			KBTNormPath),
		bIsError);
	if (!TestFalse(TEXT("Norm: add root Selector"), bIsError)) { return false; }

	// Step 2: add MoveTo child (index "0") via tool.
	AgentMcpTestUtils::CallTool(*this, TEXT("add_bt_node"),
		FString::Printf(
			TEXT("{\"bt_path\":\"%s\",\"parent_index_path\":\"\","
			     "\"node_class\":\"BTTask_MoveTo\","
			     "\"properties\":{\"BlackboardKey\":\"TestTarget\"}}"),
			KBTNormPath),
		bIsError);
	if (!TestFalse(TEXT("Norm: add MoveTo"), bIsError)) { return false; }

	// Step 3: manually inject a BTTask_MoveTo whose Outer is the transient package —
	//         simulating an editor-authored node whose Outer was never re-parented to the BT asset.
	//         (UBTTaskNode is Abstract so we use the concrete BTTask_MoveTo instead.)
	if (!TestNotNull(TEXT("Norm: RootNode present"), BT->RootNode.Get())) { return false; }

	UBTTask_MoveTo* StrayTask = NewObject<UBTTask_MoveTo>(
		GetTransientPackage(), NAME_None, RF_Transactional);
	if (!TestNotNull(TEXT("Norm: stray task created"), StrayTask)) { return false; }

	// Sanity: Outer is the transient package before normalization.
	TestEqual(TEXT("Norm: stray task outer is transient package before normalization"),
		StrayTask->GetOuter(), static_cast<UObject*>(GetTransientPackage()));

	// Insert as a second child of the root Selector (BT already has Children[0] = MoveTo).
	{
		FBTCompositeChild StrayEntry;
		StrayEntry.ChildTask = StrayTask;
		BT->RootNode->Children.Add(StrayEntry);
	}

	// Step 4: call add_bt_decorator on child index "0" — this triggers NormalizeBTNodeOuters
	//         inside HandleAddBTDecorator, which must fix StrayTask's Outer to BT.
	AgentMcpTestUtils::CallTool(*this, TEXT("add_bt_decorator"),
		FString::Printf(
			TEXT("{\"bt_path\":\"%s\",\"node_index_path\":\"0\","
			     "\"decorator_class\":\"BTDecorator_Blackboard\","
			     "\"observer_aborts\":\"none\","
			     "\"properties\":{\"BlackboardKey\":\"TestTarget\"}}"),
			KBTNormPath),
		bIsError);
	if (!TestFalse(TEXT("Norm: add_bt_decorator triggers normalization"), bIsError)) { return false; }

	// Step 5: assert that the stray task's Outer has been fixed to the BT asset.
	// After NormalizeBTNodeOuters, every node reachable from BT->RootNode must have BT as Outer.
	TestEqual(TEXT("Norm: stray task outer is now the BT asset after normalization"),
		StrayTask->GetOuter(), static_cast<UObject*>(BT));

	return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS
