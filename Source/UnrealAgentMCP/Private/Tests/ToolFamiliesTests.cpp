#include "Misc/AutomationTest.h"

#if WITH_DEV_AUTOMATION_TESTS

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

#endif // WITH_DEV_AUTOMATION_TESTS
