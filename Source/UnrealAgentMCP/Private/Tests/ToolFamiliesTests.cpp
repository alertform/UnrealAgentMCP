#include "Misc/AutomationTest.h"

#if WITH_DEV_AUTOMATION_TESTS

#include "HAL/FileManager.h"
#include "Misc/Paths.h"
#include "Misc/ScopeExit.h"
#include "Tests/AgentMcpTestHelpers.h"

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FToolFamiliesCdoPropertyTest,
	"UnrealAgentMCP.ToolFamilies.CdoGetSetRoundTrip",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FToolFamiliesCdoPropertyTest::RunTest(const FString& Parameters)
{
	UBlueprint* Blueprint = AgentMcpTestUtils::MakeTransientBlueprint(TEXT("BP_McpCdoTest"));
	if (!TestNotNull(TEXT("transient blueprint created"), Blueprint))
	{
		return true;
	}
	const FString Path = Blueprint->GetPathName();
	bool bIsError = false;

	// AActor::bCanBeDamaged is an EditAnywhere bool ("bCanBeDamaged") - present on every Actor CDO.
	const TSharedPtr<FJsonObject> Got = AgentMcpTestUtils::CallTool(*this, TEXT("get_cdo_property"),
		FString::Printf(TEXT("{\"blueprint_path\":\"%s\",\"property\":\"bCanBeDamaged\"}"), *Path), bIsError);
	TestFalse(TEXT("get_cdo_property ok"), bIsError);
	FString Original;
	if (TestNotNull(TEXT("get payload parses"), Got.Get()))
	{
		Original = Got->GetStringField(TEXT("value"));
		TestTrue(TEXT("bool value shape"), Original == TEXT("True") || Original == TEXT("False") || Original == TEXT("true") || Original == TEXT("false"));
	}

	// Flip it via set, read back via get.
	const FString Flipped = (Original.Compare(TEXT("True"), ESearchCase::IgnoreCase) == 0) ? TEXT("False") : TEXT("True");
	AgentMcpTestUtils::CallTool(*this, TEXT("set_cdo_property"),
		FString::Printf(TEXT("{\"blueprint_path\":\"%s\",\"property\":\"bCanBeDamaged\",\"value\":\"%s\"}"), *Path, *Flipped), bIsError);
	TestFalse(TEXT("set_cdo_property ok"), bIsError);
	const TSharedPtr<FJsonObject> Got2 = AgentMcpTestUtils::CallTool(*this, TEXT("get_cdo_property"),
		FString::Printf(TEXT("{\"blueprint_path\":\"%s\",\"property\":\"bCanBeDamaged\"}"), *Path), bIsError);
	if (Got2.IsValid())
	{
		TestEqual(TEXT("value round-trips"), Got2->GetStringField(TEXT("value")).ToLower(), Flipped.ToLower());
	}

	// Unknown property -> tool error listing the property name.
	AgentMcpTestUtils::CallTool(*this, TEXT("get_cdo_property"),
		FString::Printf(TEXT("{\"blueprint_path\":\"%s\",\"property\":\"NoSuchPropXyz\"}"), *Path), bIsError);
	TestTrue(TEXT("unknown property is a tool error"), bIsError);

	// Invalid value for typed property -> tool error (ImportText failure surfaced).
	AgentMcpTestUtils::CallTool(*this, TEXT("set_cdo_property"),
		FString::Printf(TEXT("{\"blueprint_path\":\"%s\",\"property\":\"bCanBeDamaged\",\"value\":\"not-a-bool-xyz\"}"), *Path), bIsError);
	TestTrue(TEXT("invalid value is a tool error"), bIsError);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FToolFamiliesAssetTest,
	"UnrealAgentMCP.ToolFamilies.AssetSearchInfoRefsSave",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FToolFamiliesAssetTest::RunTest(const FString& Parameters)
{
	bool bIsError = false;

	// search_assets: by class, project-wide. Project has Blueprints (BP_*).
	const TSharedPtr<FJsonObject> Search = AgentMcpTestUtils::CallTool(*this, TEXT("search_assets"),
		TEXT("{\"class_name\":\"Blueprint\",\"name_contains\":\"BP_\",\"limit\":10}"), bIsError);
	TestFalse(TEXT("search_assets ok"), bIsError);
	FString AnyAssetPath;
	if (TestNotNull(TEXT("search payload parses"), Search.Get()))
	{
		const TArray<TSharedPtr<FJsonValue>>& Assets = Search->GetArrayField(TEXT("assets"));
		TestTrue(TEXT("found at least one BP"), Assets.Num() >= 1);
		if (Assets.Num() > 0)
		{
			AnyAssetPath = Assets[0]->AsObject()->GetStringField(TEXT("package_path"));
		}
	}

	// get_asset_info on a found asset.
	if (!AnyAssetPath.IsEmpty())
	{
		const TSharedPtr<FJsonObject> Info = AgentMcpTestUtils::CallTool(*this, TEXT("get_asset_info"),
			FString::Printf(TEXT("{\"asset_path\":\"%s\"}"), *AnyAssetPath), bIsError);
		TestFalse(TEXT("get_asset_info ok"), bIsError);
		if (Info.IsValid())
		{
			TestTrue(TEXT("info has class"), Info->HasField(TEXT("class")));
		}

		// get_references both directions return arrays (counts may be 0 - shape only).
		const TSharedPtr<FJsonObject> Refs = AgentMcpTestUtils::CallTool(*this, TEXT("get_references"),
			FString::Printf(TEXT("{\"asset_path\":\"%s\",\"direction\":\"dependencies\"}"), *AnyAssetPath), bIsError);
		TestFalse(TEXT("get_references ok"), bIsError);
		if (Refs.IsValid())
		{
			TestTrue(TEXT("refs has packages array"), Refs->HasField(TEXT("packages")));
		}
	}

	// Unknown asset -> tool errors.
	AgentMcpTestUtils::CallTool(*this, TEXT("get_asset_info"), TEXT("{\"asset_path\":\"/Game/Nope/NoAsset\"}"), bIsError);
	TestTrue(TEXT("unknown asset info is a tool error"), bIsError);

	// save_asset writes a real file: create -> save -> file exists -> cleanup.
	AgentMcpTestUtils::CallTool(*this, TEXT("create_blueprint"),
		TEXT("{\"asset_path\":\"/Game/Dev/AgentMcpTests/BP_McpSaveTest\",\"parent_class\":\"Actor\"}"), bIsError);
	TestFalse(TEXT("create for save ok"), bIsError);
	const FString DiskPath = FPaths::ProjectContentDir() / TEXT("Dev/AgentMcpTests/BP_McpSaveTest.uasset");
	ON_SCOPE_EXIT
	{
		// Keep the user's working tree clean: remove the test artifact from disk.
		IFileManager::Get().Delete(*DiskPath, /*RequireExists*/ false, /*EvenReadOnly*/ true);
	};
	const TSharedPtr<FJsonObject> Saved = AgentMcpTestUtils::CallTool(*this, TEXT("save_asset"),
		TEXT("{\"asset_path\":\"/Game/Dev/AgentMcpTests/BP_McpSaveTest\"}"), bIsError);
	TestFalse(TEXT("save_asset ok"), bIsError);
	TestTrue(TEXT("uasset file exists on disk"), IFileManager::Get().FileExists(*DiskPath));
	return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS
