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
#include "Kismet2/KismetEditorUtilities.h"
#include "Misc/ScopeExit.h"
#include "Tests/AgentMcpTestHelpers.h"
#include "UObject/Package.h"

// ---------------------------------------------------------------------------
// Constants
// ---------------------------------------------------------------------------

namespace
{
	// Transient paths — never touch disk; GC'd after test run.
	constexpr const TCHAR* KAnimBPName = TEXT("ABP_McpAnimGraphTest");
	constexpr const TCHAR* KSkelName   = TEXT("SK_McpAnimGraphTestSkel");
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

#endif // WITH_DEV_AUTOMATION_TESTS
