#include "Misc/AutomationTest.h"

#if WITH_DEV_AUTOMATION_TESTS

#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "Engine/Blueprint.h"
#include "GameFramework/Actor.h"
#include "Kismet2/KismetEditorUtilities.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"
#include "Serialization/JsonWriter.h"
#include "Server/McpProtocol.h"
#include "UObject/Package.h"

namespace NodeGraphTestHelpers
{
	TSharedPtr<FJsonObject> Parse(const FString& Json)
	{
		TSharedPtr<FJsonObject> Obj;
		const TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(Json);
		FJsonSerializer::Deserialize(Reader, Obj);
		return Obj;
	}

	/** Creates a transient Actor blueprint that never touches disk (GC'd after the test run). */
	UBlueprint* MakeTransientBlueprint(const FString& BaseName)
	{
		const FName UniqueName = MakeUniqueObjectName(GetTransientPackage(), UBlueprint::StaticClass(), FName(*BaseName));
		// NOTE: verify exact CreateBlueprint signature in Kismet2/KismetEditorUtilities.h and adapt
		// (some 5.x overloads take CallingContext as the last param — default it).
		return FKismetEditorUtilities::CreateBlueprint(
			AActor::StaticClass(), GetTransientPackage(), UniqueName,
			BPTYPE_Normal, UBlueprint::StaticClass(), UBlueprintGeneratedClass::StaticClass());
	}

	/** Calls one MCP tool through the full protocol path; returns the parsed JSON payload of content[0].text. */
	TSharedPtr<FJsonObject> CallTool(FAutomationTestBase& Test, const FString& ToolName, const FString& ArgsJson, bool& bOutIsError)
	{
		const FString Request = FString::Printf(
			TEXT("{\"jsonrpc\":\"2.0\",\"id\":1,\"method\":\"tools/call\",\"params\":{\"name\":\"%s\",\"arguments\":%s}}"),
			*ToolName, *ArgsJson);
		const TSharedPtr<FJsonObject> Response = Parse(AgentMcp::Protocol::HandleMessage(Request));
		bOutIsError = true;
		if (!Test.TestTrue(FString::Printf(TEXT("%s response has result"), *ToolName),
			Response.IsValid() && Response->HasField(TEXT("result"))))
		{
			return nullptr;
		}
		const TSharedPtr<FJsonObject> Result = Response->GetObjectField(TEXT("result"));
		bOutIsError = Result->GetBoolField(TEXT("isError"));
		const TArray<TSharedPtr<FJsonValue>>& Content = Result->GetArrayField(TEXT("content"));
		if (Content.Num() == 0)
		{
			return nullptr;
		}
		const FString Text = Content[0]->AsObject()->GetStringField(TEXT("text"));
		return Parse(Text); // nullptr is fine for plain-text error payloads
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FNodeGraphReadGraphTest,
	"UnrealAgentMCP.NodeGraph.ReadGraphReturnsEventGraph",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FNodeGraphReadGraphTest::RunTest(const FString& Parameters)
{
	UBlueprint* Blueprint = NodeGraphTestHelpers::MakeTransientBlueprint(TEXT("BP_McpReadGraphTest"));
	if (!TestNotNull(TEXT("transient blueprint created"), Blueprint))
	{
		return true;
	}
	const FString Path = Blueprint->GetPathName();

	bool bIsError = false;
	const TSharedPtr<FJsonObject> Payload = NodeGraphTestHelpers::CallTool(*this, TEXT("read_graph"),
		FString::Printf(TEXT("{\"blueprint_path\":\"%s\"}"), *Path), bIsError);

	TestFalse(TEXT("read_graph succeeds"), bIsError);
	if (TestNotNull(TEXT("payload parses as JSON"), Payload.Get()))
	{
		TestEqual(TEXT("graph is EventGraph"), Payload->GetStringField(TEXT("graph")), FString(TEXT("EventGraph")));
		TestTrue(TEXT("has nodes array"), Payload->HasField(TEXT("nodes")));
		// A fresh Actor BP's event graph contains ghost default events (BeginPlay/Tick/ActorBeginOverlap).
		const TArray<TSharedPtr<FJsonValue>>& Nodes = Payload->GetArrayField(TEXT("nodes"));
		TestEqual(TEXT("node_count matches array"), static_cast<int32>(Payload->GetNumberField(TEXT("node_count"))), Nodes.Num());
		for (const TSharedPtr<FJsonValue>& NodeValue : Nodes)
		{
			const TSharedPtr<FJsonObject> Node = NodeValue->AsObject();
			TestTrue(TEXT("node has id"), Node->HasField(TEXT("id")));
			TestTrue(TEXT("node has class"), Node->HasField(TEXT("class")));
			TestTrue(TEXT("node has pins"), Node->HasField(TEXT("pins")));
		}
	}

	// Unknown blueprint path -> tool error, not crash.
	const TSharedPtr<FJsonObject> BadPayload = NodeGraphTestHelpers::CallTool(*this, TEXT("read_graph"),
		TEXT("{\"blueprint_path\":\"/Game/DoesNotExist/BP_Nope\"}"), bIsError);
	TestTrue(TEXT("unknown blueprint is a tool error"), bIsError);

	// Unknown graph name -> tool error listing available graphs.
	const TSharedPtr<FJsonObject> BadGraph = NodeGraphTestHelpers::CallTool(*this, TEXT("read_graph"),
		FString::Printf(TEXT("{\"blueprint_path\":\"%s\",\"graph_name\":\"NoSuchGraph\"}"), *Path), bIsError);
	TestTrue(TEXT("unknown graph is a tool error"), bIsError);
	return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS
