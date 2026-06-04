#include "Misc/AutomationTest.h"

#if WITH_DEV_AUTOMATION_TESTS

#include "Tests/AgentMcpTestHelpers.h"

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FNodeGraphReadGraphTest,
	"UnrealAgentMCP.NodeGraph.ReadGraphReturnsEventGraph",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FNodeGraphReadGraphTest::RunTest(const FString& Parameters)
{
	UBlueprint* Blueprint = AgentMcpTestUtils::MakeTransientBlueprint(TEXT("BP_McpReadGraphTest"));
	if (!TestNotNull(TEXT("transient blueprint created"), Blueprint))
	{
		return true;
	}
	const FString Path = Blueprint->GetPathName();

	bool bIsError = false;
	const TSharedPtr<FJsonObject> Payload = AgentMcpTestUtils::CallTool(*this, TEXT("read_graph"),
		FString::Printf(TEXT("{\"blueprint_path\":\"%s\"}"), *Path), bIsError);

	TestFalse(TEXT("read_graph succeeds"), bIsError);
	if (TestNotNull(TEXT("payload parses as JSON"), Payload.Get()))
	{
		// Engine may suffix the ubergraph name (EventGraph_0) under name pressure; assert the family, not the literal.
		TestTrue(TEXT("graph is an event graph"), Payload->GetStringField(TEXT("graph")).StartsWith(TEXT("EventGraph")));
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
	const TSharedPtr<FJsonObject> BadPayload = AgentMcpTestUtils::CallTool(*this, TEXT("read_graph"),
		TEXT("{\"blueprint_path\":\"/Game/DoesNotExist/BP_Nope\"}"), bIsError);
	TestTrue(TEXT("unknown blueprint is a tool error"), bIsError);

	// Unknown graph name -> tool error listing available graphs.
	const TSharedPtr<FJsonObject> BadGraph = AgentMcpTestUtils::CallTool(*this, TEXT("read_graph"),
		FString::Printf(TEXT("{\"blueprint_path\":\"%s\",\"graph_name\":\"NoSuchGraph\"}"), *Path), bIsError);
	TestTrue(TEXT("unknown graph is a tool error"), bIsError);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FBlueprintToolsCreateCompileTest,
	"UnrealAgentMCP.NodeGraph.CreateAndCompileBlueprint",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FBlueprintToolsCreateCompileTest::RunTest(const FString& Parameters)
{
	bool bIsError = false;

	// create_blueprint creates an in-memory asset (not saved to disk; saving is a P3 tool).
	const TSharedPtr<FJsonObject> Created = AgentMcpTestUtils::CallTool(*this, TEXT("create_blueprint"),
		TEXT("{\"asset_path\":\"/Game/Dev/AgentMcpTests/BP_McpCreated\",\"parent_class\":\"Actor\"}"), bIsError);
	TestFalse(TEXT("create_blueprint succeeds"), bIsError);
	FString CreatedPath;
	if (TestNotNull(TEXT("create payload parses"), Created.Get()))
	{
		CreatedPath = Created->GetStringField(TEXT("blueprint_path"));
		TestTrue(TEXT("created path echoes asset path"), CreatedPath.Contains(TEXT("BP_McpCreated")));
	}

	// Duplicate creation must be a tool error, not a crash/overwrite.
	AgentMcpTestUtils::CallTool(*this, TEXT("create_blueprint"),
		TEXT("{\"asset_path\":\"/Game/Dev/AgentMcpTests/BP_McpCreated\"}"), bIsError);
	TestTrue(TEXT("duplicate create is a tool error"), bIsError);

	// Unknown parent class -> tool error.
	AgentMcpTestUtils::CallTool(*this, TEXT("create_blueprint"),
		TEXT("{\"asset_path\":\"/Game/Dev/AgentMcpTests/BP_McpBadParent\",\"parent_class\":\"NoSuchClassXyz\"}"), bIsError);
	TestTrue(TEXT("unknown parent class is a tool error"), bIsError);

	// compile_blueprint on the freshly created (empty) BP: zero errors, isError false.
	const TSharedPtr<FJsonObject> Compiled = AgentMcpTestUtils::CallTool(*this, TEXT("compile_blueprint"),
		FString::Printf(TEXT("{\"blueprint_path\":\"%s\"}"), *CreatedPath), bIsError);
	TestFalse(TEXT("compile_blueprint tool succeeds"), bIsError);
	if (TestNotNull(TEXT("compile payload parses"), Compiled.Get()))
	{
		TestEqual(TEXT("no compile errors"), static_cast<int32>(Compiled->GetNumberField(TEXT("num_errors"))), 0);
		TestEqual(TEXT("status ok"), Compiled->GetStringField(TEXT("status")), FString(TEXT("ok")));
	}
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FNodeGraphAddNodeTest,
	"UnrealAgentMCP.NodeGraph.AddNodeTypes",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FNodeGraphAddNodeTest::RunTest(const FString& Parameters)
{
	UBlueprint* Blueprint = AgentMcpTestUtils::MakeTransientBlueprint(TEXT("BP_McpAddNodeTest"));
	if (!TestNotNull(TEXT("transient blueprint created"), Blueprint))
	{
		return true;
	}
	const FString Path = Blueprint->GetPathName();
	bool bIsError = false;

	// call_function: KismetSystemLibrary.PrintString
	const TSharedPtr<FJsonObject> CallNode = AgentMcpTestUtils::CallTool(*this, TEXT("add_node"),
		FString::Printf(TEXT("{\"blueprint_path\":\"%s\",\"node_type\":\"call_function\",\"class_name\":\"KismetSystemLibrary\",\"function_name\":\"PrintString\",\"pos_x\":400,\"pos_y\":100}"), *Path), bIsError);
	TestFalse(TEXT("add call_function succeeds"), bIsError);
	if (TestNotNull(TEXT("call node payload parses"), CallNode.Get()))
	{
		TestTrue(TEXT("returns node_id"), CallNode->HasField(TEXT("node_id")));
		bool bHasInString = false;
		for (const TSharedPtr<FJsonValue>& PinValue : CallNode->GetArrayField(TEXT("pins")))
		{
			bHasInString |= (PinValue->AsObject()->GetStringField(TEXT("name")) == TEXT("InString"));
		}
		TestTrue(TEXT("PrintString node exposes InString pin"), bHasInString);
	}

	// event: ReceiveBeginPlay — first call returns an enabled node...
	const TSharedPtr<FJsonObject> EventNode = AgentMcpTestUtils::CallTool(*this, TEXT("add_node"),
		FString::Printf(TEXT("{\"blueprint_path\":\"%s\",\"node_type\":\"event\",\"event_name\":\"ReceiveBeginPlay\"}"), *Path), bIsError);
	TestFalse(TEXT("add event succeeds"), bIsError);
	FString FirstEventId;
	if (TestNotNull(TEXT("event payload parses"), EventNode.Get()))
	{
		FirstEventId = EventNode->GetStringField(TEXT("node_id"));
	}
	// ...second call must reuse it (existing: true, same id), never duplicate.
	const TSharedPtr<FJsonObject> EventAgain = AgentMcpTestUtils::CallTool(*this, TEXT("add_node"),
		FString::Printf(TEXT("{\"blueprint_path\":\"%s\",\"node_type\":\"event\",\"event_name\":\"ReceiveBeginPlay\"}"), *Path), bIsError);
	TestFalse(TEXT("re-add event succeeds"), bIsError);
	if (TestNotNull(TEXT("second event payload parses"), EventAgain.Get()))
	{
		TestTrue(TEXT("second add reports existing"), EventAgain->GetBoolField(TEXT("existing")));
		TestEqual(TEXT("same node id"), EventAgain->GetStringField(TEXT("node_id")), FirstEventId);
	}

	// Extra assertion: the event node must be enabled (not a ghost) so compile and execution work.
	const TSharedPtr<FJsonObject> GraphAfterEvent = AgentMcpTestUtils::CallTool(*this, TEXT("read_graph"),
		FString::Printf(TEXT("{\"blueprint_path\":\"%s\"}"), *Path), bIsError);
	TestFalse(TEXT("read_graph after add event succeeds"), bIsError);
	if (TestNotNull(TEXT("graph view parses"), GraphAfterEvent.Get()))
	{
		bool bFoundEventEnabled = false;
		for (const TSharedPtr<FJsonValue>& NodeValue : GraphAfterEvent->GetArrayField(TEXT("nodes")))
		{
			const TSharedPtr<FJsonObject> Node = NodeValue->AsObject();
			if (Node->GetStringField(TEXT("id")) == FirstEventId)
			{
				bFoundEventEnabled = Node->GetBoolField(TEXT("enabled"));
				break;
			}
		}
		TestTrue(TEXT("event node reports enabled==true via read_graph"), bFoundEventEnabled);
	}

	// branch + sequence + self spawn fine.
	AgentMcpTestUtils::CallTool(*this, TEXT("add_node"),
		FString::Printf(TEXT("{\"blueprint_path\":\"%s\",\"node_type\":\"branch\"}"), *Path), bIsError);
	TestFalse(TEXT("add branch succeeds"), bIsError);
	AgentMcpTestUtils::CallTool(*this, TEXT("add_node"),
		FString::Printf(TEXT("{\"blueprint_path\":\"%s\",\"node_type\":\"sequence\"}"), *Path), bIsError);
	TestFalse(TEXT("add sequence succeeds"), bIsError);
	AgentMcpTestUtils::CallTool(*this, TEXT("add_node"),
		FString::Printf(TEXT("{\"blueprint_path\":\"%s\",\"node_type\":\"self\"}"), *Path), bIsError);
	TestFalse(TEXT("add self succeeds"), bIsError);

	// Errors: unknown function / unknown node_type are tool errors.
	AgentMcpTestUtils::CallTool(*this, TEXT("add_node"),
		FString::Printf(TEXT("{\"blueprint_path\":\"%s\",\"node_type\":\"call_function\",\"class_name\":\"KismetSystemLibrary\",\"function_name\":\"NoSuchFunctionXyz\"}"), *Path), bIsError);
	TestTrue(TEXT("unknown function is a tool error"), bIsError);
	AgentMcpTestUtils::CallTool(*this, TEXT("add_node"),
		FString::Printf(TEXT("{\"blueprint_path\":\"%s\",\"node_type\":\"flux_capacitor\"}"), *Path), bIsError);
	TestTrue(TEXT("unknown node_type is a tool error"), bIsError);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FNodeGraphConnectPinsTest,
	"UnrealAgentMCP.NodeGraph.ConnectPinsAndDefaults",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FNodeGraphConnectPinsTest::RunTest(const FString& Parameters)
{
	UBlueprint* Blueprint = AgentMcpTestUtils::MakeTransientBlueprint(TEXT("BP_McpConnectTest"));
	if (!TestNotNull(TEXT("transient blueprint created"), Blueprint))
	{
		return true;
	}
	const FString Path = Blueprint->GetPathName();
	bool bIsError = false;

	const TSharedPtr<FJsonObject> EventNode = AgentMcpTestUtils::CallTool(*this, TEXT("add_node"),
		FString::Printf(TEXT("{\"blueprint_path\":\"%s\",\"node_type\":\"event\",\"event_name\":\"ReceiveBeginPlay\"}"), *Path), bIsError);
	const TSharedPtr<FJsonObject> PrintNode = AgentMcpTestUtils::CallTool(*this, TEXT("add_node"),
		FString::Printf(TEXT("{\"blueprint_path\":\"%s\",\"node_type\":\"call_function\",\"class_name\":\"KismetSystemLibrary\",\"function_name\":\"PrintString\"}"), *Path), bIsError);
	if (!TestNotNull(TEXT("event created"), EventNode.Get()) || !TestNotNull(TEXT("print created"), PrintNode.Get()))
	{
		return true;
	}
	const FString EventId = EventNode->GetStringField(TEXT("node_id"));
	const FString PrintId = PrintNode->GetStringField(TEXT("node_id"));

	// Exec link: BeginPlay.then -> PrintString.execute
	AgentMcpTestUtils::CallTool(*this, TEXT("connect_pins"),
		FString::Printf(TEXT("{\"blueprint_path\":\"%s\",\"from_node_id\":\"%s\",\"from_pin\":\"then\",\"to_node_id\":\"%s\",\"to_pin\":\"execute\"}"), *Path, *EventId, *PrintId), bIsError);
	TestFalse(TEXT("exec connect succeeds"), bIsError);

	// Verify via read_graph that the link is real.
	const TSharedPtr<FJsonObject> GraphView = AgentMcpTestUtils::CallTool(*this, TEXT("read_graph"),
		FString::Printf(TEXT("{\"blueprint_path\":\"%s\"}"), *Path), bIsError);
	bool bLinkFound = false;
	if (TestNotNull(TEXT("graph view parses"), GraphView.Get()))
	{
		for (const TSharedPtr<FJsonValue>& NodeValue : GraphView->GetArrayField(TEXT("nodes")))
		{
			const TSharedPtr<FJsonObject> Node = NodeValue->AsObject();
			if (Node->GetStringField(TEXT("id")) != EventId) { continue; }
			for (const TSharedPtr<FJsonValue>& PinValue : Node->GetArrayField(TEXT("pins")))
			{
				const TSharedPtr<FJsonObject> Pin = PinValue->AsObject();
				if (Pin->GetStringField(TEXT("name")) != TEXT("then")) { continue; }
				for (const TSharedPtr<FJsonValue>& LinkValue : Pin->GetArrayField(TEXT("links")))
				{
					bLinkFound |= (LinkValue->AsObject()->GetStringField(TEXT("node_id")) == PrintId);
				}
			}
		}
	}
	TestTrue(TEXT("read_graph shows the exec link"), bLinkFound);

	// Illegal link (exec out -> exec out) must return the schema's reason text.
	AgentMcpTestUtils::CallTool(*this, TEXT("connect_pins"),
		FString::Printf(TEXT("{\"blueprint_path\":\"%s\",\"from_node_id\":\"%s\",\"from_pin\":\"then\",\"to_node_id\":\"%s\",\"to_pin\":\"then\"}"), *Path, *EventId, *PrintId), bIsError);
	TestTrue(TEXT("illegal connect is a tool error"), bIsError);

	// set_pin_default on InString, then verify via read_graph.
	AgentMcpTestUtils::CallTool(*this, TEXT("set_pin_default"),
		FString::Printf(TEXT("{\"blueprint_path\":\"%s\",\"node_id\":\"%s\",\"pin_name\":\"InString\",\"value\":\"Hello from MCP\"}"), *Path, *PrintId), bIsError);
	TestFalse(TEXT("set_pin_default succeeds"), bIsError);
	const TSharedPtr<FJsonObject> GraphView2 = AgentMcpTestUtils::CallTool(*this, TEXT("read_graph"),
		FString::Printf(TEXT("{\"blueprint_path\":\"%s\"}"), *Path), bIsError);
	bool bDefaultFound = false;
	if (GraphView2.IsValid())
	{
		for (const TSharedPtr<FJsonValue>& NodeValue : GraphView2->GetArrayField(TEXT("nodes")))
		{
			const TSharedPtr<FJsonObject> Node = NodeValue->AsObject();
			if (Node->GetStringField(TEXT("id")) != PrintId) { continue; }
			for (const TSharedPtr<FJsonValue>& PinValue : Node->GetArrayField(TEXT("pins")))
			{
				const TSharedPtr<FJsonObject> Pin = PinValue->AsObject();
				if (Pin->GetStringField(TEXT("name")) == TEXT("InString"))
				{
					FString DefaultValue;
					Pin->TryGetStringField(TEXT("default_value"), DefaultValue);
					bDefaultFound = (DefaultValue == TEXT("Hello from MCP"));
				}
			}
		}
	}
	TestTrue(TEXT("read_graph shows the new default"), bDefaultFound);

	// Schema-rejected default (exec pin) must be a tool error, not a silent {"set":true}.
	AgentMcpTestUtils::CallTool(*this, TEXT("set_pin_default"),
		FString::Printf(TEXT("{\"blueprint_path\":\"%s\",\"node_id\":\"%s\",\"pin_name\":\"execute\",\"value\":\"whatever\"}"), *Path, *PrintId), bIsError);
	TestTrue(TEXT("default on exec pin is a tool error"), bIsError);

	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FNodeGraphClosedLoopTest,
	"UnrealAgentMCP.NodeGraph.P2AcceptanceClosedLoop",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FNodeGraphClosedLoopTest::RunTest(const FString& Parameters)
{
	// The P2 exit criterion as a test: BeginPlay -> PrintString("Hello from MCP"), compiled clean.
	UBlueprint* Blueprint = AgentMcpTestUtils::MakeTransientBlueprint(TEXT("BP_McpClosedLoop"));
	if (!TestNotNull(TEXT("transient blueprint created"), Blueprint))
	{
		return true;
	}
	const FString Path = Blueprint->GetPathName();
	bool bIsError = false;

	const TSharedPtr<FJsonObject> EventNode = AgentMcpTestUtils::CallTool(*this, TEXT("add_node"),
		FString::Printf(TEXT("{\"blueprint_path\":\"%s\",\"node_type\":\"event\",\"event_name\":\"ReceiveBeginPlay\"}"), *Path), bIsError);
	const TSharedPtr<FJsonObject> PrintNode = AgentMcpTestUtils::CallTool(*this, TEXT("add_node"),
		FString::Printf(TEXT("{\"blueprint_path\":\"%s\",\"node_type\":\"call_function\",\"class_name\":\"KismetSystemLibrary\",\"function_name\":\"PrintString\",\"pos_x\":400}"), *Path), bIsError);
	if (!EventNode.IsValid() || !PrintNode.IsValid())
	{
		AddError(TEXT("node creation failed; aborting closed loop"));
		return true;
	}
	const FString EventId = EventNode->GetStringField(TEXT("node_id"));
	const FString PrintId = PrintNode->GetStringField(TEXT("node_id"));

	AgentMcpTestUtils::CallTool(*this, TEXT("connect_pins"),
		FString::Printf(TEXT("{\"blueprint_path\":\"%s\",\"from_node_id\":\"%s\",\"from_pin\":\"then\",\"to_node_id\":\"%s\",\"to_pin\":\"execute\"}"), *Path, *EventId, *PrintId), bIsError);
	TestFalse(TEXT("connect succeeds"), bIsError);
	AgentMcpTestUtils::CallTool(*this, TEXT("set_pin_default"),
		FString::Printf(TEXT("{\"blueprint_path\":\"%s\",\"node_id\":\"%s\",\"pin_name\":\"InString\",\"value\":\"Hello from MCP\"}"), *Path, *PrintId), bIsError);
	TestFalse(TEXT("default set succeeds"), bIsError);

	const TSharedPtr<FJsonObject> Compiled = AgentMcpTestUtils::CallTool(*this, TEXT("compile_blueprint"),
		FString::Printf(TEXT("{\"blueprint_path\":\"%s\"}"), *Path), bIsError);
	TestFalse(TEXT("compile tool succeeds"), bIsError);
	if (TestNotNull(TEXT("compile payload parses"), Compiled.Get()))
	{
		TestEqual(TEXT("closed loop compiles clean"), static_cast<int32>(Compiled->GetNumberField(TEXT("num_errors"))), 0);
	}

	// delete_node removes PrintString; graph shrinks; unknown id errors.
	AgentMcpTestUtils::CallTool(*this, TEXT("delete_node"),
		FString::Printf(TEXT("{\"blueprint_path\":\"%s\",\"node_id\":\"%s\"}"), *Path, *PrintId), bIsError);
	TestFalse(TEXT("delete succeeds"), bIsError);
	const TSharedPtr<FJsonObject> GraphView = AgentMcpTestUtils::CallTool(*this, TEXT("read_graph"),
		FString::Printf(TEXT("{\"blueprint_path\":\"%s\"}"), *Path), bIsError);
	if (GraphView.IsValid())
	{
		for (const TSharedPtr<FJsonValue>& NodeValue : GraphView->GetArrayField(TEXT("nodes")))
		{
			TestNotEqual(TEXT("PrintString node is gone"), NodeValue->AsObject()->GetStringField(TEXT("id")), PrintId);
		}
	}
	AgentMcpTestUtils::CallTool(*this, TEXT("delete_node"),
		FString::Printf(TEXT("{\"blueprint_path\":\"%s\",\"node_id\":\"%s\"}"), *Path, *PrintId), bIsError);
	TestTrue(TEXT("double delete is a tool error"), bIsError);

	// Deliberately out of scope here: a broken-graph -> compile-error round-trip case. add_node's
	// validation makes an invalid graph hard to construct deterministically through the tools alone.
	// TODO(P3): force a compile error via a post-hoc renamed variable on a second BP and assert the
	// error text reaches the agent through compile_blueprint's messages array.
	return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS
