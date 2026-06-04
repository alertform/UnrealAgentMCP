#include "Misc/AutomationTest.h"

#if WITH_DEV_AUTOMATION_TESTS

#include "Core/AgentMcpAuditLog.h"
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

#endif // WITH_DEV_AUTOMATION_TESTS
