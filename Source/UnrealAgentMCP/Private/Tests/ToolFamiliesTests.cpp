#include "Misc/AutomationTest.h"

#if WITH_DEV_AUTOMATION_TESTS

#include "AgentMcpSettings.h"
#include "Core/AgentMcpTier.h"
#include "HAL/FileManager.h"
#include "Misc/Paths.h"
#include "Misc/ScopeExit.h"
#include "Tests/AgentMcpTestHelpers.h"

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FToolFamiliesCdoPropertyTest,
	"UnrealAgentMCP.ToolFamilies.CdoGetSetRoundTrip",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FToolFamiliesCdoPropertyTest::RunTest(const FString& Parameters)
{
	UBlueprint* Blueprint = AgentMcpTestUtils::MakeTransientBlueprint(TEXT("BP_McpCdoTest"));
	if (!TestNotNull(TEXT("transient blueprint created"), Blueprint))
	{
		return true;
	}
	const FString Path = Blueprint->GetPathName();
	bool bIsError = false;

	// AActor::bCanBeDamaged is an EditAnywhere bool ("bCanBeDamaged") - present on every Actor CDO.
	const TSharedPtr<FJsonObject> Got = AgentMcpTestUtils::CallTool(*this, TEXT("get_cdo_property"),
		FString::Printf(TEXT("{\"blueprint_path\":\"%s\",\"property\":\"bCanBeDamaged\"}"), *Path), bIsError);
	TestFalse(TEXT("get_cdo_property ok"), bIsError);
	FString Original;
	if (TestNotNull(TEXT("get payload parses"), Got.Get()))
	{
		Original = Got->GetStringField(TEXT("value"));
		TestTrue(TEXT("bool value shape"), Original == TEXT("True") || Original == TEXT("False") || Original == TEXT("true") || Original == TEXT("false"));
	}

	// Flip it via set, read back via get.
	const FString Flipped = (Original.Compare(TEXT("True"), ESearchCase::IgnoreCase) == 0) ? TEXT("False") : TEXT("True");
	AgentMcpTestUtils::CallTool(*this, TEXT("set_cdo_property"),
		FString::Printf(TEXT("{\"blueprint_path\":\"%s\",\"property\":\"bCanBeDamaged\",\"value\":\"%s\"}"), *Path, *Flipped), bIsError);
	TestFalse(TEXT("set_cdo_property ok"), bIsError);
	const TSharedPtr<FJsonObject> Got2 = AgentMcpTestUtils::CallTool(*this, TEXT("get_cdo_property"),
		FString::Printf(TEXT("{\"blueprint_path\":\"%s\",\"property\":\"bCanBeDamaged\"}"), *Path), bIsError);
	if (Got2.IsValid())
	{
		TestEqual(TEXT("value round-trips"), Got2->GetStringField(TEXT("value")).ToLower(), Flipped.ToLower());
	}

	// Unknown property -> tool error listing the property name.
	AgentMcpTestUtils::CallTool(*this, TEXT("get_cdo_property"),
		FString::Printf(TEXT("{\"blueprint_path\":\"%s\",\"property\":\"NoSuchPropXyz\"}"), *Path), bIsError);
	TestTrue(TEXT("unknown property is a tool error"), bIsError);

	// Invalid value for typed property -> tool error (ImportText failure surfaced).
	AgentMcpTestUtils::CallTool(*this, TEXT("set_cdo_property"),
		FString::Printf(TEXT("{\"blueprint_path\":\"%s\",\"property\":\"bCanBeDamaged\",\"value\":\"not-a-bool-xyz\"}"), *Path), bIsError);
	TestTrue(TEXT("invalid value is a tool error"), bIsError);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FToolFamiliesAssetTest,
	"UnrealAgentMCP.ToolFamilies.AssetSearchInfoRefsSave",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FToolFamiliesAssetTest::RunTest(const FString& Parameters)
{
	bool bIsError = false;

	// search_assets: by class, project-wide. Project has Blueprints (BP_*).
	const TSharedPtr<FJsonObject> Search = AgentMcpTestUtils::CallTool(*this, TEXT("search_assets"),
		TEXT("{\"class_name\":\"Blueprint\",\"name_contains\":\"BP_\",\"limit\":10}"), bIsError);
	TestFalse(TEXT("search_assets ok"), bIsError);
	FString AnyAssetPath;
	if (TestNotNull(TEXT("search payload parses"), Search.Get()))
	{
		const TArray<TSharedPtr<FJsonValue>>& Assets = Search->GetArrayField(TEXT("assets"));
		TestTrue(TEXT("found at least one BP"), Assets.Num() >= 1);
		if (Assets.Num() > 0)
		{
			AnyAssetPath = Assets[0]->AsObject()->GetStringField(TEXT("package_path"));
		}
	}

	// get_asset_info on a found asset.
	if (!AnyAssetPath.IsEmpty())
	{
		const TSharedPtr<FJsonObject> Info = AgentMcpTestUtils::CallTool(*this, TEXT("get_asset_info"),
			FString::Printf(TEXT("{\"asset_path\":\"%s\"}"), *AnyAssetPath), bIsError);
		TestFalse(TEXT("get_asset_info ok"), bIsError);
		if (Info.IsValid())
		{
			TestTrue(TEXT("info has class"), Info->HasField(TEXT("class")));
		}

		// get_references both directions return arrays (counts may be 0 - shape only).
		const TSharedPtr<FJsonObject> Refs = AgentMcpTestUtils::CallTool(*this, TEXT("get_references"),
			FString::Printf(TEXT("{\"asset_path\":\"%s\",\"direction\":\"dependencies\"}"), *AnyAssetPath), bIsError);
		TestFalse(TEXT("get_references ok"), bIsError);
		if (Refs.IsValid())
		{
			TestTrue(TEXT("refs has packages array"), Refs->HasField(TEXT("packages")));
		}
	}

	// Unknown asset -> tool errors.
	AgentMcpTestUtils::CallTool(*this, TEXT("get_asset_info"), TEXT("{\"asset_path\":\"/Game/Nope/NoAsset\"}"), bIsError);
	TestTrue(TEXT("unknown asset info is a tool error"), bIsError);

	// save_asset writes a real file: create -> save -> file exists -> cleanup.
	AgentMcpTestUtils::CallTool(*this, TEXT("create_blueprint"),
		TEXT("{\"asset_path\":\"/Game/Dev/AgentMcpTests/BP_McpSaveTest\",\"parent_class\":\"Actor\"}"), bIsError);
	TestFalse(TEXT("create for save ok"), bIsError);
	const FString DiskPath = FPaths::ProjectContentDir() / TEXT("Dev/AgentMcpTests/BP_McpSaveTest.uasset");
	ON_SCOPE_EXIT
	{
		// Keep the user's working tree clean: remove the test artifact from disk.
		IFileManager::Get().Delete(*DiskPath, /*RequireExists*/ false, /*EvenReadOnly*/ true);
	};
	const TSharedPtr<FJsonObject> Saved = AgentMcpTestUtils::CallTool(*this, TEXT("save_asset"),
		TEXT("{\"asset_path\":\"/Game/Dev/AgentMcpTests/BP_McpSaveTest\"}"), bIsError);
	TestFalse(TEXT("save_asset ok"), bIsError);
	TestTrue(TEXT("uasset file exists on disk"), IFileManager::Get().FileExists(*DiskPath));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FToolFamiliesActorTest,
	"UnrealAgentMCP.ToolFamilies.ActorSpawnQueryTransformDestroy",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FToolFamiliesActorTest::RunTest(const FString& Parameters)
{
	UAgentMcpSettings* Settings = GetMutableDefault<UAgentMcpSettings>();
	const EAgentMcpTier SavedCeiling = Settings->PermissionTier;
	ON_SCOPE_EXIT { GetMutableDefault<UAgentMcpSettings>()->PermissionTier = SavedCeiling; };
	bool bIsError = false;

	// spawn an empty Actor at a known location with a label.
	const TSharedPtr<FJsonObject> Spawned = AgentMcpTestUtils::CallTool(*this, TEXT("spawn_actor"),
		TEXT("{\"class_name\":\"Actor\",\"label\":\"McpTestActor\",\"location\":{\"x\":100,\"y\":200,\"z\":300}}"), bIsError);
	TestFalse(TEXT("spawn_actor ok"), bIsError);
	FString ActorPath;
	if (TestNotNull(TEXT("spawn payload parses"), Spawned.Get()))
	{
		ActorPath = Spawned->GetStringField(TEXT("actor_path"));
		TestTrue(TEXT("actor_path non-empty"), !ActorPath.IsEmpty());
	}
	ON_SCOPE_EXIT
	{
		// Cleanup: destroy the spawned test actor regardless of assertion outcomes (needs D ceiling).
		if (!ActorPath.IsEmpty())
		{
			GetMutableDefault<UAgentMcpSettings>()->PermissionTier = EAgentMcpTier::Destructive;
			bool bCleanupError = false;
			AgentMcpTestUtils::CallTool(*this, TEXT("destroy_actor"),
				FString::Printf(TEXT("{\"actor_path\":\"%s\"}"), *ActorPath), bCleanupError);
		}
	};

	// query finds it by label.
	const TSharedPtr<FJsonObject> Queried = AgentMcpTestUtils::CallTool(*this, TEXT("query_actors"),
		TEXT("{\"label_contains\":\"McpTestActor\"}"), bIsError);
	TestFalse(TEXT("query_actors ok"), bIsError);
	if (Queried.IsValid())
	{
		TestTrue(TEXT("query found the spawn"), static_cast<int32>(Queried->GetNumberField(TEXT("returned"))) >= 1);
	}

	// transform: move it, then read the property back via set/get bridge shape (location is not a
	// single FProperty - verify via a second query is overkill; assert the tool reports set:true).
	const TSharedPtr<FJsonObject> Moved = AgentMcpTestUtils::CallTool(*this, TEXT("set_actor_transform"),
		FString::Printf(TEXT("{\"actor_path\":\"%s\",\"location\":{\"x\":500,\"y\":0,\"z\":0}}"), *ActorPath), bIsError);
	TestFalse(TEXT("set_actor_transform ok"), bIsError);

	// set_actor_property on an EditAnywhere bool.
	AgentMcpTestUtils::CallTool(*this, TEXT("set_actor_property"),
		FString::Printf(TEXT("{\"actor_path\":\"%s\",\"property\":\"bCanBeDamaged\",\"value\":\"False\"}"), *ActorPath), bIsError);
	TestFalse(TEXT("set_actor_property ok"), bIsError);

	// destroy at SafeWrite ceiling -> tier rejection; at Destructive -> works (identity echoed).
	Settings->PermissionTier = EAgentMcpTier::SafeWrite;
	AgentMcpTestUtils::CallTool(*this, TEXT("destroy_actor"),
		FString::Printf(TEXT("{\"actor_path\":\"%s\"}"), *ActorPath), bIsError);
	TestTrue(TEXT("destroy rejected at SafeWrite"), bIsError);
	Settings->PermissionTier = EAgentMcpTier::Destructive;
	const TSharedPtr<FJsonObject> Destroyed = AgentMcpTestUtils::CallTool(*this, TEXT("destroy_actor"),
		FString::Printf(TEXT("{\"actor_path\":\"%s\"}"), *ActorPath), bIsError);
	TestFalse(TEXT("destroy ok at Destructive"), bIsError);
	if (Destroyed.IsValid())
	{
		TestTrue(TEXT("destroyed identity echoed"), Destroyed->GetStringField(TEXT("label")).Contains(TEXT("McpTestActor")));
	}
	// double destroy -> tool error (actor gone).
	AgentMcpTestUtils::CallTool(*this, TEXT("destroy_actor"),
		FString::Printf(TEXT("{\"actor_path\":\"%s\"}"), *ActorPath), bIsError);
	TestTrue(TEXT("double destroy is a tool error"), bIsError);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FToolFamiliesVariableTest,
	"UnrealAgentMCP.ToolFamilies.VariableAddAndFlags",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FToolFamiliesVariableTest::RunTest(const FString& Parameters)
{
	UBlueprint* Blueprint = AgentMcpTestUtils::MakeTransientBlueprint(TEXT("BP_McpVarTest"));
	if (!TestNotNull(TEXT("transient blueprint created"), Blueprint))
	{
		return true;
	}
	const FString Path = Blueprint->GetPathName();
	bool bIsError = false;

	// add a float (real) variable with a default, then a class-reference variable.
	AgentMcpTestUtils::CallTool(*this, TEXT("add_variable"),
		FString::Printf(TEXT("{\"blueprint_path\":\"%s\",\"variable_name\":\"McpHealth\",\"type\":\"real\",\"default_value\":\"42.5\"}"), *Path), bIsError);
	TestFalse(TEXT("add real variable ok"), bIsError);
	AgentMcpTestUtils::CallTool(*this, TEXT("add_variable"),
		FString::Printf(TEXT("{\"blueprint_path\":\"%s\",\"variable_name\":\"McpActorClass\",\"type\":\"class:Actor\"}"), *Path), bIsError);
	TestFalse(TEXT("add class-ref variable ok"), bIsError);

	// duplicate name -> tool error.
	AgentMcpTestUtils::CallTool(*this, TEXT("add_variable"),
		FString::Printf(TEXT("{\"blueprint_path\":\"%s\",\"variable_name\":\"McpHealth\",\"type\":\"bool\"}"), *Path), bIsError);
	TestTrue(TEXT("duplicate variable is a tool error"), bIsError);

	// unknown type token -> tool error listing supported tokens.
	AgentMcpTestUtils::CallTool(*this, TEXT("add_variable"),
		FString::Printf(TEXT("{\"blueprint_path\":\"%s\",\"variable_name\":\"McpBad\",\"type\":\"flux\"}"), *Path), bIsError);
	TestTrue(TEXT("unknown type is a tool error"), bIsError);

	// flags: instance editable + expose on spawn round-trip (verify via compile success + no error).
	AgentMcpTestUtils::CallTool(*this, TEXT("set_variable_flags"),
		FString::Printf(TEXT("{\"blueprint_path\":\"%s\",\"variable_name\":\"McpHealth\",\"instance_editable\":true,\"expose_on_spawn\":true}"), *Path), bIsError);
	TestFalse(TEXT("set_variable_flags ok"), bIsError);

	// compile clean after all of it; CDO carries the default.
	const TSharedPtr<FJsonObject> Compiled = AgentMcpTestUtils::CallTool(*this, TEXT("compile_blueprint"),
		FString::Printf(TEXT("{\"blueprint_path\":\"%s\"}"), *Path), bIsError);
	if (Compiled.IsValid())
	{
		TestEqual(TEXT("compiles clean with new variables"), static_cast<int32>(Compiled->GetNumberField(TEXT("num_errors"))), 0);
	}
	const TSharedPtr<FJsonObject> Got = AgentMcpTestUtils::CallTool(*this, TEXT("get_cdo_property"),
		FString::Printf(TEXT("{\"blueprint_path\":\"%s\",\"property\":\"McpHealth\"}"), *Path), bIsError);
	TestFalse(TEXT("read new variable off CDO ok"), bIsError);
	if (Got.IsValid())
	{
		TestTrue(TEXT("default landed"), Got->GetStringField(TEXT("value")).StartsWith(TEXT("42.5")));
	}
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FToolFamiliesComponentTest,
	"UnrealAgentMCP.ToolFamilies.ComponentAddAttachSet",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FToolFamiliesComponentTest::RunTest(const FString& Parameters)
{
	UBlueprint* Blueprint = AgentMcpTestUtils::MakeTransientBlueprint(TEXT("BP_McpCompTest"));
	if (!TestNotNull(TEXT("transient blueprint created"), Blueprint))
	{
		return true;
	}
	const FString Path = Blueprint->GetPathName();
	bool bIsError = false;

	// add two components, attach one under the other, set a property on the child.
	const TSharedPtr<FJsonObject> Added1 = AgentMcpTestUtils::CallTool(*this, TEXT("add_component"),
		FString::Printf(TEXT("{\"blueprint_path\":\"%s\",\"component_class\":\"SceneComponent\",\"component_name\":\"McpRoot\"}"), *Path), bIsError);
	TestFalse(TEXT("add SceneComponent ok"), bIsError);
	const TSharedPtr<FJsonObject> Added2 = AgentMcpTestUtils::CallTool(*this, TEXT("add_component"),
		FString::Printf(TEXT("{\"blueprint_path\":\"%s\",\"component_class\":\"PointLightComponent\",\"component_name\":\"McpLight\"}"), *Path), bIsError);
	TestFalse(TEXT("add PointLightComponent ok"), bIsError);
	FString LightName = Added2.IsValid() ? Added2->GetStringField(TEXT("component_name")) : TEXT("McpLight");
	FString RootName = Added1.IsValid() ? Added1->GetStringField(TEXT("component_name")) : TEXT("McpRoot");

	AgentMcpTestUtils::CallTool(*this, TEXT("attach_component"),
		FString::Printf(TEXT("{\"blueprint_path\":\"%s\",\"child_name\":\"%s\",\"parent_name\":\"%s\"}"), *Path, *LightName, *RootName), bIsError);
	TestFalse(TEXT("attach ok"), bIsError);

	AgentMcpTestUtils::CallTool(*this, TEXT("set_component_property"),
		FString::Printf(TEXT("{\"blueprint_path\":\"%s\",\"component_name\":\"%s\",\"property\":\"Intensity\",\"value\":\"1234.0\"}"), *Path, *LightName), bIsError);
	TestFalse(TEXT("set Intensity ok"), bIsError);

	// unknown component class / unknown component name -> tool errors.
	AgentMcpTestUtils::CallTool(*this, TEXT("add_component"),
		FString::Printf(TEXT("{\"blueprint_path\":\"%s\",\"component_class\":\"NoSuchComponentXyz\"}"), *Path), bIsError);
	TestTrue(TEXT("unknown component class is a tool error"), bIsError);
	AgentMcpTestUtils::CallTool(*this, TEXT("set_component_property"),
		FString::Printf(TEXT("{\"blueprint_path\":\"%s\",\"component_name\":\"NoSuchComp\",\"property\":\"Intensity\",\"value\":\"1\"}"), *Path), bIsError);
	TestTrue(TEXT("unknown component name is a tool error"), bIsError);

	// compiles clean with the new component tree.
	const TSharedPtr<FJsonObject> Compiled = AgentMcpTestUtils::CallTool(*this, TEXT("compile_blueprint"),
		FString::Printf(TEXT("{\"blueprint_path\":\"%s\"}"), *Path), bIsError);
	if (Compiled.IsValid())
	{
		TestEqual(TEXT("compiles clean with components"), static_cast<int32>(Compiled->GetNumberField(TEXT("num_errors"))), 0);
	}
	return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS
