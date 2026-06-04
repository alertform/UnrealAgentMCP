#include "Misc/AutomationTest.h"

#if WITH_DEV_AUTOMATION_TESTS

#include "Core/AgentMcpToolRegistry.h"
#include "Core/McpTypes.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"

namespace AgentMcpTestHelpers
{
	FAgentMcpToolDef MakeDummyTool(const FString& Name)
	{
		FAgentMcpToolDef Def;
		Def.Name = Name;
		Def.Description = TEXT("Dummy tool for tests");
		Def.InputSchema = MakeShared<FJsonObject>();
		Def.InputSchema->SetStringField(TEXT("type"), TEXT("object"));
		Def.Tier = EAgentMcpTier::ReadOnly;
		Def.Handler = FAgentMcpToolHandler::CreateLambda([](const TSharedPtr<FJsonObject>&)
		{
			return FAgentMcpToolResult::Success(TEXT("dummy-ok"));
		});
		return Def;
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FAgentMcpRegistryRegisterFindTest,
	"UnrealAgentMCP.Registry.RegisterAndFind",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FAgentMcpRegistryRegisterFindTest::RunTest(const FString& Parameters)
{
	FAgentMcpToolRegistry Registry;
	Registry.Register(AgentMcpTestHelpers::MakeDummyTool(TEXT("test_dummy")));

	const FAgentMcpToolDef* Found = Registry.Find(TEXT("test_dummy"));
	TestNotNull(TEXT("registered tool is findable"), Found);
	if (Found)
	{
		TestEqual(TEXT("name round-trips"), Found->Name, FString(TEXT("test_dummy")));
		const FAgentMcpToolResult Result = Found->Handler.Execute(nullptr);
		TestFalse(TEXT("dummy handler succeeds"), Result.bIsError);
		TestEqual(TEXT("dummy handler payload"), Result.Text, FString(TEXT("dummy-ok")));
	}
	TestNull(TEXT("unknown tool returns nullptr"), Registry.Find(TEXT("nope")));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FAgentMcpRegistryToolsJsonTest,
	"UnrealAgentMCP.Registry.BuildToolsJson",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FAgentMcpRegistryToolsJsonTest::RunTest(const FString& Parameters)
{
	FAgentMcpToolRegistry Registry;
	Registry.Register(AgentMcpTestHelpers::MakeDummyTool(TEXT("alpha")));
	Registry.Register(AgentMcpTestHelpers::MakeDummyTool(TEXT("beta")));

	const TArray<TSharedPtr<FJsonValue>> Tools = Registry.BuildToolsJson();
	TestEqual(TEXT("two tools serialized"), Tools.Num(), 2);

	bool bFoundAlpha = false;
	bool bFoundBeta = false;
	for (const TSharedPtr<FJsonValue>& Value : Tools)
	{
		const TSharedPtr<FJsonObject> Obj = Value->AsObject();
		if (Obj->GetStringField(TEXT("name")) == TEXT("alpha"))
		{
			bFoundAlpha = true;
			TestEqual(TEXT("description present"), Obj->GetStringField(TEXT("description")), FString(TEXT("Dummy tool for tests")));
			TestTrue(TEXT("inputSchema present"), Obj->HasField(TEXT("inputSchema")));
		}
		if (Obj->GetStringField(TEXT("name")) == TEXT("beta")) { bFoundBeta = true; }
	}
	TestTrue(TEXT("alpha found in tools json"), bFoundAlpha);
	TestTrue(TEXT("beta found in tools json"), bFoundBeta);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FAgentMcpRegistryOverwriteTest,
	"UnrealAgentMCP.Registry.DuplicateRegisterOverwrites",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FAgentMcpRegistryOverwriteTest::RunTest(const FString& Parameters)
{
	FAgentMcpToolRegistry Registry;
	Registry.Register(AgentMcpTestHelpers::MakeDummyTool(TEXT("dup")));
	FAgentMcpToolDef Second = AgentMcpTestHelpers::MakeDummyTool(TEXT("dup"));
	Second.Description = TEXT("Second registration wins");
	Registry.Register(MoveTemp(Second));

	TestEqual(TEXT("still one tool"), Registry.Num(), 1);
	const FAgentMcpToolDef* Found = Registry.Find(TEXT("dup"));
	if (Found)
	{
		TestEqual(TEXT("last registration wins"), Found->Description, FString(TEXT("Second registration wins")));
	}
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FAgentMcpRegistryRejectsInvalidTest,
	"UnrealAgentMCP.Registry.RejectsInvalidRegistration",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FAgentMcpRegistryRejectsInvalidTest::RunTest(const FString& Parameters)
{
	FAgentMcpToolRegistry Registry;

	AddExpectedMessage(TEXT("Rejected invalid tool registration"), ELogVerbosity::Warning, EAutomationExpectedMessageFlags::Contains, 2);

	FAgentMcpToolDef EmptyName;
	EmptyName.Handler = FAgentMcpToolHandler::CreateLambda([](const TSharedPtr<FJsonObject>&)
	{
		return FAgentMcpToolResult::Success(TEXT(""));
	});
	Registry.Register(MoveTemp(EmptyName));
	TestEqual(TEXT("empty-name registration rejected"), Registry.Num(), 0);

	FAgentMcpToolDef NoHandler;
	NoHandler.Name = TEXT("unbound");
	Registry.Register(MoveTemp(NoHandler));
	TestEqual(TEXT("unbound-handler registration rejected"), Registry.Num(), 0);

	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FAgentMcpRegistryNullSchemaJsonTest,
	"UnrealAgentMCP.Registry.NullSchemaSerializesEmptyObject",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FAgentMcpRegistryNullSchemaJsonTest::RunTest(const FString& Parameters)
{
	FAgentMcpToolRegistry Registry;

	FAgentMcpToolDef NoSchema;
	NoSchema.Name = TEXT("no_schema");
	NoSchema.Description = TEXT("Tool registered without an input schema");
	NoSchema.Handler = FAgentMcpToolHandler::CreateLambda([](const TSharedPtr<FJsonObject>&)
	{
		return FAgentMcpToolResult::Success(TEXT(""));
	});
	Registry.Register(MoveTemp(NoSchema));

	const TArray<TSharedPtr<FJsonValue>> Tools = Registry.BuildToolsJson();
	TestEqual(TEXT("one tool serialized"), Tools.Num(), 1);
	if (Tools.Num() == 1)
	{
		const TSharedPtr<FJsonObject> Obj = Tools[0]->AsObject();
		TestTrue(TEXT("inputSchema field present even without schema"), Obj->HasField(TEXT("inputSchema")));
		TestEqual(TEXT("fallback schema is empty object"), Obj->GetObjectField(TEXT("inputSchema"))->Values.Num(), 0);
	}
	return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS
