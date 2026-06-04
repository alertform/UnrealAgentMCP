#include "Misc/AutomationTest.h"

#if WITH_DEV_AUTOMATION_TESTS

#include "Tests/AgentMcpTestHelpers.h"

// ---------------------------------------------------------------------------
// FClassPinDefaultTest
// Verifies that set_pin_default handles PC_Class pins via TrySetDefaultObject.
// Fixture: GameplayStatics.GetActorOfClass  — its ActorClass pin is PC_Class
//          with meta-class AActor, so:
//   * PointLight  (IS an AActor subclass)  -> success
//   * Texture2D   (NOT an AActor subclass) -> "not a subclass" error
//   * NoSuch.NothingXyz (doesn't exist)    -> "Could not load" error
// ---------------------------------------------------------------------------
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FClassPinDefaultTest,
	"UnrealAgentMCP.P5.ClassPinDefault",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FClassPinDefaultTest::RunTest(const FString& Parameters)
{
	UBlueprint* Blueprint = AgentMcpTestUtils::MakeTransientBlueprint(TEXT("BP_McpClassPinTest"));
	if (!TestNotNull(TEXT("transient blueprint created"), Blueprint))
	{
		return true;
	}
	const FString Path = Blueprint->GetPathName();
	bool bIsError = false;

	// Step 1 — add_node: GameplayStatics.GetActorOfClass; capture node_id.
	const TSharedPtr<FJsonObject> NodePayload = AgentMcpTestUtils::CallTool(*this, TEXT("add_node"),
		FString::Printf(
			TEXT("{\"blueprint_path\":\"%s\",\"node_type\":\"call_function\",\"class_name\":\"GameplayStatics\",\"function_name\":\"GetActorOfClass\"}"),
			*Path),
		bIsError);
	if (!TestFalse(TEXT("add_node GetActorOfClass succeeds"), bIsError) ||
		!TestNotNull(TEXT("add_node payload parses"), NodePayload.Get()))
	{
		return true;
	}
	const FString NodeId = NodePayload->GetStringField(TEXT("node_id"));

	// Step 2 — valid subclass: PointLight IS an AActor subclass -> expect {set:true}.
	const TSharedPtr<FJsonObject> GoodResult = AgentMcpTestUtils::CallTool(*this, TEXT("set_pin_default"),
		FString::Printf(
			TEXT("{\"blueprint_path\":\"%s\",\"node_id\":\"%s\",\"pin_name\":\"ActorClass\",\"value\":\"/Script/Engine.PointLight\"}"),
			*Path, *NodeId),
		bIsError);
	TestFalse(TEXT("set PointLight class succeeds (no error)"), bIsError);
	if (TestNotNull(TEXT("good-class result parses"), GoodResult.Get()))
	{
		TestTrue(TEXT("result reports set:true"), GoodResult->GetBoolField(TEXT("set")));
	}

	// Step 3 — wrong subclass: Texture2D is NOT an AActor subclass -> expect error containing "not a subclass".
	const FString BadSubclassText = AgentMcpTestUtils::CallToolRawText(*this, TEXT("set_pin_default"),
		FString::Printf(
			TEXT("{\"blueprint_path\":\"%s\",\"node_id\":\"%s\",\"pin_name\":\"ActorClass\",\"value\":\"/Script/Engine.Texture2D\"}"),
			*Path, *NodeId),
		bIsError);
	TestTrue(TEXT("Texture2D set is a tool error"), bIsError);
	TestTrue(TEXT("error mentions 'not a subclass'"),
		BadSubclassText.Contains(TEXT("not a subclass"), ESearchCase::IgnoreCase));

	// Step 4 — non-existent class -> expect error containing "Could not load".
	const FString MissingText = AgentMcpTestUtils::CallToolRawText(*this, TEXT("set_pin_default"),
		FString::Printf(
			TEXT("{\"blueprint_path\":\"%s\",\"node_id\":\"%s\",\"pin_name\":\"ActorClass\",\"value\":\"/Script/NoSuch.NothingXyz\"}"),
			*Path, *NodeId),
		bIsError);
	TestTrue(TEXT("missing class set is a tool error"), bIsError);
	TestTrue(TEXT("error mentions 'Could not load'"),
		MissingText.Contains(TEXT("Could not load"), ESearchCase::IgnoreCase));

	return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS
