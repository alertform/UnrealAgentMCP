#include "Misc/AutomationTest.h"

#if WITH_DEV_AUTOMATION_TESTS

#include "AgentMcpSettings.h"
#include "Animation/AnimMontage.h"
#include "Animation/AnimSequence.h"
#include "Core/AgentMcpTier.h"
#include "Core/AgentMcpToolRegistry.h"
#include "Core/McpTypes.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "Editor.h"
#include "Engine/Blueprint.h"
#include "GameplayEffect.h"
#include "GameplayEffectComponents/TargetTagsGameplayEffectComponent.h"
#include "GameplayTagContainer.h"
#include "Kismet2/KismetEditorUtilities.h"
#include "Misc/ScopeExit.h"
#include "Tests/AgentMcpTestHelpers.h"
#include "UObject/Package.h"

// ---------------------------------------------------------------------------
// Constants
// ---------------------------------------------------------------------------

namespace
{
	// A guaranteed-present AnimSequence from the project's Mannequin content.
	constexpr const TCHAR* KIdleAnimPath = TEXT("/Game/Characters/Mannequins/Animations/Manny/MM_Idle");

	// Tag registered natively by the host project — always present in the editor process.
	constexpr const TCHAR* KMeleeAttackTag = TEXT("Ability.Melee.Attack");

	// Temporary asset paths created during tests (all under /Game/Dev_Test/).
	constexpr const TCHAR* KMontageTestPath  = TEXT("/Game/Dev_Test/AM_McpCreateMontageTest");
	constexpr const TCHAR* KGeTestPath       = TEXT("/Game/Dev_Test/BP_McpGeTargetTagsTest");
}

// ---------------------------------------------------------------------------
// Helper: delete an asset via the tool (ignores errors so cleanup is best-effort).
// ---------------------------------------------------------------------------
namespace
{
	void CleanupAsset(FAutomationTestBase& Test, const FString& Path)
	{
		// Raise ceiling to Destructive for cleanup then restore.
		UAgentMcpSettings* Settings = GetMutableDefault<UAgentMcpSettings>();
		const EAgentMcpTier Saved = Settings->PermissionTier;
		Settings->PermissionTier = EAgentMcpTier::Destructive;
		bool bErr = false;
		AgentMcpTestUtils::CallTool(Test, TEXT("delete_asset"),
			FString::Printf(TEXT("{\"asset_path\":\"%s\"}"), *Path), bErr);
		Settings->PermissionTier = Saved;
	}
}

// ---------------------------------------------------------------------------
// FCreateAnimMontageTest
// Creates a montage from MM_Idle, asserts key fields, then cleans up.
// ---------------------------------------------------------------------------
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCreateAnimMontageTest,
	"UnrealAgentMCP.P7.CreateAnimMontage",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FCreateAnimMontageTest::RunTest(const FString& Parameters)
{
	// Unconditional cleanup so a failed run doesn't leave /Game/Dev_Test debris.
	ON_SCOPE_EXIT { CleanupAsset(*this, KMontageTestPath); };

	bool bIsError = false;

	// Step 1 — create_anim_montage: happy path.
	const TSharedPtr<FJsonObject> CreateResult = AgentMcpTestUtils::CallTool(*this, TEXT("create_anim_montage"),
		FString::Printf(
			TEXT("{\"source_animation\":\"%s\",\"asset_path\":\"%s\",\"slot_name\":\"DefaultSlot\"}"),
			KIdleAnimPath, KMontageTestPath),
		bIsError);
	TestFalse(TEXT("create_anim_montage succeeds"), bIsError);
	if (!TestNotNull(TEXT("create result parses"), CreateResult.Get()))
	{
		return true;
	}
	TestTrue(TEXT("created:true"), CreateResult->GetBoolField(TEXT("created")));

	const double Length = CreateResult->GetNumberField(TEXT("length"));
	TestTrue(TEXT("length > 0"), Length > 0.0);

	TestEqual(TEXT("slot is DefaultSlot"), CreateResult->GetStringField(TEXT("slot")), FString(TEXT("DefaultSlot")));

	// Verify the asset is loadable.
	UAnimMontage* Montage = LoadObject<UAnimMontage>(nullptr,
		*(CreateResult->GetStringField(TEXT("asset_path"))));
	if (TestNotNull(TEXT("montage loads from registry path"), Montage))
	{
		TestTrue(TEXT("montage has at least one slot track"), Montage->SlotAnimTracks.Num() > 0);
		TestEqual(TEXT("slot name matches"), Montage->SlotAnimTracks[0].SlotName, FName(TEXT("DefaultSlot")));
		TestTrue(TEXT("segment count >= 1"), Montage->SlotAnimTracks[0].AnimTrack.AnimSegments.Num() >= 1);
	}

	// Step 2 — duplicate path -> error.
	const FString DupErr = AgentMcpTestUtils::CallToolRawText(*this, TEXT("create_anim_montage"),
		FString::Printf(
			TEXT("{\"source_animation\":\"%s\",\"asset_path\":\"%s\"}"),
			KIdleAnimPath, KMontageTestPath),
		bIsError);
	TestTrue(TEXT("duplicate asset_path is error"), bIsError);
	TestTrue(TEXT("dup error mentions already-exists"),
		DupErr.Contains(TEXT("already"), ESearchCase::IgnoreCase) ||
		DupErr.Contains(TEXT("exists"), ESearchCase::IgnoreCase));

	// Step 3 — non-existent source -> error.
	const FString SrcErr = AgentMcpTestUtils::CallToolRawText(*this, TEXT("create_anim_montage"),
		TEXT("{\"source_animation\":\"/Game/NoSuchAnim\",\"asset_path\":\"/Game/Dev_Test/AM_ShouldNotExist\"}"),
		bIsError);
	TestTrue(TEXT("missing source animation is error"), bIsError);

	return true;
}

// ---------------------------------------------------------------------------
// FAddAnimNotifyTest
// Creates a montage (reuses the one from CreateAnimMontage if already present,
// or creates fresh), adds AnimNotify_PlaySound at fraction 0.5, then cleans up.
// ---------------------------------------------------------------------------
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FAddAnimNotifyTest,
	"UnrealAgentMCP.P7.AddAnimNotify",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FAddAnimNotifyTest::RunTest(const FString& Parameters)
{
	// Always create a fresh temporary montage so this test is independent.
	constexpr const TCHAR* LocalMontagePath = TEXT("/Game/Dev_Test/AM_McpAddNotifyTest");
	ON_SCOPE_EXIT { CleanupAsset(*this, LocalMontagePath); };

	bool bIsError = false;

	// Create temp montage.
	const TSharedPtr<FJsonObject> CreateResult = AgentMcpTestUtils::CallTool(*this, TEXT("create_anim_montage"),
		FString::Printf(
			TEXT("{\"source_animation\":\"%s\",\"asset_path\":\"%s\"}"),
			KIdleAnimPath, LocalMontagePath),
		bIsError);
	if (!TestFalse(TEXT("create temp montage succeeds"), bIsError) ||
		!TestNotNull(TEXT("create result parses"), CreateResult.Get()))
	{
		return true;
	}
	const FString MontageObjPath = CreateResult->GetStringField(TEXT("asset_path"));
	const double TotalLength = CreateResult->GetNumberField(TEXT("length"));

	// Step 1 — add AnimNotify_PlaySound at fraction 0.5.
	const TSharedPtr<FJsonObject> AddResult = AgentMcpTestUtils::CallTool(*this, TEXT("add_anim_notify"),
		FString::Printf(
			TEXT("{\"asset_path\":\"%s\",\"notify_class\":\"/Script/Engine.AnimNotify_PlaySound\",\"fraction\":0.5}"),
			*MontageObjPath),
		bIsError);
	TestFalse(TEXT("add_anim_notify at fraction 0.5 succeeds"), bIsError);
	if (TestNotNull(TEXT("add result parses"), AddResult.Get()))
	{
		TestTrue(TEXT("added:true"), AddResult->GetBoolField(TEXT("added")));

		const double NotifyTime = AddResult->GetNumberField(TEXT("time"));
		const double Expected = TotalLength * 0.5;
		// Allow 1-frame tolerance.
		TestTrue(TEXT("notify time is near 50% of length"),
			FMath::Abs(NotifyTime - Expected) < 0.1);

		TestTrue(TEXT("notify_class contains AnimNotify_PlaySound"),
			AddResult->GetStringField(TEXT("notify_class")).Contains(TEXT("AnimNotify_PlaySound"), ESearchCase::IgnoreCase));
	}

	// Verify in-memory: Notifies.Num() == 1.
	UAnimMontage* Montage = LoadObject<UAnimMontage>(nullptr, *MontageObjPath);
	if (TestNotNull(TEXT("montage loads after notify add"), Montage))
	{
		TestEqual(TEXT("Notifies.Num() == 1"), Montage->Notifies.Num(), 1);
		if (Montage->Notifies.Num() > 0)
		{
			const float ApproxHalf = static_cast<float>(TotalLength * 0.5);
			TestTrue(TEXT("notify time approx half"),
				FMath::Abs(Montage->Notifies[0].GetTime() - ApproxHalf) < 0.2f);
		}
	}

	// Step 2 — error: both time AND fraction provided.
	const FString BothErr = AgentMcpTestUtils::CallToolRawText(*this, TEXT("add_anim_notify"),
		FString::Printf(
			TEXT("{\"asset_path\":\"%s\",\"notify_class\":\"/Script/Engine.AnimNotify_PlaySound\",\"time\":0.1,\"fraction\":0.5}"),
			*MontageObjPath),
		bIsError);
	TestTrue(TEXT("time+fraction both is error"), bIsError);

	// Step 3 — error: neither time nor fraction.
	const FString NoneErr = AgentMcpTestUtils::CallToolRawText(*this, TEXT("add_anim_notify"),
		FString::Printf(
			TEXT("{\"asset_path\":\"%s\",\"notify_class\":\"/Script/Engine.AnimNotify_PlaySound\"}"),
			*MontageObjPath),
		bIsError);
	TestTrue(TEXT("neither time nor fraction is error"), bIsError);

	// Step 4 — error: non-existent asset.
	const FString NoAssetErr = AgentMcpTestUtils::CallToolRawText(*this, TEXT("add_anim_notify"),
		TEXT("{\"asset_path\":\"/Game/NoSuchMontage\",\"notify_class\":\"/Script/Engine.AnimNotify_PlaySound\",\"fraction\":0.5}"),
		bIsError);
	TestTrue(TEXT("missing asset is error"), bIsError);

	return true;
}

// ---------------------------------------------------------------------------
// FSetGeTargetTagsTest
// Creates a temporary GameplayEffect BP, calls set_ge_target_tags, verifies
// the component and tag are present, then cleans up.
// ---------------------------------------------------------------------------
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FSetGeTargetTagsTest,
	"UnrealAgentMCP.P7.SetGeTargetTags",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FSetGeTargetTagsTest::RunTest(const FString& Parameters)
{
	ON_SCOPE_EXIT { CleanupAsset(*this, KGeTestPath); };

	bool bIsError = false;

	// Step 1 — create a GameplayEffect blueprint at the test path.
	const TSharedPtr<FJsonObject> CreateResult = AgentMcpTestUtils::CallTool(*this, TEXT("create_blueprint"),
		FString::Printf(
			TEXT("{\"asset_path\":\"%s\",\"parent_class\":\"/Script/GameplayAbilities.GameplayEffect\"}"),
			KGeTestPath),
		bIsError);
	if (!TestFalse(TEXT("create GE blueprint succeeds"), bIsError) ||
		!TestNotNull(TEXT("create GE result parses"), CreateResult.Get()))
	{
		return true;
	}
	const FString BpPath = CreateResult->GetStringField(TEXT("blueprint_path"));

	// Step 2 — set_ge_target_tags with the native Ability.Melee.Attack tag.
	const TSharedPtr<FJsonObject> SetResult = AgentMcpTestUtils::CallTool(*this, TEXT("set_ge_target_tags"),
		FString::Printf(
			TEXT("{\"blueprint_path\":\"%s\",\"granted_tags\":[\"%s\"]}"),
			*BpPath, KMeleeAttackTag),
		bIsError);
	TestFalse(TEXT("set_ge_target_tags succeeds"), bIsError);
	if (TestNotNull(TEXT("set result parses"), SetResult.Get()))
	{
		TestTrue(TEXT("set:true"), SetResult->GetBoolField(TEXT("set")));

		// The response must list the tag we just set.
		bool bTagFound = false;
		for (const TSharedPtr<FJsonValue>& TagVal : SetResult->GetArrayField(TEXT("granted_tags")))
		{
			if (TagVal->AsString().Contains(TEXT("Melee.Attack"), ESearchCase::IgnoreCase))
			{
				bTagFound = true;
			}
		}
		TestTrue(TEXT("response granted_tags contains Melee.Attack"), bTagFound);

		TestEqual(TEXT("component is UTargetTagsGameplayEffectComponent"),
			SetResult->GetStringField(TEXT("component")),
			FString(UTargetTagsGameplayEffectComponent::StaticClass()->GetName()));
	}

	// Step 3 — verify in-memory via CDO.
	UBlueprint* BP = LoadObject<UBlueprint>(nullptr, *BpPath);
	if (TestNotNull(TEXT("GE BP loads"), BP) && BP->GeneratedClass)
	{
		UGameplayEffect* GE = Cast<UGameplayEffect>(BP->GeneratedClass->GetDefaultObject());
		if (TestNotNull(TEXT("GE CDO valid"), GE))
		{
			const UTargetTagsGameplayEffectComponent* Comp = GE->FindComponent<UTargetTagsGameplayEffectComponent>();
			TestNotNull(TEXT("UTargetTagsGameplayEffectComponent present on CDO"), Comp);
			if (Comp)
			{
				const FInheritedTagContainer& Tags = Comp->GetConfiguredTargetTagChanges();
				const bool bInAdded = Tags.Added.HasTagExact(
					FGameplayTag::RequestGameplayTag(FName(KMeleeAttackTag)));
				const bool bInCombined = Tags.CombinedTags.HasTagExact(
					FGameplayTag::RequestGameplayTag(FName(KMeleeAttackTag)));
				TestTrue(TEXT("Melee.Attack tag present in Added or CombinedTags"),
					bInAdded || bInCombined);
			}
		}
	}

	// Step 4 — error: unregistered tag.
	const FString UnregErr = AgentMcpTestUtils::CallToolRawText(*this, TEXT("set_ge_target_tags"),
		FString::Printf(
			TEXT("{\"blueprint_path\":\"%s\",\"granted_tags\":[\"No.Such.Tag.XyzAbc\"]}"),
			*BpPath),
		bIsError);
	TestTrue(TEXT("unregistered tag is error"), bIsError);

	// Step 5 — error: non-GE blueprint.
	{
		// create a plain Actor BP.
		constexpr const TCHAR* TmpActorPath = TEXT("/Game/Dev_Test/BP_McpGeNonGeTest");
		ON_SCOPE_EXIT { CleanupAsset(*this, TmpActorPath); };

		const TSharedPtr<FJsonObject> ActorBP = AgentMcpTestUtils::CallTool(*this, TEXT("create_blueprint"),
			FString::Printf(TEXT("{\"asset_path\":\"%s\",\"parent_class\":\"Actor\"}"), TmpActorPath),
			bIsError);
		TestFalse(TEXT("create Actor BP for non-GE test ok"), bIsError);
		if (ActorBP.IsValid())
		{
			const FString ActorBpPath = ActorBP->GetStringField(TEXT("blueprint_path"));
			const FString NonGeErr = AgentMcpTestUtils::CallToolRawText(*this, TEXT("set_ge_target_tags"),
				FString::Printf(
					TEXT("{\"blueprint_path\":\"%s\",\"granted_tags\":[\"%s\"]}"),
					*ActorBpPath, KMeleeAttackTag),
				bIsError);
			TestTrue(TEXT("non-GE blueprint is error"), bIsError);
			TestTrue(TEXT("non-GE error mentions GameplayEffect"),
				NonGeErr.Contains(TEXT("GameplayEffect"), ESearchCase::IgnoreCase));
		}
	}

	return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS
