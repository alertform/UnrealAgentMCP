#include "Misc/AutomationTest.h"

#if WITH_DEV_AUTOMATION_TESTS

#include "AgentMcpSettings.h"
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

#endif // WITH_DEV_AUTOMATION_TESTS
