#include "Misc/AutomationTest.h"

#if WITH_DEV_AUTOMATION_TESTS

#include "Core/AgentMcpAuditLog.h"
#include "Core/AgentMcpToolRegistry.h"
#include "Core/McpTypes.h"
#include "AgentMcpSettings.h"
#include "Misc/ScopeExit.h"
#include "Tests/AgentMcpTestHelpers.h"

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

#endif // WITH_DEV_AUTOMATION_TESTS
