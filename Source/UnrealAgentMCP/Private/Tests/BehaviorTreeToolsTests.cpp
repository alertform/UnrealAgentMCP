#include "Misc/AutomationTest.h"

#if WITH_DEV_AUTOMATION_TESTS

#include "AgentMcpSettings.h"
#include "BehaviorTree/BehaviorTree.h"
#include "BehaviorTree/BlackboardData.h"
#include "BehaviorTree/Blackboard/BlackboardKeyType_Object.h"
#include "BehaviorTree/BTCompositeNode.h"
#include "BehaviorTree/BTDecorator.h"
#include "BehaviorTree/BTTaskNode.h"
#include "Core/AgentMcpTier.h"
#include "Core/AgentMcpToolRegistry.h"
#include "Core/McpTypes.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "Misc/ScopeExit.h"
#include "Tests/AgentMcpTestHelpers.h"
#include "UObject/Package.h"
#include "UObject/SavePackage.h"

// ---------------------------------------------------------------------------
// Constants
// ---------------------------------------------------------------------------

namespace
{
	constexpr const TCHAR* KBBPath  = TEXT("/Game/Dev_Test/BB_McpBTTest");
	constexpr const TCHAR* KBTPath  = TEXT("/Game/Dev_Test/BT_McpBTTest");
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

#endif // WITH_DEV_AUTOMATION_TESTS
