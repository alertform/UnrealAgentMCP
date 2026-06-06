#include "Misc/AutomationTest.h"

#if WITH_DEV_AUTOMATION_TESTS

#include "AgentMcpSettings.h"
#include "Core/AgentMcpTier.h"
#include "InputAction.h"
#include "InputMappingContext.h"
#include "Misc/ScopeExit.h"
#include "Tests/AgentMcpTestHelpers.h"

// ---------------------------------------------------------------------------
// Helper: delete an asset via the tool (mirrors AnimAndGeToolsTests pattern).
// Raises tier to Destructive for the call, then restores.
// ---------------------------------------------------------------------------
namespace
{
	void CleanupInputAsset(FAutomationTestBase& Test, const FString& Path)
	{
		UAgentMcpSettings* Settings = GetMutableDefault<UAgentMcpSettings>();
		const EAgentMcpTier Saved = Settings->PermissionTier;
		Settings->PermissionTier = EAgentMcpTier::Destructive;
		ON_SCOPE_EXIT { Settings->PermissionTier = Saved; };
		bool bErr = false;
		AgentMcpTestUtils::CallTool(Test, TEXT("delete_asset"),
			FString::Printf(TEXT("{\"asset_path\":\"%s\"}"), *Path), bErr);
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FInputAssetCreationTest,
	"UnrealAgentMCP.Input.CreateActionAndContext",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FInputAssetCreationTest::RunTest(const FString& Parameters)
{
	// Unconditional cleanup so a failed run doesn't leave unsaved package debris.
	ON_SCOPE_EXIT
	{
		CleanupInputAsset(*this, TEXT("/Game/Dev/AgentMcpTests/IA_McpMove"));
		CleanupInputAsset(*this, TEXT("/Game/Dev/AgentMcpTests/IMC_McpDefault"));
	};

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

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FInputMappingEntryTest,
	"UnrealAgentMCP.Input.AddMappingEntry",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FInputMappingEntryTest::RunTest(const FString& Parameters)
{
	// Unconditional cleanup so a failed run doesn't leave unsaved package debris.
	ON_SCOPE_EXIT
	{
		CleanupInputAsset(*this, TEXT("/Game/Dev/AgentMcpTests/IA_McpJump"));
		CleanupInputAsset(*this, TEXT("/Game/Dev/AgentMcpTests/IMC_McpJumpCtx"));
	};

	bool bIsError = false;

	// Fixture: fresh action + context via the tools themselves.
	AgentMcpTestUtils::CallTool(*this, TEXT("create_input_action"),
		TEXT("{\"asset_path\":\"/Game/Dev/AgentMcpTests/IA_McpJump\"}"), bIsError);
	AgentMcpTestUtils::CallTool(*this, TEXT("create_mapping_context"),
		TEXT("{\"asset_path\":\"/Game/Dev/AgentMcpTests/IMC_McpJumpCtx\"}"), bIsError);

	// Map SpaceBar.
	const TSharedPtr<FJsonObject> Mapped = AgentMcpTestUtils::CallTool(*this, TEXT("add_mapping_entry"),
		TEXT("{\"context_path\":\"/Game/Dev/AgentMcpTests/IMC_McpJumpCtx\",\"action_path\":\"/Game/Dev/AgentMcpTests/IA_McpJump\",\"key\":\"SpaceBar\"}"), bIsError);
	TestFalse(TEXT("add_mapping_entry ok"), bIsError);
	if (TestNotNull(TEXT("mapping payload parses"), Mapped.Get()))
	{
		TestEqual(TEXT("one mapping total"), static_cast<int32>(Mapped->GetNumberField(TEXT("total_mappings"))), 1);
	}
	// Live verification on the context object.
	UInputMappingContext* Live = FindObject<UInputMappingContext>(nullptr, TEXT("/Game/Dev/AgentMcpTests/IMC_McpJumpCtx.IMC_McpJumpCtx"));
	if (TestNotNull(TEXT("live context exists"), Live))
	{
		TestEqual(TEXT("live mapping count"), Live->GetMappings().Num(), 1);
	}

	// Bad key name -> tool error with examples.
	AgentMcpTestUtils::CallTool(*this, TEXT("add_mapping_entry"),
		TEXT("{\"context_path\":\"/Game/Dev/AgentMcpTests/IMC_McpJumpCtx\",\"action_path\":\"/Game/Dev/AgentMcpTests/IA_McpJump\",\"key\":\"NoSuchKeyXyz\"}"), bIsError);
	TestTrue(TEXT("bad key is a tool error"), bIsError);

	// Unknown context/action -> tool errors.
	AgentMcpTestUtils::CallTool(*this, TEXT("add_mapping_entry"),
		TEXT("{\"context_path\":\"/Game/Nope/IMC_X\",\"action_path\":\"/Game/Dev/AgentMcpTests/IA_McpJump\",\"key\":\"W\"}"), bIsError);
	TestTrue(TEXT("unknown context is a tool error"), bIsError);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FAutoLayoutTest,
	"UnrealAgentMCP.Input.AutoLayoutArrangesGraph",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FAutoLayoutTest::RunTest(const FString& Parameters)
{
	UBlueprint* Blueprint = AgentMcpTestUtils::MakeTransientBlueprint(TEXT("BP_McpLayoutTest"));
	if (!TestNotNull(TEXT("transient blueprint created"), Blueprint))
	{
		return true;
	}
	const FString Path = Blueprint->GetPathName();
	bool bIsError = false;

	// Build a 3-node chain, all piled at origin: BeginPlay -> PrintString -> PrintString2.
	const TSharedPtr<FJsonObject> Ev = AgentMcpTestUtils::CallTool(*this, TEXT("add_node"),
		FString::Printf(TEXT("{\"blueprint_path\":\"%s\",\"node_type\":\"event\",\"event_name\":\"ReceiveBeginPlay\"}"), *Path), bIsError);
	const TSharedPtr<FJsonObject> P1 = AgentMcpTestUtils::CallTool(*this, TEXT("add_node"),
		FString::Printf(TEXT("{\"blueprint_path\":\"%s\",\"node_type\":\"call_function\",\"class_name\":\"KismetSystemLibrary\",\"function_name\":\"PrintString\"}"), *Path), bIsError);
	const TSharedPtr<FJsonObject> P2 = AgentMcpTestUtils::CallTool(*this, TEXT("add_node"),
		FString::Printf(TEXT("{\"blueprint_path\":\"%s\",\"node_type\":\"call_function\",\"class_name\":\"KismetSystemLibrary\",\"function_name\":\"PrintString\"}"), *Path), bIsError);
	if (!Ev.IsValid() || !P1.IsValid() || !P2.IsValid()) { AddError(TEXT("fixture failed")); return true; }
	const FString EvId = Ev->GetStringField(TEXT("node_id"));
	const FString P1Id = P1->GetStringField(TEXT("node_id"));
	const FString P2Id = P2->GetStringField(TEXT("node_id"));
	AgentMcpTestUtils::CallTool(*this, TEXT("connect_pins"),
		FString::Printf(TEXT("{\"blueprint_path\":\"%s\",\"from_node_id\":\"%s\",\"from_pin\":\"then\",\"to_node_id\":\"%s\",\"to_pin\":\"execute\"}"), *Path, *EvId, *P1Id), bIsError);
	AgentMcpTestUtils::CallTool(*this, TEXT("connect_pins"),
		FString::Printf(TEXT("{\"blueprint_path\":\"%s\",\"from_node_id\":\"%s\",\"from_pin\":\"then\",\"to_node_id\":\"%s\",\"to_pin\":\"execute\"}"), *Path, *P1Id, *P2Id), bIsError);

	// auto_layout.
	const TSharedPtr<FJsonObject> Layout = AgentMcpTestUtils::CallTool(*this, TEXT("auto_layout"),
		FString::Printf(TEXT("{\"blueprint_path\":\"%s\"}"), *Path), bIsError);
	TestFalse(TEXT("auto_layout ok"), bIsError);
	if (TestNotNull(TEXT("layout payload parses"), Layout.Get()))
	{
		TestTrue(TEXT("laid out >= 3 nodes"), static_cast<int32>(Layout->GetNumberField(TEXT("laid_out"))) >= 3);
		TestTrue(TEXT("at least 3 layers (chain depth)"), static_cast<int32>(Layout->GetNumberField(TEXT("layers"))) >= 3);
	}

	// Verify via read_graph: the chain is strictly left-to-right and no two chain nodes overlap.
	const TSharedPtr<FJsonObject> View = AgentMcpTestUtils::CallTool(*this, TEXT("read_graph"),
		FString::Printf(TEXT("{\"blueprint_path\":\"%s\"}"), *Path), bIsError);
	double EvX = -1, P1X = -1, P2X = -1;
	TSet<FString> Positions;
	bool bOverlap = false;
	if (View.IsValid())
	{
		for (const TSharedPtr<FJsonValue>& NodeValue : View->GetArrayField(TEXT("nodes")))
		{
			const TSharedPtr<FJsonObject> Node = NodeValue->AsObject();
			const TSharedPtr<FJsonObject> Pos = Node->GetObjectField(TEXT("pos"));
			const FString PosKey = FString::Printf(TEXT("%d,%d"),
				static_cast<int32>(Pos->GetNumberField(TEXT("x"))), static_cast<int32>(Pos->GetNumberField(TEXT("y"))));
			bOverlap |= Positions.Contains(PosKey);
			Positions.Add(PosKey);
			const FString Id = Node->GetStringField(TEXT("id"));
			if (Id == EvId) { EvX = Pos->GetNumberField(TEXT("x")); }
			if (Id == P1Id) { P1X = Pos->GetNumberField(TEXT("x")); }
			if (Id == P2Id) { P2X = Pos->GetNumberField(TEXT("x")); }
		}
	}
	TestTrue(TEXT("chain flows left to right"), EvX < P1X && P1X < P2X);
	TestFalse(TEXT("no two nodes share a position"), bOverlap);
	return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS
