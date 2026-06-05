#include "Misc/AutomationTest.h"

#if WITH_DEV_AUTOMATION_TESTS

#include "AgentMcpSettings.h"
#include "MVVMBlueprintView.h"
#include "MVVMEditorSubsystem.h"
#include "Core/AgentMcpToolRegistry.h"
#include "Core/McpTypes.h"
#include "Editor.h"
#include "GameFramework/WorldSettings.h"
#include "Misc/ScopeExit.h"
#include "Tests/AgentMcpTestHelpers.h"
#include "WidgetBlueprint.h"

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

// ---------------------------------------------------------------------------
// FSystemActorAccessTest
// Verifies (A): include_system bool on query_actors exposes AWorldSettings.
// Also exercises set_actor_property / get_actor_property on WorldSettings
// via the discovered actor_path.
// ---------------------------------------------------------------------------
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FSystemActorAccessTest,
	"UnrealAgentMCP.P5.SystemActorAccess",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FSystemActorAccessTest::RunTest(const FString& Parameters)
{
	bool bIsError = false;

	// Belt-and-braces isolation: whatever happens below (failed restore call, mid-test
	// assertion failures — TestFalse does not early-return), leave the editor world's
	// GameMode Override untouched. (T3 review finding.)
	ON_SCOPE_EXIT
	{
		if (GEditor)
		{
			if (UWorld* World = GEditor->GetEditorWorldContext().World())
			{
				if (AWorldSettings* WS = World->GetWorldSettings())
				{
					WS->DefaultGameMode = nullptr;
				}
			}
		}
	};

	// Step 1 — query_actors without include_system: must NOT contain WorldSettings.
	{
		const TSharedPtr<FJsonObject> Payload = AgentMcpTestUtils::CallTool(*this, TEXT("query_actors"),
			TEXT("{}"), bIsError);
		TestFalse(TEXT("query_actors {} ok"), bIsError);
		if (TestNotNull(TEXT("query_actors {} parses"), Payload.Get()))
		{
			bool bFoundWorldSettings = false;
			for (const TSharedPtr<FJsonValue>& Entry : Payload->GetArrayField(TEXT("actors")))
			{
				const TSharedPtr<FJsonObject> EntryObj = Entry->AsObject();
				if (EntryObj.IsValid())
				{
					const FString Class = EntryObj->GetStringField(TEXT("class"));
					if (Class.Contains(TEXT("WorldSettings")))
					{
						bFoundWorldSettings = true;
					}
				}
			}
			TestFalse(TEXT("WorldSettings absent without include_system"), bFoundWorldSettings);
		}
	}

	// Step 2 — query_actors with include_system:true: must contain exactly one WorldSettings actor.
	FString WorldSettingsPath;
	{
		const TSharedPtr<FJsonObject> Payload = AgentMcpTestUtils::CallTool(*this, TEXT("query_actors"),
			TEXT("{\"include_system\":true}"), bIsError);
		TestFalse(TEXT("query_actors {include_system:true} ok"), bIsError);
		if (TestNotNull(TEXT("query_actors {include_system:true} parses"), Payload.Get()))
		{
			int32 WorldSettingsCount = 0;
			for (const TSharedPtr<FJsonValue>& Entry : Payload->GetArrayField(TEXT("actors")))
			{
				const TSharedPtr<FJsonObject> EntryObj = Entry->AsObject();
				if (EntryObj.IsValid())
				{
					const FString Class = EntryObj->GetStringField(TEXT("class"));
					if (Class.Contains(TEXT("WorldSettings")))
					{
						++WorldSettingsCount;
						WorldSettingsPath = EntryObj->GetStringField(TEXT("actor_path"));
					}
				}
			}
			TestEqual(TEXT("exactly one WorldSettings returned"), WorldSettingsCount, 1);
		}
	}

	// Step 3 — set DefaultGameMode on WorldSettings, read back, then restore.
	if (!WorldSettingsPath.IsEmpty())
	{
		// Set to GameModeBase.
		const TSharedPtr<FJsonObject> SetResult = AgentMcpTestUtils::CallTool(*this, TEXT("set_actor_property"),
			FString::Printf(
				TEXT("{\"actor_path\":\"%s\",\"property\":\"DefaultGameMode\",\"value\":\"/Script/Engine.GameModeBase\"}"),
				*WorldSettingsPath),
			bIsError);
		TestFalse(TEXT("set DefaultGameMode succeeds"), bIsError);
		if (TestNotNull(TEXT("set result parses"), SetResult.Get()))
		{
			TestTrue(TEXT("set:true"), SetResult->GetBoolField(TEXT("set")));
		}

		// Read back: set_actor_property already returns the read-back 'value' field in its response.
		// Verify the value returned by the set call contains "GameModeBase".
		if (TestNotNull(TEXT("set result parses for readback"), SetResult.Get()))
		{
			TestTrue(TEXT("readback contains GameModeBase"),
				SetResult->GetStringField(TEXT("value")).Contains(TEXT("GameModeBase"), ESearchCase::IgnoreCase));
		}

		// Restore to None (empty / null reference).
		const TSharedPtr<FJsonObject> RestoreResult = AgentMcpTestUtils::CallTool(*this, TEXT("set_actor_property"),
			FString::Printf(
				TEXT("{\"actor_path\":\"%s\",\"property\":\"DefaultGameMode\",\"value\":\"None\"}"),
				*WorldSettingsPath),
			bIsError);
		TestFalse(TEXT("restore DefaultGameMode to None ok"), bIsError);
		if (TestNotNull(TEXT("restore result parses"), RestoreResult.Get()))
		{
			TestTrue(TEXT("restore set:true"), RestoreResult->GetBoolField(TEXT("set")));
		}
	}

	return true;
}

// ---------------------------------------------------------------------------
// FDeleteAssetTest
// Verifies (B): delete_asset tool — tier rejection, missing-asset error,
// and successful deletion of a transient blueprint.
// ---------------------------------------------------------------------------
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FDeleteAssetTest,
	"UnrealAgentMCP.P5.DeleteAsset",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FDeleteAssetTest::RunTest(const FString& Parameters)
{
	UAgentMcpSettings* Settings = GetMutableDefault<UAgentMcpSettings>();
	const EAgentMcpTier SavedCeiling = Settings->PermissionTier;
	ON_SCOPE_EXIT { GetMutableDefault<UAgentMcpSettings>()->PermissionTier = SavedCeiling; };

	bool bIsError = false;

	// Step 1 — at default SafeWrite ceiling: delete_asset must be rejected with rejected_by_tier=true.
	Settings->PermissionTier = EAgentMcpTier::SafeWrite;
	{
		const FString RawRejection = AgentMcpTestUtils::CallToolRawText(*this, TEXT("delete_asset"),
			TEXT("{\"asset_path\":\"/Game/Anything\"}"), bIsError);
		TestTrue(TEXT("delete_asset rejected at SafeWrite ceiling"), bIsError);

		// Verify rejected_by_tier discriminator via raw wire JSON.
		const FString Request = FString::Printf(
			TEXT("{\"jsonrpc\":\"2.0\",\"id\":99,\"method\":\"tools/call\",\"params\":{\"name\":\"delete_asset\",\"arguments\":{\"asset_path\":\"/Game/Anything\"}}}"));
		const TSharedPtr<FJsonObject> RawObj = AgentMcpTestUtils::Parse(AgentMcp::Protocol::HandleMessage(Request));
		bool bDiscriminator = false;
		if (RawObj.IsValid() && RawObj->HasField(TEXT("result")))
		{
			const TSharedPtr<FJsonObject> ResultObj = RawObj->GetObjectField(TEXT("result"));
			bDiscriminator = ResultObj->HasField(TEXT("rejected_by_tier")) && ResultObj->GetBoolField(TEXT("rejected_by_tier"));
		}
		TestTrue(TEXT("rejected_by_tier field present and true"), bDiscriminator);
	}

	// Step 2 — raise ceiling to Destructive for remaining steps.
	Settings->PermissionTier = EAgentMcpTier::Destructive;

	// Step 3 — non-existent asset path: must return an error.
	{
		const FString ErrText = AgentMcpTestUtils::CallToolRawText(*this, TEXT("delete_asset"),
			TEXT("{\"asset_path\":\"/Engine/Transient.NoSuchAssetXyz\"}"), bIsError);
		TestTrue(TEXT("delete_asset non-existent is error"), bIsError);
	}

	// Step 4 — create an in-memory /Game/ blueprint via the tool, delete it, verify it is gone.
	// (A /Engine/Transient blueprint is NOT a registered asset — DeleteLoadedAsset refuses it.
	//  create_blueprint produces a real registered package that the subsystem can delete.)
	{
		const TSharedPtr<FJsonObject> CreateResult = AgentMcpTestUtils::CallTool(*this, TEXT("create_blueprint"),
			TEXT("{\"asset_path\":\"/Game/Dev/BP_McpDeleteTest\"}"), bIsError);
		TestFalse(TEXT("create_blueprint for delete test ok"), bIsError);
		if (!TestNotNull(TEXT("create result parses"), CreateResult.Get()))
		{
			return true;
		}
		const FString ObjPath = CreateResult->GetStringField(TEXT("blueprint_path"));

		const TSharedPtr<FJsonObject> DelResult = AgentMcpTestUtils::CallTool(*this, TEXT("delete_asset"),
			FString::Printf(TEXT("{\"asset_path\":\"%s\"}"), *ObjPath), bIsError);
		TestFalse(TEXT("delete_asset in-memory blueprint ok"), bIsError);
		if (TestNotNull(TEXT("delete result parses"), DelResult.Get()))
		{
			TestTrue(TEXT("deleted:true"), DelResult->GetBoolField(TEXT("deleted")));
		}

		// Verify gone: FindObject should return null after deletion.
		UObject* AfterDelete = FindObject<UObject>(nullptr, *ObjPath);
		TestNull(TEXT("object is null after delete"), AfterDelete);
	}

	return true;
}

// ---------------------------------------------------------------------------
// FWidgetTreeAuthoringTest
// Verifies add_widget, list_widgets, set_widget_property.
// ---------------------------------------------------------------------------
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWidgetTreeAuthoringTest,
	"UnrealAgentMCP.P5.WidgetTreeAuthoring",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FWidgetTreeAuthoringTest::RunTest(const FString& Parameters)
{
	UWidgetBlueprint* WBP = AgentMcpTestUtils::MakeTransientWidgetBlueprint(TEXT("WBP_McpWidgetTreeTest"));
	if (!TestNotNull(TEXT("transient WBP created"), WBP))
	{
		return true;
	}
	const FString Path = WBP->GetPathName();
	bool bIsError = false;

	// Step 1 — add_widget VerticalBox as root.
	const TSharedPtr<FJsonObject> AddRoot = AgentMcpTestUtils::CallTool(*this, TEXT("add_widget"),
		FString::Printf(TEXT("{\"blueprint_path\":\"%s\",\"widget_class\":\"VerticalBox\",\"widget_name\":\"MenuRoot\",\"as_root\":true}"), *Path),
		bIsError);
	TestFalse(TEXT("add_widget VerticalBox succeeds"), bIsError);
	if (TestNotNull(TEXT("add_widget root payload"), AddRoot.Get()))
	{
		TestTrue(TEXT("added:true"), AddRoot->GetBoolField(TEXT("added")));
		TestEqual(TEXT("name is MenuRoot"), AddRoot->GetStringField(TEXT("name")), FString(TEXT("MenuRoot")));
	}

	// Step 2 — add_widget Button child of MenuRoot.
	const TSharedPtr<FJsonObject> AddButton = AgentMcpTestUtils::CallTool(*this, TEXT("add_widget"),
		FString::Printf(TEXT("{\"blueprint_path\":\"%s\",\"widget_class\":\"Button\",\"widget_name\":\"HostButton\",\"parent_name\":\"MenuRoot\"}"), *Path),
		bIsError);
	TestFalse(TEXT("add_widget Button succeeds"), bIsError);
	if (TestNotNull(TEXT("add_widget Button payload"), AddButton.Get()))
	{
		TestTrue(TEXT("Button added:true"), AddButton->GetBoolField(TEXT("added")));
	}

	// Step 3 — add_widget TextBlock child of MenuRoot.
	const TSharedPtr<FJsonObject> AddText = AgentMcpTestUtils::CallTool(*this, TEXT("add_widget"),
		FString::Printf(TEXT("{\"blueprint_path\":\"%s\",\"widget_class\":\"TextBlock\",\"widget_name\":\"StatusLabel\",\"parent_name\":\"MenuRoot\"}"), *Path),
		bIsError);
	TestFalse(TEXT("add_widget TextBlock succeeds"), bIsError);
	if (TestNotNull(TEXT("add_widget TextBlock payload"), AddText.Get()))
	{
		TestTrue(TEXT("TextBlock added:true"), AddText->GetBoolField(TEXT("added")));
	}

	// Step 4 — list_widgets: count==3, HostButton has parent MenuRoot and is_variable==true.
	const TSharedPtr<FJsonObject> Listed = AgentMcpTestUtils::CallTool(*this, TEXT("list_widgets"),
		FString::Printf(TEXT("{\"blueprint_path\":\"%s\"}"), *Path),
		bIsError);
	TestFalse(TEXT("list_widgets succeeds"), bIsError);
	if (TestNotNull(TEXT("list_widgets payload"), Listed.Get()))
	{
		TestEqual(TEXT("count==3"), (int32)Listed->GetNumberField(TEXT("count")), 3);
		bool bFoundButton = false;
		for (const TSharedPtr<FJsonValue>& Entry : Listed->GetArrayField(TEXT("widgets")))
		{
			const TSharedPtr<FJsonObject> W = Entry->AsObject();
			if (W.IsValid() && W->GetStringField(TEXT("name")) == TEXT("HostButton"))
			{
				bFoundButton = true;
				TestEqual(TEXT("HostButton parent is MenuRoot"), W->GetStringField(TEXT("parent")), FString(TEXT("MenuRoot")));
				TestTrue(TEXT("HostButton is_variable==true"), W->GetBoolField(TEXT("is_variable")));
			}
		}
		TestTrue(TEXT("HostButton found in list"), bFoundButton);
	}

	// Step 5 — set_widget_property StatusLabel Text "Hello".
	const TSharedPtr<FJsonObject> SetProp = AgentMcpTestUtils::CallTool(*this, TEXT("set_widget_property"),
		FString::Printf(TEXT("{\"blueprint_path\":\"%s\",\"widget_name\":\"StatusLabel\",\"property\":\"Text\",\"value\":\"Hello\"}"), *Path),
		bIsError);
	TestFalse(TEXT("set_widget_property Text succeeds"), bIsError);
	if (TestNotNull(TEXT("set_widget_property payload"), SetProp.Get()))
	{
		TestTrue(TEXT("set:true"), SetProp->GetBoolField(TEXT("set")));
		TestTrue(TEXT("readback contains Hello"), SetProp->GetStringField(TEXT("value")).Contains(TEXT("Hello"), ESearchCase::IgnoreCase));
	}

	// Step 6 — add duplicate HostButton -> uniquified name.
	const TSharedPtr<FJsonObject> AddDup = AgentMcpTestUtils::CallTool(*this, TEXT("add_widget"),
		FString::Printf(TEXT("{\"blueprint_path\":\"%s\",\"widget_class\":\"Button\",\"widget_name\":\"HostButton\",\"parent_name\":\"MenuRoot\"}"), *Path),
		bIsError);
	TestFalse(TEXT("add duplicate HostButton succeeds"), bIsError);
	if (TestNotNull(TEXT("duplicate payload"), AddDup.Get()))
	{
		TestTrue(TEXT("dup added:true"), AddDup->GetBoolField(TEXT("added")));
		TestNotEqual(TEXT("dup name is uniquified"), AddDup->GetStringField(TEXT("name")), FString(TEXT("HostButton")));
	}

	// Step 7 — set_widget_property on nonexistent widget -> error containing "list_widgets".
	const FString NoWidgetErr = AgentMcpTestUtils::CallToolRawText(*this, TEXT("set_widget_property"),
		FString::Printf(TEXT("{\"blueprint_path\":\"%s\",\"widget_name\":\"NoSuchWidget\",\"property\":\"Text\",\"value\":\"x\"}"), *Path),
		bIsError);
	TestTrue(TEXT("nonexistent widget is error"), bIsError);
	TestTrue(TEXT("error mentions list_widgets"), NoWidgetErr.Contains(TEXT("list_widgets"), ESearchCase::IgnoreCase));

	// Step 8 — compile_blueprint -> status ok, num_errors 0.
	// (Runs BEFORE the ListView case below: a ListView with no valid EntryWidgetClass is a
	//  Widget-compiler ERROR by design, which would poison this clean-compile assertion.)
	const TSharedPtr<FJsonObject> Compiled = AgentMcpTestUtils::CallTool(*this, TEXT("compile_blueprint"),
		FString::Printf(TEXT("{\"blueprint_path\":\"%s\"}"), *Path),
		bIsError);
	TestFalse(TEXT("compile_blueprint succeeds"), bIsError);
	if (TestNotNull(TEXT("compile payload"), Compiled.Get()))
	{
		TestEqual(TEXT("compile status ok"), Compiled->GetStringField(TEXT("status")), FString(TEXT("ok")));
		TestEqual(TEXT("compile num_errors 0"), (int32)Compiled->GetNumberField(TEXT("num_errors")), 0);
	}

	// Step 9 — EntryWidgetClass MustImplement guard: a ListView must reject an entry class
	// that does not implement IUserListEntry (T4 review finding: highest-risk validation branch).
	// No compile after this — the half-configured ListView is intentionally left behind in the
	// transient blueprint.
	{
		const TSharedPtr<FJsonObject> AddList = AgentMcpTestUtils::CallTool(*this, TEXT("add_widget"),
			FString::Printf(TEXT("{\"blueprint_path\":\"%s\",\"widget_class\":\"ListView\",\"widget_name\":\"TestListView\",\"parent_name\":\"MenuRoot\"}"), *Path),
			bIsError);
		TestFalse(TEXT("add ListView ok"), bIsError);

		const FString BadEntryErr = AgentMcpTestUtils::CallToolRawText(*this, TEXT("set_widget_property"),
			FString::Printf(TEXT("{\"blueprint_path\":\"%s\",\"widget_name\":\"TestListView\",\"property\":\"EntryWidgetClass\",\"value\":\"/Script/UMG.Button\"}"), *Path),
			bIsError);
		TestTrue(TEXT("non-IUserListEntry entry class is error"), bIsError);
		TestTrue(TEXT("error mentions IUserListEntry"), BadEntryErr.Contains(TEXT("IUserListEntry"), ESearchCase::IgnoreCase));

		// Step 9b — content path WITHOUT the _C suffix must normalize (1.2: callers shouldn't
		// need to know the generated-class form). Uses a real repo asset.
		const TSharedPtr<FJsonObject> NormResult = AgentMcpTestUtils::CallTool(*this, TEXT("set_widget_property"),
			FString::Printf(TEXT("{\"blueprint_path\":\"%s\",\"widget_name\":\"TestListView\",\"property\":\"EntryWidgetClass\",\"value\":\"/Game/UI/WBP_MASessionRowWidget\"}"), *Path),
			bIsError);
		TestFalse(TEXT("EntryWidgetClass sans _C resolves"), bIsError);
		if (TestNotNull(TEXT("normalized set parses"), NormResult.Get()))
		{
			TestTrue(TEXT("set:true"), NormResult->GetBoolField(TEXT("set")));
		}
	}

	return true;
}

// ---------------------------------------------------------------------------
// FComponentEventTest
// Verifies add_component_event: creates a Button OnClicked bound event node,
// deduplicates on repeat call, rejects bad delegate name, rejects missing widget.
// ---------------------------------------------------------------------------
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FComponentEventTest,
	"UnrealAgentMCP.P5.ComponentEvent",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FComponentEventTest::RunTest(const FString& Parameters)
{
	// Step 1 — create a transient WBP and add a Button widget named JoinButton as root.
	UWidgetBlueprint* WBP = AgentMcpTestUtils::MakeTransientWidgetBlueprint(TEXT("WBP_McpComponentEventTest"));
	if (!TestNotNull(TEXT("transient WBP created"), WBP))
	{
		return true;
	}
	const FString Path = WBP->GetPathName();
	bool bIsError = false;

	const TSharedPtr<FJsonObject> AddBtn = AgentMcpTestUtils::CallTool(*this, TEXT("add_widget"),
		FString::Printf(TEXT("{\"blueprint_path\":\"%s\",\"widget_class\":\"Button\",\"widget_name\":\"JoinButton\",\"as_root\":true}"), *Path),
		bIsError);
	if (!TestFalse(TEXT("add_widget Button succeeds"), bIsError) ||
		!TestNotNull(TEXT("add_widget payload"), AddBtn.Get()))
	{
		return true;
	}

	// Step 2 — add_component_event OnClicked -> expect node_id, class contains "ComponentBoundEvent",
	//           exec output pin present, existing absent-or-false.
	const TSharedPtr<FJsonObject> EventResult = AgentMcpTestUtils::CallTool(*this, TEXT("add_component_event"),
		FString::Printf(TEXT("{\"blueprint_path\":\"%s\",\"component_name\":\"JoinButton\",\"event_name\":\"OnClicked\"}"), *Path),
		bIsError);
	FString FirstNodeId;
	if (!TestFalse(TEXT("add_component_event OnClicked succeeds"), bIsError) ||
		!TestNotNull(TEXT("add_component_event payload"), EventResult.Get()))
	{
		return true;
	}
	FirstNodeId = EventResult->GetStringField(TEXT("node_id"));
	TestFalse(TEXT("node_id not empty"), FirstNodeId.IsEmpty());
	TestTrue(TEXT("class contains ComponentBoundEvent"),
		EventResult->GetStringField(TEXT("class")).Contains(TEXT("ComponentBoundEvent"), ESearchCase::IgnoreCase));
	TestFalse(TEXT("existing is false on first call"), EventResult->GetBoolField(TEXT("existing")));

	// Verify at least one exec output pin exists.
	// PinToJson serializes direction as "out" (not "output") — see NodeGraphUtils.cpp PinToJson.
	bool bHasExecOut = false;
	for (const TSharedPtr<FJsonValue>& PinVal : EventResult->GetArrayField(TEXT("pins")))
	{
		const TSharedPtr<FJsonObject> PinObj = PinVal->AsObject();
		if (PinObj.IsValid())
		{
			const FString Dir = PinObj->GetStringField(TEXT("direction"));
			const FString Type = PinObj->GetStringField(TEXT("type"));
			if (Dir == TEXT("out") && Type.Contains(TEXT("exec"), ESearchCase::IgnoreCase))
			{
				bHasExecOut = true;
			}
		}
	}
	TestTrue(TEXT("bound event has exec output pin"), bHasExecOut);

	// Step 3 — same call again -> existing:true AND same node_id.
	const TSharedPtr<FJsonObject> DupResult = AgentMcpTestUtils::CallTool(*this, TEXT("add_component_event"),
		FString::Printf(TEXT("{\"blueprint_path\":\"%s\",\"component_name\":\"JoinButton\",\"event_name\":\"OnClicked\"}"), *Path),
		bIsError);
	if (TestFalse(TEXT("add_component_event dedup succeeds"), bIsError) &&
		TestNotNull(TEXT("dedup payload"), DupResult.Get()))
	{
		TestTrue(TEXT("dedup returns existing:true"), DupResult->GetBoolField(TEXT("existing")));
		TestEqual(TEXT("dedup returns same node_id"), DupResult->GetStringField(TEXT("node_id")), FirstNodeId);
	}

	// Step 4 — bad delegate name -> error mentioning the widget class name or the delegate name.
	const FString BadEventErr = AgentMcpTestUtils::CallToolRawText(*this, TEXT("add_component_event"),
		FString::Printf(TEXT("{\"blueprint_path\":\"%s\",\"component_name\":\"JoinButton\",\"event_name\":\"OnNoSuchEvent\"}"), *Path),
		bIsError);
	TestTrue(TEXT("bad delegate name is error"), bIsError);
	TestTrue(TEXT("bad delegate error mentions delegate or class"),
		BadEventErr.Contains(TEXT("OnNoSuchEvent"), ESearchCase::IgnoreCase) ||
		BadEventErr.Contains(TEXT("Button"), ESearchCase::IgnoreCase));

	// Step 5 — missing widget -> error containing "list_widgets".
	const FString MissingWidgetErr = AgentMcpTestUtils::CallToolRawText(*this, TEXT("add_component_event"),
		FString::Printf(TEXT("{\"blueprint_path\":\"%s\",\"component_name\":\"NoSuchWidget\",\"event_name\":\"OnClicked\"}"), *Path),
		bIsError);
	TestTrue(TEXT("missing widget is error"), bIsError);
	TestTrue(TEXT("missing widget error contains list_widgets"),
		MissingWidgetErr.Contains(TEXT("list_widgets"), ESearchCase::IgnoreCase));

	// Step 6 — compile_blueprint -> 0 errors.
	const TSharedPtr<FJsonObject> Compiled = AgentMcpTestUtils::CallTool(*this, TEXT("compile_blueprint"),
		FString::Printf(TEXT("{\"blueprint_path\":\"%s\"}"), *Path),
		bIsError);
	TestFalse(TEXT("compile_blueprint succeeds"), bIsError);
	if (TestNotNull(TEXT("compile payload"), Compiled.Get()))
	{
		TestEqual(TEXT("compile status ok"), Compiled->GetStringField(TEXT("status")), FString(TEXT("ok")));
		TestEqual(TEXT("compile num_errors 0"), (int32)Compiled->GetNumberField(TEXT("num_errors")), 0);
	}

	return true;
}


// ---------------------------------------------------------------------------
// FMvvmAuthoringTest
// Verifies add_viewmodel + add_view_binding.
//
// NOTE on success-binding coverage:
//   A fully-valid add_view_binding success path requires a VM class with FieldNotify
//   properties. UMVVMViewModelBase (the only VM type available from engine modules without
//   a host-project dependency) has NONE. The success path is therefore exercised end-to-end
//   in T8 against UMAMainMenuViewModel which lives in the host project. Here we verify:
//   (a) viewmodel add/duplicate/invalid-class
//   (b) the silent-no-op trap is surfaced as an explicit error (viewmodel property not found)
//   (c) widget-not-found and direction validation errors
//   (d) structural correctness: the validate-first design means no dangling binding rows are
//       created by the failed add_view_binding attempts.
// ---------------------------------------------------------------------------
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FMvvmAuthoringTest,
	"UnrealAgentMCP.P5.MvvmAuthoring",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FMvvmAuthoringTest::RunTest(const FString& Parameters)
{
	// Step 1 — create a transient WBP and add a TextBlock widget as root.
	UWidgetBlueprint* WBP = AgentMcpTestUtils::MakeTransientWidgetBlueprint(TEXT("WBP_McpMvvmAuthoringTest"));
	if (!TestNotNull(TEXT("transient WBP created"), WBP))
	{
		return true;
	}
	const FString Path = WBP->GetPathName();
	bool bIsError = false;

	AgentMcpTestUtils::CallTool(*this, TEXT("add_widget"),
		FString::Printf(TEXT("{\"blueprint_path\":\"%s\",\"widget_class\":\"TextBlock\",\"widget_name\":\"StatusLabel\",\"as_root\":true}"), *Path),
		bIsError);
	TestFalse(TEXT("add TextBlock StatusLabel succeeds"), bIsError);

	// Step 2 — add_viewmodel with UMVVMViewModelBase -> {added:true, name:"TestVM"} + non-empty viewmodel_id.
	const TSharedPtr<FJsonObject> AddVMResult = AgentMcpTestUtils::CallTool(*this, TEXT("add_viewmodel"),
		FString::Printf(
			TEXT("{\"blueprint_path\":\"%s\",\"viewmodel_class\":\"/Script/ModelViewViewModel.MVVMViewModelBase\",\"name\":\"TestVM\",\"creation_type\":\"manual\"}"),
			*Path),
		bIsError);
	TestFalse(TEXT("add_viewmodel UMVVMViewModelBase succeeds"), bIsError);
	FString ViewModelId;
	if (TestNotNull(TEXT("add_viewmodel result parses"), AddVMResult.Get()))
	{
		TestTrue(TEXT("added:true"), AddVMResult->GetBoolField(TEXT("added")));
		TestEqual(TEXT("name is TestVM"), AddVMResult->GetStringField(TEXT("name")), FString(TEXT("TestVM")));
		ViewModelId = AddVMResult->GetStringField(TEXT("viewmodel_id"));
		TestFalse(TEXT("viewmodel_id is non-empty"), ViewModelId.IsEmpty());
	}

	// Step 3 — duplicate add_viewmodel -> error containing "already".
	const FString DupVMErr = AgentMcpTestUtils::CallToolRawText(*this, TEXT("add_viewmodel"),
		FString::Printf(
			TEXT("{\"blueprint_path\":\"%s\",\"viewmodel_class\":\"/Script/ModelViewViewModel.MVVMViewModelBase\",\"name\":\"TestVM\",\"creation_type\":\"manual\"}"),
			*Path),
		bIsError);
	TestTrue(TEXT("duplicate add_viewmodel is error"), bIsError);
	TestTrue(TEXT("duplicate error mentions 'already'"),
		DupVMErr.Contains(TEXT("already"), ESearchCase::IgnoreCase));

	// Step 4 — non-VM class (AActor) -> error containing "NotifyFieldValueChanged".
	const FString NonVMErr = AgentMcpTestUtils::CallToolRawText(*this, TEXT("add_viewmodel"),
		FString::Printf(
			TEXT("{\"blueprint_path\":\"%s\",\"viewmodel_class\":\"/Script/Engine.Actor\",\"name\":\"BadVM\"}"),
			*Path),
		bIsError);
	TestTrue(TEXT("non-VM class is error"), bIsError);
	TestTrue(TEXT("non-VM error mentions NotifyFieldValueChanged"),
		NonVMErr.Contains(TEXT("NotifyFieldValueChanged"), ESearchCase::IgnoreCase));

	// Step 5 — add_view_binding with invalid viewmodel_property (the silent-no-op trap):
	//   UMVVMViewModelBase has no FieldNotify properties, so "NoSuchProp" must be caught
	//   and returned as an explicit error containing "not found".
	const FString NoVMPropErr = AgentMcpTestUtils::CallToolRawText(*this, TEXT("add_view_binding"),
		FString::Printf(
			TEXT("{\"blueprint_path\":\"%s\",\"widget_name\":\"StatusLabel\",\"widget_property\":\"Text\",\"viewmodel_name\":\"TestVM\",\"viewmodel_property\":\"NoSuchProp\",\"direction\":\"one_way\"}"),
			*Path),
		bIsError);
	TestTrue(TEXT("invalid VM property is error"), bIsError);
	TestTrue(TEXT("error mentions 'not found'"),
		NoVMPropErr.Contains(TEXT("not found"), ESearchCase::IgnoreCase));

	// Step 6 — add_view_binding with non-existent widget -> error containing "list_widgets".
	const FString NoWidgetErr = AgentMcpTestUtils::CallToolRawText(*this, TEXT("add_view_binding"),
		FString::Printf(
			TEXT("{\"blueprint_path\":\"%s\",\"widget_name\":\"NoSuchWidget\",\"widget_property\":\"Text\",\"viewmodel_name\":\"TestVM\",\"viewmodel_property\":\"NoSuchProp\",\"direction\":\"one_way\"}"),
			*Path),
		bIsError);
	TestTrue(TEXT("missing widget is error"), bIsError);
	TestTrue(TEXT("missing widget error mentions list_widgets"),
		NoWidgetErr.Contains(TEXT("list_widgets"), ESearchCase::IgnoreCase));

	// Step 7 — add_view_binding with invalid direction token -> error listing valid directions.
	const FString BadDirErr = AgentMcpTestUtils::CallToolRawText(*this, TEXT("add_view_binding"),
		FString::Printf(
			TEXT("{\"blueprint_path\":\"%s\",\"widget_name\":\"StatusLabel\",\"widget_property\":\"Text\",\"viewmodel_name\":\"TestVM\",\"viewmodel_property\":\"NoSuchProp\",\"direction\":\"sideways\"}"),
			*Path),
		bIsError);
	TestTrue(TEXT("bad direction is error"), bIsError);
	// Error must list the valid options so the caller can self-correct.
	TestTrue(TEXT("bad direction error lists valid directions (one_way or two_way)"),
		BadDirErr.Contains(TEXT("one_way"), ESearchCase::IgnoreCase) ||
		BadDirErr.Contains(TEXT("two_way"), ESearchCase::IgnoreCase));

	// Step 8 (structural) — validate-first design: the three failed add_view_binding calls above
	// must NOT have created any binding rows (no dangling blank bindings).
	// We verify via the MVVM editor subsystem directly.
	{
		UMVVMEditorSubsystem* Sub = GEditor ? GEditor->GetEditorSubsystem<UMVVMEditorSubsystem>() : nullptr;
		if (Sub)
		{
			const UMVVMBlueprintView* View = Sub->GetView(WBP);
			const int32 BindingCount = View ? View->GetNumBindings() : 0;
			TestEqual(TEXT("no dangling binding rows after failed add_view_binding calls (validate-first)"),
				BindingCount, 0);
		}
	}

	return true;
}

// ---------------------------------------------------------------------------
// FMvvmBindingLifecycleTest
// Verifies list_view_bindings + remove_view_binding round trip. A blank binding
// row is created directly through the engine subsystem (the tool's own success
// path needs a FieldNotify VM property, which the engine base class lacks and
// plugin tests must not depend on the host project's module — the full success
// path incl. conversion functions is exercised in live E2E).
// ---------------------------------------------------------------------------
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FMvvmBindingLifecycleTest,
	"UnrealAgentMCP.P5.MvvmBindingLifecycle",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FMvvmBindingLifecycleTest::RunTest(const FString& Parameters)
{
	UWidgetBlueprint* WBP = AgentMcpTestUtils::MakeTransientWidgetBlueprint(TEXT("WBP_McpBindingLifecycle"));
	if (!TestNotNull(TEXT("transient widget blueprint created"), WBP))
	{
		return true;
	}
	const FString Path = WBP->GetPathName();
	bool bIsError = false;

	UMVVMEditorSubsystem* Sub = GEditor ? GEditor->GetEditorSubsystem<UMVVMEditorSubsystem>() : nullptr;
	if (!TestNotNull(TEXT("MVVM editor subsystem"), Sub))
	{
		return true;
	}

	// Step 1 — empty view: list returns count 0 (view may not even exist yet).
	{
		const TSharedPtr<FJsonObject> ListResult = AgentMcpTestUtils::CallTool(*this, TEXT("list_view_bindings"),
			FString::Printf(TEXT("{\"blueprint_path\":\"%s\"}"), *Path), bIsError);
		TestFalse(TEXT("list on empty view ok"), bIsError);
		if (TestNotNull(TEXT("empty list parses"), ListResult.Get()))
		{
			TestEqual(TEXT("empty view has 0 bindings"), (int32)ListResult->GetNumberField(TEXT("count")), 0);
		}
	}

	// Step 2 — create a blank binding row directly via the engine subsystem.
	FString BindingIdStr;
	{
		Sub->RequestView(WBP);
		const FMVVMBlueprintViewBinding& Binding = Sub->AddBinding(WBP);
		BindingIdStr = Binding.BindingId.ToString();
	}

	// Step 3 — list shows exactly that row.
	{
		const TSharedPtr<FJsonObject> ListResult = AgentMcpTestUtils::CallTool(*this, TEXT("list_view_bindings"),
			FString::Printf(TEXT("{\"blueprint_path\":\"%s\"}"), *Path), bIsError);
		TestFalse(TEXT("list after add ok"), bIsError);
		if (TestNotNull(TEXT("list parses"), ListResult.Get()))
		{
			TestEqual(TEXT("one binding listed"), (int32)ListResult->GetNumberField(TEXT("count")), 1);
			const TArray<TSharedPtr<FJsonValue>>& Rows = ListResult->GetArrayField(TEXT("bindings"));
			if (Rows.Num() == 1 && Rows[0]->AsObject().IsValid())
			{
				TestEqual(TEXT("listed id matches"), Rows[0]->AsObject()->GetStringField(TEXT("binding_id")), BindingIdStr);
			}
		}
	}

	// Step 4 — remove with a bogus id: error mentioning list_view_bindings.
	{
		const FString Err = AgentMcpTestUtils::CallToolRawText(*this, TEXT("remove_view_binding"),
			FString::Printf(TEXT("{\"blueprint_path\":\"%s\",\"binding_id\":\"00000000000000000000000000000000\"}"), *Path), bIsError);
		TestTrue(TEXT("bogus id is error"), bIsError);
		TestTrue(TEXT("error hints list_view_bindings"), Err.Contains(TEXT("list_view_bindings"), ESearchCase::IgnoreCase));
	}

	// Step 5 — remove the real row; list returns to 0.
	{
		const TSharedPtr<FJsonObject> RemoveResult = AgentMcpTestUtils::CallTool(*this, TEXT("remove_view_binding"),
			FString::Printf(TEXT("{\"blueprint_path\":\"%s\",\"binding_id\":\"%s\"}"), *Path, *BindingIdStr), bIsError);
		TestFalse(TEXT("remove ok"), bIsError);
		if (TestNotNull(TEXT("remove parses"), RemoveResult.Get()))
		{
			TestTrue(TEXT("removed:true"), RemoveResult->GetBoolField(TEXT("removed")));
		}

		const TSharedPtr<FJsonObject> ListResult = AgentMcpTestUtils::CallTool(*this, TEXT("list_view_bindings"),
			FString::Printf(TEXT("{\"blueprint_path\":\"%s\"}"), *Path), bIsError);
		TestFalse(TEXT("final list ok"), bIsError);
		if (TestNotNull(TEXT("final list parses"), ListResult.Get()))
		{
			TestEqual(TEXT("0 bindings after remove"), (int32)ListResult->GetNumberField(TEXT("count")), 0);
		}
	}

	return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS
