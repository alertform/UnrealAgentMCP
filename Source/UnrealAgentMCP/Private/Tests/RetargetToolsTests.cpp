#include "Misc/AutomationTest.h"

#if WITH_DEV_AUTOMATION_TESTS

#include "AgentMcpSettings.h"
#include "Animation/AnimSequence.h"
#include "Animation/Skeleton.h"
#include "AnimDataController.h"
#include "Animation/AnimData/IAnimationDataModel.h"
#include "Core/AgentMcpTier.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "Misc/ScopeExit.h"
#include "Tests/AgentMcpTestHelpers.h"

namespace
{
	// Both meshes share SK_Mannequin but are distinct assets — exercises the full pipeline
	// (two auto-characterized IK rigs + retargeter + batch op) on guaranteed-present content.
	constexpr const TCHAR* KSrcMesh = TEXT("/Game/Characters/Mannequins/Meshes/SKM_Quinn_Simple");
	constexpr const TCHAR* KTgtMesh = TEXT("/Game/Characters/Mannequins/Meshes/SKM_Manny_Simple");
	constexpr const TCHAR* KIdleAnim = TEXT("/Game/Characters/Mannequins/Animations/Manny/MM_Idle");
	constexpr const TCHAR* KOutDir = TEXT("/Game/Dev_Test/RetargetOut");

	void CleanupAssetRT(FAutomationTestBase& Test, const FString& Path)
	{
		UAgentMcpSettings* Settings = GetMutableDefault<UAgentMcpSettings>();
		const EAgentMcpTier Saved = Settings->PermissionTier;
		Settings->PermissionTier = EAgentMcpTier::Destructive;
		ON_SCOPE_EXIT { Settings->PermissionTier = Saved; };
		bool bErr = false;
		AgentMcpTestUtils::CallTool(Test, TEXT("delete_asset"),
			FString::Printf(TEXT("{\"asset_path\":\"%s\",\"force\":true}"), *Path), bErr);
	}
}

// ---------------------------------------------------------------------------
// Error paths: missing args / non-/Game output.
// ---------------------------------------------------------------------------
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FRetargetAnimationsErrorPathsTest,
	"UnrealAgentMCP.Retarget.ErrorPaths",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FRetargetAnimationsErrorPathsTest::RunTest(const FString& Parameters)
{
	bool bIsError = false;

	const FString NoSrc = AgentMcpTestUtils::CallToolRawText(*this, TEXT("retarget_animations"),
		TEXT("{}"), bIsError);
	TestTrue(TEXT("missing source_mesh is error"), bIsError);
	TestTrue(TEXT("error names source_mesh"), NoSrc.Contains(TEXT("source_mesh")));

	const FString NoAnims = AgentMcpTestUtils::CallToolRawText(*this, TEXT("retarget_animations"),
		FString::Printf(TEXT("{\"source_mesh\":\"%s\",\"target_mesh\":\"%s\",\"output_path\":\"%s\",\"animations\":[]}"),
			KSrcMesh, KTgtMesh, KOutDir),
		bIsError);
	TestTrue(TEXT("empty animations is error"), bIsError);
	TestTrue(TEXT("error names animations"), NoAnims.Contains(TEXT("animations")));

	const FString BadOut = AgentMcpTestUtils::CallToolRawText(*this, TEXT("retarget_animations"),
		FString::Printf(TEXT("{\"source_mesh\":\"%s\",\"target_mesh\":\"%s\",\"output_path\":\"/Engine/Bad\",\"animations\":[\"%s\"]}"),
			KSrcMesh, KTgtMesh, KIdleAnim),
		bIsError);
	TestTrue(TEXT("non-/Game output_path is error"), bIsError);
	TestTrue(TEXT("error names /Game"), BadOut.Contains(TEXT("/Game")));

	const FString BadMesh = AgentMcpTestUtils::CallToolRawText(*this, TEXT("retarget_animations"),
		FString::Printf(TEXT("{\"source_mesh\":\"/Game/NoSuchMesh\",\"target_mesh\":\"%s\",\"output_path\":\"%s\",\"animations\":[\"%s\"]}"),
			KTgtMesh, KOutDir, KIdleAnim),
		bIsError);
	TestTrue(TEXT("nonexistent source mesh is error"), bIsError);

	return true;
}

// ---------------------------------------------------------------------------
// Happy path: full pipeline Quinn_Simple -> Manny_Simple on MM_Idle with chain
// overrides, T-pose retarget pose from MM_T_Pose, and finger-track stripping.
// ---------------------------------------------------------------------------
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FRetargetAnimationsHappyPathTest,
	"UnrealAgentMCP.Retarget.FullPipelineWithStrip",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FRetargetAnimationsHappyPathTest::RunTest(const FString& Parameters)
{
	// Cleanup all generated assets regardless of outcome.
	ON_SCOPE_EXIT
	{
		CleanupAssetRT(*this, FString(KOutDir) + TEXT("/MM_Idle_McpRT"));
		CleanupAssetRT(*this, FString(KOutDir) + TEXT("/RTG_SKM_Quinn_Simple_to_SKM_Manny_Simple"));
		CleanupAssetRT(*this, FString(KOutDir) + TEXT("/IK_SKM_Quinn_Simple_AutoSrc"));
		CleanupAssetRT(*this, FString(KOutDir) + TEXT("/IK_SKM_Manny_Simple_AutoTgt"));
	};

	bool bIsError = false;
	const FString Args = FString::Printf(
		TEXT("{\"source_mesh\":\"%s\",\"target_mesh\":\"%s\",\"output_path\":\"%s\",\"suffix\":\"_McpRT\",")
		TEXT("\"animations\":[\"%s\"],")
		TEXT("\"target_tpose_animation\":\"/Game/Characters/Mannequins/Animations/Manny/MM_T_Pose\",")
		TEXT("\"tpose_bones\":[\"clavicle_l\",\"upperarm_l\",\"lowerarm_l\",\"hand_l\",\"clavicle_r\",\"upperarm_r\",\"lowerarm_r\",\"hand_r\"],")
		TEXT("\"strip_track_prefixes\":[\"thumb_\",\"index_\",\"middle_\",\"ring_\",\"pinky_\"],")
		TEXT("\"save\":false}"),
		KSrcMesh, KTgtMesh, KOutDir, KIdleAnim);

	const TSharedPtr<FJsonObject> Result = AgentMcpTestUtils::CallTool(*this,
		TEXT("retarget_animations"), Args, bIsError);
	TestFalse(TEXT("retarget_animations happy path is not an error"), bIsError);
	if (!TestNotNull(TEXT("result parses"), Result.Get()))
	{
		return true;
	}

	TestTrue(TEXT("retargeted:true"), Result->GetBoolField(TEXT("retargeted")));

	const TArray<TSharedPtr<FJsonValue>>* Outputs = nullptr;
	if (!TestTrue(TEXT("outputs array has 1 entry"),
		Result->TryGetArrayField(TEXT("outputs"), Outputs) && Outputs->Num() == 1))
	{
		return true;
	}
	const FString OutPath = (*Outputs)[0]->AsObject()->GetStringField(TEXT("output"));
	TestTrue(TEXT("output lives under output_path"), OutPath.StartsWith(KOutDir));

	// The produced sequence exists, targets the right skeleton, and finger tracks are gone.
	UAnimSequence* OutSeq = LoadObject<UAnimSequence>(nullptr, *OutPath);
	if (TestNotNull(TEXT("output AnimSequence loads"), OutSeq))
	{
		USkeletalMesh* TgtMesh = LoadObject<USkeletalMesh>(nullptr, KTgtMesh);
		if (TgtMesh)
		{
			TestEqual(TEXT("output skeleton matches target mesh skeleton"),
				OutSeq->GetSkeleton(), TgtMesh->GetSkeleton());
		}
		TArray<FName> TrackNames;
		OutSeq->GetDataModel()->GetBoneTrackNames(TrackNames);
		bool bHasFinger = false;
		for (const FName& Track : TrackNames)
		{
			const FString TrackStr = Track.ToString();
			if (TrackStr.StartsWith(TEXT("thumb_")) || TrackStr.StartsWith(TEXT("index_")) ||
				TrackStr.StartsWith(TEXT("middle_")) || TrackStr.StartsWith(TEXT("ring_")) ||
				TrackStr.StartsWith(TEXT("pinky_")))
			{
				bHasFinger = true;
				break;
			}
		}
		TestFalse(TEXT("finger tracks stripped from output"), bHasFinger);
		TestTrue(TEXT("non-finger tracks remain"), TrackNames.Num() > 0);
	}

	const double Stripped = Result->GetNumberField(TEXT("stripped_tracks"));
	TestTrue(TEXT("stripped_tracks > 0"), Stripped > 0.0);

	return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS
