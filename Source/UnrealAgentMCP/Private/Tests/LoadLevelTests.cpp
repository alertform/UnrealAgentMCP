#include "Misc/AutomationTest.h"

#if WITH_DEV_AUTOMATION_TESTS

#include "Tests/AgentMcpTestHelpers.h"
#include "AssetRegistry/AssetRegistryModule.h"

// ---------------------------------------------------------------------------
// Error-path: non-existent map path -> isError + "not found" in message
// ---------------------------------------------------------------------------
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FLoadLevelErrorPathTest,
	"UnrealAgentMCP.LoadLevel.MissingMapReturnsNotFound",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FLoadLevelErrorPathTest::RunTest(const FString& Parameters)
{
	bool bIsError = false;
	const FString RawText = AgentMcpTestUtils::CallToolRawText(*this,
		TEXT("load_level"),
		TEXT("{\"map_path\":\"/Game/Does/Not/Exist\"}"),
		bIsError);

	TestTrue(TEXT("load_level with missing map is a tool error"), bIsError);
	TestTrue(TEXT("error message contains 'not found'"),
		RawText.Contains(TEXT("not found"), ESearchCase::IgnoreCase));

	return true;
}

// ---------------------------------------------------------------------------
// Happy-path: load a real map -> loaded=true, world name matches, engine_info
// reflects the new level.
//
// Force a synchronous asset-registry scan before calling load_level so the
// headless -NullRHI environment has the map registered before the tool's
// asset-existence check runs.  If LoadMap still fails after the scan the
// test fails honestly — no soft-skip.
// ---------------------------------------------------------------------------
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FLoadLevelHappyPathTest,
	"UnrealAgentMCP.LoadLevel.LoadRealMapAndVerifyCurrentLevel",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FLoadLevelHappyPathTest::RunTest(const FString& Parameters)
{
	// Force the asset registry to finish scanning before the tool's existence check.
	FAssetRegistryModule& AssetRegistry =
		FModuleManager::LoadModuleChecked<FAssetRegistryModule>("AssetRegistry");
	AssetRegistry.Get().ScanPathsSynchronous({ TEXT("/Game/Maps") }, /*bForceRescan*/ true);

	bool bIsError = false;

	// Use the project's main map (lives under /Game/Maps/).
	const TArray<FString> CandidatePaths = {
		TEXT("/Game/Maps/ThirdPersonMap"),
	};

	FString UsedPath;
	TSharedPtr<FJsonObject> LoadResult;

	for (const FString& Candidate : CandidatePaths)
	{
		bIsError = false;
		LoadResult = AgentMcpTestUtils::CallTool(*this, TEXT("load_level"),
			FString::Printf(TEXT("{\"map_path\":\"%s\"}"), *Candidate),
			bIsError);
		if (!bIsError && LoadResult.IsValid())
		{
			UsedPath = Candidate;
			break;
		}
	}

	// No soft-skip: if all candidates failed, fail the test with the error information.
	TestFalse(TEXT("load_level succeeded for at least one candidate path"), UsedPath.IsEmpty());
	if (UsedPath.IsEmpty())
	{
		AddError(TEXT("load_level returned an error for all candidate paths. "
			"Check that the map asset exists and that LoadMap works in this configuration."));
		return false;
	}

	// loaded == true
	bool bLoaded = false;
	TestTrue(TEXT("loaded field is true"), LoadResult->TryGetBoolField(TEXT("loaded"), bLoaded) && bLoaded);

	// world name contains the map name
	FString WorldName;
	LoadResult->TryGetStringField(TEXT("world"), WorldName);
	TestTrue(TEXT("world name contains 'ThirdPersonMap'"),
		WorldName.Contains(TEXT("ThirdPersonMap"), ESearchCase::IgnoreCase));

	// engine_info.current_level reflects the loaded map
	bIsError = false;
	const TSharedPtr<FJsonObject> InfoResult = AgentMcpTestUtils::CallTool(*this, TEXT("engine_info"),
		TEXT("{}"), bIsError);
	TestFalse(TEXT("engine_info ok after load_level"), bIsError);
	if (InfoResult.IsValid())
	{
		FString CurrentLevel;
		InfoResult->TryGetStringField(TEXT("current_level"), CurrentLevel);
		TestTrue(TEXT("engine_info.current_level contains 'ThirdPersonMap'"),
			CurrentLevel.Contains(TEXT("ThirdPersonMap"), ESearchCase::IgnoreCase));
	}

	return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS
