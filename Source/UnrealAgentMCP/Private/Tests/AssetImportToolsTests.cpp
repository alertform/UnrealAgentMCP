#include "Misc/AutomationTest.h"

#if WITH_DEV_AUTOMATION_TESTS

#include "Tests/AgentMcpTestHelpers.h"
#include "Editor.h"
#include "Engine/Texture2D.h"
#include "HAL/FileManager.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "Subsystems/EditorAssetSubsystem.h"

namespace
{
	/**
	 * Writes a minimal uncompressed 24-bit 2x2 TGA (18-byte header + 4 BGR pixels = 30 bytes)
	 * under Saved/Temp. Built byte-by-byte — no embedded base64 blobs whose validity we can't
	 * eyeball. Returns the absolute path with forward slashes (JSON-safe), or empty on failure.
	 */
	FString WriteTestTga(const TCHAR* FileName)
	{
		TArray<uint8> Bytes;
		Bytes.SetNumZeroed(18);
		Bytes[2] = 2;                  // image type: uncompressed true-color
		Bytes[12] = 2; Bytes[13] = 0;  // width  = 2 (little-endian)
		Bytes[14] = 2; Bytes[15] = 0;  // height = 2 (little-endian)
		Bytes[16] = 24;                // bits per pixel
		for (int32 Pixel = 0; Pixel < 4; ++Pixel)
		{
			Bytes.Add(0x00); Bytes.Add(0x00); Bytes.Add(0xFF); // BGR: opaque red
		}
		const FString Path = FPaths::Combine(FPaths::ProjectSavedDir(), TEXT("Temp"), FileName);
		if (!FFileHelper::SaveArrayToFile(Bytes, *Path))
		{
			return FString();
		}
		return FPaths::ConvertRelativePathToFull(Path).Replace(TEXT("\\"), TEXT("/"));
	}
}

// ---------------------------------------------------------------------------
// Error-path: missing source_file argument -> isError + mentions 'source_file'
// ---------------------------------------------------------------------------
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FImportAssetMissingSourceArgTest,
	"UnrealAgentMCP.AssetImport.MissingSourceFileArgIsError",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FImportAssetMissingSourceArgTest::RunTest(const FString& Parameters)
{
	bool bIsError = false;
	const FString RawText = AgentMcpTestUtils::CallToolRawText(*this,
		TEXT("import_asset"), TEXT("{}"), bIsError);

	TestTrue(TEXT("import_asset without source_file is a tool error"), bIsError);
	TestTrue(TEXT("error message names 'source_file'"),
		RawText.Contains(TEXT("source_file"), ESearchCase::IgnoreCase));
	return true;
}

// ---------------------------------------------------------------------------
// Error-path: source file does not exist on disk -> isError + 'not found'
// ---------------------------------------------------------------------------
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FImportAssetMissingFileTest,
	"UnrealAgentMCP.AssetImport.NonexistentSourceFileIsError",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FImportAssetMissingFileTest::RunTest(const FString& Parameters)
{
	bool bIsError = false;
	const FString RawText = AgentMcpTestUtils::CallToolRawText(*this,
		TEXT("import_asset"),
		TEXT("{\"source_file\":\"X:/AgentMcp/DoesNotExist/missing.tga\",\"destination_path\":\"/Game/__McpTests\"}"),
		bIsError);

	TestTrue(TEXT("import_asset with nonexistent file is a tool error"), bIsError);
	TestTrue(TEXT("error message contains 'not found'"),
		RawText.Contains(TEXT("not found"), ESearchCase::IgnoreCase));
	return true;
}

// ---------------------------------------------------------------------------
// Error-path: destination outside /Game (e.g. /Engine) -> isError + mentions /Game
// ---------------------------------------------------------------------------
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FImportAssetBadDestinationTest,
	"UnrealAgentMCP.AssetImport.DestinationOutsideGameIsError",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FImportAssetBadDestinationTest::RunTest(const FString& Parameters)
{
	const FString TgaPath = WriteTestTga(TEXT("AgentMcpImportBadDest.tga"));
	TestFalse(TEXT("test TGA written"), TgaPath.IsEmpty());
	if (TgaPath.IsEmpty())
	{
		return false;
	}

	bool bIsError = false;
	const FString RawText = AgentMcpTestUtils::CallToolRawText(*this,
		TEXT("import_asset"),
		FString::Printf(TEXT("{\"source_file\":\"%s\",\"destination_path\":\"/Engine/Sneaky\"}"), *TgaPath),
		bIsError);

	TestTrue(TEXT("import_asset into /Engine is a tool error"), bIsError);
	TestTrue(TEXT("error message names '/Game'"), RawText.Contains(TEXT("/Game")));

	IFileManager::Get().Delete(*TgaPath);
	return true;
}

// ---------------------------------------------------------------------------
// Error-path: unsupported extension -> isError + 'unsupported'
// ---------------------------------------------------------------------------
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FImportAssetUnsupportedExtensionTest,
	"UnrealAgentMCP.AssetImport.UnsupportedExtensionIsError",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FImportAssetUnsupportedExtensionTest::RunTest(const FString& Parameters)
{
	// Any on-disk file with an extension outside the allowlist.
	const FString Path = FPaths::Combine(FPaths::ProjectSavedDir(), TEXT("Temp"), TEXT("AgentMcpImport.xyz"));
	FFileHelper::SaveStringToFile(TEXT("not an asset"), *Path);
	const FString FullPath = FPaths::ConvertRelativePathToFull(Path).Replace(TEXT("\\"), TEXT("/"));

	bool bIsError = false;
	const FString RawText = AgentMcpTestUtils::CallToolRawText(*this,
		TEXT("import_asset"),
		FString::Printf(TEXT("{\"source_file\":\"%s\",\"destination_path\":\"/Game/__McpTests\"}"), *FullPath),
		bIsError);

	TestTrue(TEXT("import_asset with .xyz file is a tool error"), bIsError);
	TestTrue(TEXT("error message contains 'unsupported'"),
		RawText.Contains(TEXT("unsupported"), ESearchCase::IgnoreCase));

	IFileManager::Get().Delete(*FullPath);
	return true;
}

// ---------------------------------------------------------------------------
// Happy-path: import a generated TGA as a Texture2D into /Game/__McpTests.
// save defaults to false -> the package stays in memory only, nothing touches
// Content/. replace_existing=true keeps the test idempotent within one session.
// ---------------------------------------------------------------------------
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FImportAssetTgaHappyPathTest,
	"UnrealAgentMCP.AssetImport.ImportTgaCreatesTexture2D",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FImportAssetTgaHappyPathTest::RunTest(const FString& Parameters)
{
	const FString TgaPath = WriteTestTga(TEXT("AgentMcpImportHappy.tga"));
	TestFalse(TEXT("test TGA written"), TgaPath.IsEmpty());
	if (TgaPath.IsEmpty())
	{
		return false;
	}

	bool bIsError = false;
	const TSharedPtr<FJsonObject> Result = AgentMcpTestUtils::CallTool(*this,
		TEXT("import_asset"),
		FString::Printf(
			TEXT("{\"source_file\":\"%s\",\"destination_path\":\"/Game/__McpTests/Imported\",")
			TEXT("\"asset_name\":\"T_McpImportTest\",\"replace_existing\":true}"),
			*TgaPath),
		bIsError);

	TestFalse(TEXT("import_asset happy path is not an error"), bIsError);
	if (!Result.IsValid())
	{
		AddError(TEXT("import_asset returned no parseable JSON payload."));
		IFileManager::Get().Delete(*TgaPath);
		return false;
	}

	bool bImported = false;
	TestTrue(TEXT("imported field is true"),
		Result->TryGetBoolField(TEXT("imported"), bImported) && bImported);

	double Count = 0.0;
	TestTrue(TEXT("count field is 1"),
		Result->TryGetNumberField(TEXT("count"), Count) && static_cast<int32>(Count) == 1);

	bool bSaved = true;
	TestTrue(TEXT("saved field defaults to false"),
		Result->TryGetBoolField(TEXT("saved"), bSaved) && !bSaved);

	const TArray<TSharedPtr<FJsonValue>>* Assets = nullptr;
	if (TestTrue(TEXT("assets array present"), Result->TryGetArrayField(TEXT("assets"), Assets) && Assets->Num() == 1))
	{
		const TSharedPtr<FJsonObject> Entry = (*Assets)[0]->AsObject();
		TestEqual(TEXT("asset name honors asset_name override"),
			Entry->GetStringField(TEXT("name")), FString(TEXT("T_McpImportTest")));
		TestEqual(TEXT("asset class is Texture2D"),
			Entry->GetStringField(TEXT("class")), FString(TEXT("Texture2D")));
		TestEqual(TEXT("package path is destination + name"),
			Entry->GetStringField(TEXT("package_path")),
			FString(TEXT("/Game/__McpTests/Imported/T_McpImportTest")));
	}

	// The imported object really exists in memory as a UTexture2D.
	UTexture2D* Texture = FindObject<UTexture2D>(nullptr,
		TEXT("/Game/__McpTests/Imported/T_McpImportTest.T_McpImportTest"));
	TestNotNull(TEXT("imported UTexture2D is findable in memory"), Texture);

	// Cleanup: the import leaves a DIRTY in-memory package; load_level's dirty-package
	// guard (and any later test relying on it) would trip over it. Delete it from memory.
	if (Texture)
	{
		UEditorAssetSubsystem* AssetSubsystem = GEditor ? GEditor->GetEditorSubsystem<UEditorAssetSubsystem>() : nullptr;
		TestTrue(TEXT("cleanup: imported texture deleted from memory"),
			AssetSubsystem && AssetSubsystem->DeleteLoadedAsset(Texture));
	}

	IFileManager::Get().Delete(*TgaPath);
	return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS
