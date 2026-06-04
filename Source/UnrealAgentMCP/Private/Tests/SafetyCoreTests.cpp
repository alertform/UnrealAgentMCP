#include "Misc/AutomationTest.h"

#if WITH_DEV_AUTOMATION_TESTS

#include "Core/AgentMcpAuditLog.h"
#include "Core/AgentMcpToolRegistry.h"
#include "Core/McpTypes.h"
#include "AgentMcpSettings.h"
#include "Misc/ScopeExit.h"
#include "Tests/AgentMcpTestHelpers.h"
#include "UnrealAgentMCPModule.h"

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FSafetyAuditTrailTest,
	"UnrealAgentMCP.Safety.AuditTrailRecordsCalls",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FSafetyAuditTrailTest::RunTest(const FString& Parameters)
{
	bool bIsError = false;
	AgentMcpTestUtils::CallTool(*this, TEXT("engine_info"), TEXT("{}"), bIsError);
	TestFalse(TEXT("engine_info ok"), bIsError);

	const TArray<FString> Tail = FAgentMcpAuditLog::Get().Tail(20);
	bool bFound = false;
	for (const FString& Line : Tail)
	{
		if (Line.Contains(TEXT("\"tool\":\"engine_info\"")))
		{
			bFound = true;
			TestTrue(TEXT("audit line carries duration"), Line.Contains(TEXT("\"duration_ms\"")));
			TestTrue(TEXT("audit line carries is_error"), Line.Contains(TEXT("\"is_error\":false")));
		}
	}
	TestTrue(TEXT("engine_info call audited"), bFound);
	TestTrue(TEXT("audit file path is under Saved/AgentMCP"), FAgentMcpAuditLog::Get().CurrentFilePath().Contains(TEXT("AgentMCP")));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FSafetyTierEnforcementTest,
	"UnrealAgentMCP.Safety.TierCeilingRejectsDestructive",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FSafetyTierEnforcementTest::RunTest(const FString& Parameters)
{
	// Register a throwaway Destructive tool against the live registry (process-local; harmless).
	{
		FAgentMcpToolDef Def;
		Def.Name = TEXT("test_destructive_dummy");
		Def.Description = TEXT("Test-only destructive dummy");
		Def.InputSchema = MakeShared<FJsonObject>();
		Def.InputSchema->SetStringField(TEXT("type"), TEXT("object"));
		Def.Tier = EAgentMcpTier::Destructive;
		Def.Handler = FAgentMcpToolHandler::CreateLambda([](const TSharedPtr<FJsonObject>&)
		{
			return FAgentMcpToolResult::Success(TEXT("destructive-ran"));
		});
		FAgentMcpToolRegistry::Get().Register(MoveTemp(Def));
	}

	UAgentMcpSettings* Settings = GetMutableDefault<UAgentMcpSettings>();
	const EAgentMcpTier SavedCeiling = Settings->PermissionTier;
	ON_SCOPE_EXIT { GetMutableDefault<UAgentMcpSettings>()->PermissionTier = SavedCeiling; };

	bool bIsError = false;

	// Default ceiling SafeWrite: destructive call must be rejected with an actionable message.
	Settings->PermissionTier = EAgentMcpTier::SafeWrite;
	const FString Rejection = AgentMcpTestUtils::CallToolRawText(*this, TEXT("test_destructive_dummy"), TEXT("{}"), bIsError);
	TestTrue(TEXT("destructive rejected at SafeWrite ceiling"), bIsError);
	TestTrue(TEXT("rejection names required tier"), Rejection.Contains(TEXT("Destructive")));
	TestTrue(TEXT("rejection names current ceiling"), Rejection.Contains(TEXT("SafeWrite")));
	TestTrue(TEXT("rejection points at settings"), Rejection.Contains(TEXT("Project Settings")));

	// Wire-level structured discriminator: agents branch on this field, not on message text.
	// Parse rather than substring-match — the response writer's print policy is not our contract.
	const FString RawRejection = AgentMcp::Protocol::HandleMessage(
		TEXT("{\"jsonrpc\":\"2.0\",\"id\":9,\"method\":\"tools/call\",\"params\":{\"name\":\"test_destructive_dummy\"}}"));
	const TSharedPtr<FJsonObject> RawObj = AgentMcpTestUtils::Parse(RawRejection);
	bool bDiscriminatorPresent = false;
	if (RawObj.IsValid() && RawObj->HasField(TEXT("result")))
	{
		const TSharedPtr<FJsonObject> ResultObj = RawObj->GetObjectField(TEXT("result"));
		bDiscriminatorPresent = ResultObj->HasField(TEXT("rejected_by_tier")) && ResultObj->GetBoolField(TEXT("rejected_by_tier"));
	}
	TestTrue(TEXT("rejection result carries rejected_by_tier field"), bDiscriminatorPresent);

	// Rejection must be audited with rejected_by_tier.
	bool bAudited = false;
	for (const FString& Line : FAgentMcpAuditLog::Get().Tail(20))
	{
		bAudited |= (Line.Contains(TEXT("test_destructive_dummy")) && Line.Contains(TEXT("\"rejected_by_tier\":true")));
	}
	TestTrue(TEXT("tier rejection audited"), bAudited);

	// Raised ceiling: the same call now executes.
	Settings->PermissionTier = EAgentMcpTier::Destructive;
	const FString Allowed = AgentMcpTestUtils::CallToolRawText(*this, TEXT("test_destructive_dummy"), TEXT("{}"), bIsError);
	TestFalse(TEXT("destructive allowed at Destructive ceiling"), bIsError);
	TestEqual(TEXT("dummy actually ran"), Allowed, FString(TEXT("destructive-ran")));

	// ReadOnly ceiling blocks even SafeWrite tools.
	Settings->PermissionTier = EAgentMcpTier::ReadOnly;
	AgentMcpTestUtils::CallToolRawText(*this, TEXT("compile_blueprint"), TEXT("{\"blueprint_path\":\"/Game/Nope\"}"), bIsError);
	TestTrue(TEXT("SafeWrite tool rejected at ReadOnly ceiling"), bIsError);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FSafetyReadOutputLogTest,
	"UnrealAgentMCP.Safety.ReadOutputLogCapturesRecentLines",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FSafetyReadOutputLogTest::RunTest(const FString& Parameters)
{
	UE_LOG(LogAgentMcp, Display, TEXT("AuditMarker-XYZ123 from SafetyCoreTests"));

	bool bIsError = false;
	const TSharedPtr<FJsonObject> Payload = AgentMcpTestUtils::CallTool(*this, TEXT("read_output_log"),
		TEXT("{\"lines\":200,\"filter\":\"AuditMarker-XYZ123\"}"), bIsError);
	TestFalse(TEXT("read_output_log ok"), bIsError);
	if (TestNotNull(TEXT("payload parses"), Payload.Get()))
	{
		TestTrue(TEXT("marker found"), static_cast<int32>(Payload->GetNumberField(TEXT("returned"))) >= 1);
	}

	// Category filter narrows to LogAgentMcp lines only.
	const TSharedPtr<FJsonObject> ByCategory = AgentMcpTestUtils::CallTool(*this, TEXT("read_output_log"),
		TEXT("{\"lines\":200,\"category\":\"LogAgentMcp\",\"filter\":\"AuditMarker-XYZ123\"}"), bIsError);
	TestFalse(TEXT("category filter ok"), bIsError);
	if (ByCategory.IsValid())
	{
		TestTrue(TEXT("category filter still finds marker"), static_cast<int32>(ByCategory->GetNumberField(TEXT("returned"))) >= 1);
	}
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FSafetyUndoRedoTest,
	"UnrealAgentMCP.Safety.UndoRedoRoundTrip",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FSafetyUndoRedoTest::RunTest(const FString& Parameters)
{
	UBlueprint* Blueprint = AgentMcpTestUtils::MakeTransientBlueprint(TEXT("BP_McpUndoTest"));
	if (!TestNotNull(TEXT("transient blueprint created"), Blueprint))
	{
		return true;
	}
	const FString Path = Blueprint->GetPathName();
	bool bIsError = false;

	auto CountNodes = [&]() -> int32
	{
		const TSharedPtr<FJsonObject> View = AgentMcpTestUtils::CallTool(*this, TEXT("read_graph"),
			FString::Printf(TEXT("{\"blueprint_path\":\"%s\"}"), *Path), bIsError);
		return View.IsValid() ? static_cast<int32>(View->GetNumberField(TEXT("node_count"))) : -1;
	};

	const int32 Before = CountNodes();
	AgentMcpTestUtils::CallTool(*this, TEXT("add_node"),
		FString::Printf(TEXT("{\"blueprint_path\":\"%s\",\"node_type\":\"branch\"}"), *Path), bIsError);
	TestFalse(TEXT("add branch ok"), bIsError);
	TestEqual(TEXT("node added"), CountNodes(), Before + 1);

	// undo is editor-wide: this test assumes NO intervening transaction lands between the
	// add_node above and the undo below (read_graph is read-only; audit writes files, not transactions).
	const TSharedPtr<FJsonObject> UndoResult = AgentMcpTestUtils::CallTool(*this, TEXT("undo"), TEXT("{}"), bIsError);
	TestFalse(TEXT("undo tool ok"), bIsError);
	if (UndoResult.IsValid())
	{
		TestTrue(TEXT("undo applied"), UndoResult->GetBoolField(TEXT("undone")));
	}
	TestEqual(TEXT("node removed by undo"), CountNodes(), Before);

	const TSharedPtr<FJsonObject> RedoResult = AgentMcpTestUtils::CallTool(*this, TEXT("redo"), TEXT("{}"), bIsError);
	TestFalse(TEXT("redo tool ok"), bIsError);
	if (RedoResult.IsValid())
	{
		TestTrue(TEXT("redo applied"), RedoResult->GetBoolField(TEXT("redone")));
	}
	TestEqual(TEXT("node restored by redo"), CountNodes(), Before + 1);
	return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS
