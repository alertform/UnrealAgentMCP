#include "Misc/AutomationTest.h"

#if WITH_DEV_AUTOMATION_TESTS

#include "InputAction.h"
#include "InputMappingContext.h"
#include "Tests/AgentMcpTestHelpers.h"

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FInputAssetCreationTest,
	"UnrealAgentMCP.Input.CreateActionAndContext",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FInputAssetCreationTest::RunTest(const FString& Parameters)
{
	bool bIsError = false;

	// create_input_action with axis2d value type (in-memory asset, not saved).
	const TSharedPtr<FJsonObject> Action = AgentMcpTestUtils::CallTool(*this, TEXT("create_input_action"),
		TEXT("{\"asset_path\":\"/Game/Dev/AgentMcpTests/IA_McpMove\",\"value_type\":\"axis2d\"}"), bIsError);
	TestFalse(TEXT("create_input_action ok"), bIsError);
	if (TestNotNull(TEXT("action payload parses"), Action.Get()))
	{
		TestEqual(TEXT("value_type echoed"), Action->GetStringField(TEXT("value_type")), FString(TEXT("axis2d")));
		// Verify the live object really carries the value type.
		UInputAction* Live = FindObject<UInputAction>(nullptr, TEXT("/Game/Dev/AgentMcpTests/IA_McpMove.IA_McpMove"));
		if (TestNotNull(TEXT("live UInputAction exists"), Live))
		{
			TestTrue(TEXT("ValueType is Axis2D"), Live->ValueType == EInputActionValueType::Axis2D);
		}
	}

	// duplicate -> tool error.
	AgentMcpTestUtils::CallTool(*this, TEXT("create_input_action"),
		TEXT("{\"asset_path\":\"/Game/Dev/AgentMcpTests/IA_McpMove\"}"), bIsError);
	TestTrue(TEXT("duplicate action is a tool error"), bIsError);

	// bad value_type -> tool error listing supported tokens.
	AgentMcpTestUtils::CallTool(*this, TEXT("create_input_action"),
		TEXT("{\"asset_path\":\"/Game/Dev/AgentMcpTests/IA_McpBad\",\"value_type\":\"vector7d\"}"), bIsError);
	TestTrue(TEXT("bad value_type is a tool error"), bIsError);

	// create_mapping_context.
	const TSharedPtr<FJsonObject> Context = AgentMcpTestUtils::CallTool(*this, TEXT("create_mapping_context"),
		TEXT("{\"asset_path\":\"/Game/Dev/AgentMcpTests/IMC_McpDefault\"}"), bIsError);
	TestFalse(TEXT("create_mapping_context ok"), bIsError);
	if (Context.IsValid())
	{
		TestNotNull(TEXT("live UInputMappingContext exists"),
			FindObject<UInputMappingContext>(nullptr, TEXT("/Game/Dev/AgentMcpTests/IMC_McpDefault.IMC_McpDefault")));
	}
	return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS
