#include "Misc/AutomationTest.h"

#if WITH_DEV_AUTOMATION_TESTS

#include "Server/McpProtocol.h"
#include "Dom/JsonObject.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"

namespace McpProtocolTestHelpers
{
	TSharedPtr<FJsonObject> Parse(const FString& Json)
	{
		TSharedPtr<FJsonObject> Obj;
		const TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(Json);
		FJsonSerializer::Deserialize(Reader, Obj);
		return Obj;
	}

	int32 GetErrorCode(const TSharedPtr<FJsonObject>& Response)
	{
		const TSharedPtr<FJsonObject>* ErrorObj = nullptr;
		if (Response.IsValid() && Response->TryGetObjectField(TEXT("error"), ErrorObj))
		{
			return static_cast<int32>((*ErrorObj)->GetNumberField(TEXT("code")));
		}
		return 0;
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FMcpProtocolParseErrorTest,
	"UnrealAgentMCP.Protocol.ParseErrorReturns32700",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FMcpProtocolParseErrorTest::RunTest(const FString& Parameters)
{
	const FString Response = AgentMcp::Protocol::HandleMessage(TEXT("this is not json"));
	const TSharedPtr<FJsonObject> Obj = McpProtocolTestHelpers::Parse(Response);
	TestNotNull(TEXT("response parses"), Obj.Get());
	TestEqual(TEXT("parse error code"), McpProtocolTestHelpers::GetErrorCode(Obj), -32700);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FMcpProtocolInvalidRequestTest,
	"UnrealAgentMCP.Protocol.MissingMethodReturns32600",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FMcpProtocolInvalidRequestTest::RunTest(const FString& Parameters)
{
	const FString Response = AgentMcp::Protocol::HandleMessage(TEXT("{\"jsonrpc\":\"2.0\",\"id\":1}"));
	TestEqual(TEXT("invalid request code"), McpProtocolTestHelpers::GetErrorCode(McpProtocolTestHelpers::Parse(Response)), -32600);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FMcpProtocolMethodNotFoundTest,
	"UnrealAgentMCP.Protocol.UnknownMethodReturns32601",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FMcpProtocolMethodNotFoundTest::RunTest(const FString& Parameters)
{
	const FString Response = AgentMcp::Protocol::HandleMessage(TEXT("{\"jsonrpc\":\"2.0\",\"id\":7,\"method\":\"bogus/method\"}"));
	const TSharedPtr<FJsonObject> Obj = McpProtocolTestHelpers::Parse(Response);
	TestEqual(TEXT("method not found code"), McpProtocolTestHelpers::GetErrorCode(Obj), -32601);
	TestEqual(TEXT("id echoed"), static_cast<int32>(Obj->GetNumberField(TEXT("id"))), 7);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FMcpProtocolInitializeTest,
	"UnrealAgentMCP.Protocol.InitializeNegotiatesVersion",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FMcpProtocolInitializeTest::RunTest(const FString& Parameters)
{
	const FString Request = TEXT("{\"jsonrpc\":\"2.0\",\"id\":1,\"method\":\"initialize\",\"params\":{\"protocolVersion\":\"2025-06-18\",\"capabilities\":{},\"clientInfo\":{\"name\":\"test\",\"version\":\"0.0.0\"}}}");
	const TSharedPtr<FJsonObject> Obj = McpProtocolTestHelpers::Parse(AgentMcp::Protocol::HandleMessage(Request));
	const TSharedPtr<FJsonObject> Result = Obj->GetObjectField(TEXT("result"));
	TestEqual(TEXT("known version echoed"), Result->GetStringField(TEXT("protocolVersion")), FString(TEXT("2025-06-18")));
	TestEqual(TEXT("server name"), Result->GetObjectField(TEXT("serverInfo"))->GetStringField(TEXT("name")), FString(TEXT("UnrealAgentMCP")));
	TestTrue(TEXT("tools capability declared"), Result->GetObjectField(TEXT("capabilities"))->HasField(TEXT("tools")));

	const FString OldRequest = TEXT("{\"jsonrpc\":\"2.0\",\"id\":2,\"method\":\"initialize\",\"params\":{\"protocolVersion\":\"1999-01-01\"}}");
	const TSharedPtr<FJsonObject> OldObj = McpProtocolTestHelpers::Parse(AgentMcp::Protocol::HandleMessage(OldRequest));
	TestEqual(TEXT("unknown version falls back to latest"),
		OldObj->GetObjectField(TEXT("result"))->GetStringField(TEXT("protocolVersion")), FString(TEXT("2025-06-18")));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FMcpProtocolPingTest,
	"UnrealAgentMCP.Protocol.PingReturnsEmptyResult",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FMcpProtocolPingTest::RunTest(const FString& Parameters)
{
	const TSharedPtr<FJsonObject> Obj = McpProtocolTestHelpers::Parse(
		AgentMcp::Protocol::HandleMessage(TEXT("{\"jsonrpc\":\"2.0\",\"id\":3,\"method\":\"ping\"}")));
	TestTrue(TEXT("has result"), Obj->HasField(TEXT("result")));
	TestFalse(TEXT("no error"), Obj->HasField(TEXT("error")));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FMcpProtocolNotificationTest,
	"UnrealAgentMCP.Protocol.NotificationsGetNoResponse",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FMcpProtocolNotificationTest::RunTest(const FString& Parameters)
{
	TestTrue(TEXT("initialized notification -> empty"),
		AgentMcp::Protocol::HandleMessage(TEXT("{\"jsonrpc\":\"2.0\",\"method\":\"notifications/initialized\"}")).IsEmpty());
	TestTrue(TEXT("unknown notification -> empty (never error a notification)"),
		AgentMcp::Protocol::HandleMessage(TEXT("{\"jsonrpc\":\"2.0\",\"method\":\"bogus/notification\"}")).IsEmpty());
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FMcpProtocolIdEdgeCasesTest,
	"UnrealAgentMCP.Protocol.IdEdgeCases",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FMcpProtocolIdEdgeCasesTest::RunTest(const FString& Parameters)
{
	// Missing jsonrpc field with a valid method and id -> -32600, not silence.
	{
		const FString Response = AgentMcp::Protocol::HandleMessage(TEXT("{\"id\":9,\"method\":\"ping\"}"));
		TestEqual(TEXT("missing jsonrpc is invalid request"),
			McpProtocolTestHelpers::GetErrorCode(McpProtocolTestHelpers::Parse(Response)), -32600);
	}
	// Explicit id:null is a REQUEST (gets a response with id null), not a notification.
	{
		const FString Response = AgentMcp::Protocol::HandleMessage(TEXT("{\"jsonrpc\":\"2.0\",\"id\":null,\"method\":\"ping\"}"));
		TestFalse(TEXT("id:null ping gets a response"), Response.IsEmpty());
		const TSharedPtr<FJsonObject> Obj = McpProtocolTestHelpers::Parse(Response);
		TestTrue(TEXT("response has result"), Obj.IsValid() && Obj->HasField(TEXT("result")));
		if (Obj.IsValid())
		{
			const TSharedPtr<FJsonValue> IdField = Obj->TryGetField(TEXT("id"));
			TestTrue(TEXT("id field present and null"), IdField.IsValid() && IdField->Type == EJson::Null);
		}
	}
	// String id round-trips with its type preserved.
	{
		const FString Response = AgentMcp::Protocol::HandleMessage(TEXT("{\"jsonrpc\":\"2.0\",\"id\":\"req-abc\",\"method\":\"ping\"}"));
		const TSharedPtr<FJsonObject> Obj = McpProtocolTestHelpers::Parse(Response);
		FString IdString;
		TestTrue(TEXT("string id echoed as string"), Obj.IsValid() && Obj->TryGetStringField(TEXT("id"), IdString));
		TestEqual(TEXT("string id value round-trips"), IdString, FString(TEXT("req-abc")));
	}
	return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS
