#include "Tools/AssetQueryTools.h"

#include "AssetRegistry/AssetData.h"
#include "AssetRegistry/AssetRegistryModule.h"
#include "AssetRegistry/IAssetRegistry.h"
#include "Core/AgentMcpToolRegistry.h"
#include "Core/McpTypes.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "Engine/Blueprint.h"
#include "FileHelpers.h"
#include "HAL/FileManager.h"
#include "Misc/PackageName.h"
#include "Misc/Paths.h"
#include "Modules/ModuleManager.h"
#include "Tools/McpToolUtils.h"
#include "UObject/Package.h"

namespace
{
	constexpr int32 DefaultLimit = 200;
	constexpr int32 MaxLimit = 1000;

	/** Returns a loaded AssetRegistry, completing the background scan first. */
	IAssetRegistry& GetRegistry()
	{
		FAssetRegistryModule& Mod = FModuleManager::LoadModuleChecked<FAssetRegistryModule>(TEXT("AssetRegistry"));
		IAssetRegistry& AR = Mod.Get();
		AR.WaitForCompletion();
		return AR;
	}

	FAgentMcpToolResult HandleListAssets(const TSharedPtr<FJsonObject>& Args)
	{
		FString Path = TEXT("/Game");
		bool bRecursive = true;
		int32 Limit = DefaultLimit;
		if (Args.IsValid())
		{
			Args->TryGetStringField(TEXT("path"), Path);
			Args->TryGetBoolField(TEXT("recursive"), bRecursive);
			double LimitNumber = 0.0;
			if (Args->TryGetNumberField(TEXT("limit"), LimitNumber))
			{
				Limit = FMath::Clamp(static_cast<int32>(LimitNumber), 1, MaxLimit);
			}
		}
		if (!Path.StartsWith(TEXT("/")))
		{
			return FAgentMcpToolResult::Error(
				FString::Printf(TEXT("'path' must be an absolute package path starting with '/', e.g. /Game/Blueprints. Got: '%s'"), *Path));
		}

		IAssetRegistry& AssetRegistry = GetRegistry();

		TArray<FAssetData> Assets;
		AssetRegistry.GetAssetsByPath(FName(*Path), Assets, bRecursive);

		TArray<TSharedPtr<FJsonValue>> AssetArray;
		const int32 ReturnCount = FMath::Min(Assets.Num(), Limit);
		AssetArray.Reserve(ReturnCount);
		for (int32 Index = 0; Index < ReturnCount; ++Index)
		{
			TSharedRef<FJsonObject> Entry = MakeShared<FJsonObject>();
			Entry->SetStringField(TEXT("name"), Assets[Index].AssetName.ToString());
			Entry->SetStringField(TEXT("class"), Assets[Index].AssetClassPath.GetAssetName().ToString());
			Entry->SetStringField(TEXT("package_path"), Assets[Index].PackageName.ToString());
			AssetArray.Add(MakeShared<FJsonValueObject>(Entry));
		}

		TSharedRef<FJsonObject> Result = MakeShared<FJsonObject>();
		Result->SetNumberField(TEXT("total"), Assets.Num());
		Result->SetNumberField(TEXT("returned"), ReturnCount);
		Result->SetArrayField(TEXT("assets"), AssetArray);

		return FAgentMcpToolResult::Success(AgentMcp::ToolUtils::SerializeObject(Result));
	}

	FAgentMcpToolResult HandleSearchAssets(const TSharedPtr<FJsonObject>& Args)
	{
		FString NameContains;
		FString ClassName;
		FString Path = TEXT("/Game");
		int32 Limit = 100;

		if (Args.IsValid())
		{
			Args->TryGetStringField(TEXT("name_contains"), NameContains);
			Args->TryGetStringField(TEXT("class_name"), ClassName);
			Args->TryGetStringField(TEXT("path"), Path);
			double LimitNumber = 0.0;
			if (Args->TryGetNumberField(TEXT("limit"), LimitNumber))
			{
				Limit = FMath::Clamp(static_cast<int32>(LimitNumber), 1, MaxLimit);
			}
		}
		if (!Path.StartsWith(TEXT("/")))
		{
			return FAgentMcpToolResult::Error(
				FString::Printf(TEXT("'path' must start with '/', e.g. /Game. Got: '%s'"), *Path));
		}

		IAssetRegistry& AR = GetRegistry();

		FARFilter Filter;
		Filter.bRecursivePaths = true;
		Filter.PackagePaths.Add(FName(*Path));
		if (!ClassName.IsEmpty())
		{
			if (UClass* FilterClass = UClass::TryFindTypeSlow<UClass>(ClassName))
			{
				Filter.ClassPaths.Add(FilterClass->GetClassPathName());
			}
			else
			{
				// Unknown class: still query but warn via filter mismatch — no assets will match,
				// which is correct behaviour (the client typo'd the class name).
				return FAgentMcpToolResult::Error(
					FString::Printf(TEXT("class_name '%s' not found. Use a valid UClass name, e.g. Blueprint, StaticMesh."), *ClassName));
			}
		}

		TArray<FAssetData> Assets;
		AR.GetAssets(Filter, Assets);

		// Apply optional name_contains filter (case-insensitive).
		if (!NameContains.IsEmpty())
		{
			Assets.RemoveAll([&NameContains](const FAssetData& A)
			{
				return !A.AssetName.ToString().Contains(NameContains, ESearchCase::IgnoreCase);
			});
		}

		const int32 Total = Assets.Num();
		const int32 ReturnCount = FMath::Min(Total, Limit);

		TArray<TSharedPtr<FJsonValue>> AssetArray;
		AssetArray.Reserve(ReturnCount);
		for (int32 Index = 0; Index < ReturnCount; ++Index)
		{
			TSharedRef<FJsonObject> Entry = MakeShared<FJsonObject>();
			Entry->SetStringField(TEXT("name"), Assets[Index].AssetName.ToString());
			Entry->SetStringField(TEXT("class"), Assets[Index].AssetClassPath.GetAssetName().ToString());
			Entry->SetStringField(TEXT("package_path"), Assets[Index].PackageName.ToString());
			AssetArray.Add(MakeShared<FJsonValueObject>(Entry));
		}

		TSharedRef<FJsonObject> Result = MakeShared<FJsonObject>();
		Result->SetNumberField(TEXT("total"), Total);
		Result->SetNumberField(TEXT("returned"), ReturnCount);
		Result->SetArrayField(TEXT("assets"), AssetArray);
		return FAgentMcpToolResult::Success(AgentMcp::ToolUtils::SerializeObject(Result));
	}

	FAgentMcpToolResult HandleGetAssetInfo(const TSharedPtr<FJsonObject>& Args)
	{
		FString AssetPath;
		if (!Args.IsValid() || !Args->TryGetStringField(TEXT("asset_path"), AssetPath))
		{
			return FAgentMcpToolResult::Error(TEXT("Missing required string argument 'asset_path'."));
		}

		IAssetRegistry& AR = GetRegistry();

		// Accept both package paths (/Game/Foo/Bar) and object paths (/Game/Foo/Bar.Bar).
		// Try package lookup first; if not found, try as object path directly.
		FAssetData AssetData;
		{
			// Normalize: if path has no dot it's a package name — get all assets in the package.
			if (!AssetPath.Contains(TEXT(".")))
			{
				TArray<FAssetData> Found;
				AR.GetAssetsByPackageName(FName(*AssetPath), Found);
				if (Found.Num() > 0)
				{
					AssetData = Found[0];
				}
			}
			else
			{
				AssetData = AR.GetAssetByObjectPath(FSoftObjectPath(AssetPath));
			}
		}

		if (!AssetData.IsValid())
		{
			return FAgentMcpToolResult::Error(
				FString::Printf(TEXT("Asset not found in registry: '%s'. Use search_assets or list_assets to find valid paths."), *AssetPath));
		}

		TSharedRef<FJsonObject> Result = MakeShared<FJsonObject>();
		Result->SetStringField(TEXT("name"), AssetData.AssetName.ToString());
		Result->SetStringField(TEXT("class"), AssetData.AssetClassPath.GetAssetName().ToString());
		Result->SetStringField(TEXT("package_path"), AssetData.PackageName.ToString());

		// Disk size: convert package name to filename and stat it.
		FString PackageFilename;
		if (FPackageName::TryConvertLongPackageNameToFilename(AssetData.PackageName.ToString(), PackageFilename, FPackageName::GetAssetPackageExtension()))
		{
			const int64 FileSize = IFileManager::Get().FileSize(*PackageFilename);
			if (FileSize >= 0)
			{
				Result->SetNumberField(TEXT("disk_size"), static_cast<double>(FileSize));
			}
		}

		// Tags: include up to 8 tag/value pairs from AssetData.TagsAndValues.
		// Decision: iterate and cap at 8 to avoid ballooning the response for assets
		// with many auto-generated tags (e.g. StaticMesh LOD data). Clients needing
		// all tags can call this tool and filter; the cap keeps the response lean.
		TSharedRef<FJsonObject> TagsObj = MakeShared<FJsonObject>();
		int32 TagCount = 0;
		for (auto It = AssetData.TagsAndValues.CreateConstIterator(); It && TagCount < 8; ++It, ++TagCount)
		{
			TagsObj->SetStringField(It.Key().ToString(), It.Value().AsString());
		}
		Result->SetObjectField(TEXT("tags"), TagsObj);

		return FAgentMcpToolResult::Success(AgentMcp::ToolUtils::SerializeObject(Result));
	}

	FAgentMcpToolResult HandleGetReferences(const TSharedPtr<FJsonObject>& Args)
	{
		FString AssetPath;
		if (!Args.IsValid() || !Args->TryGetStringField(TEXT("asset_path"), AssetPath))
		{
			return FAgentMcpToolResult::Error(TEXT("Missing required string argument 'asset_path'."));
		}

		FString Direction = TEXT("referencers");
		if (Args.IsValid())
		{
			Args->TryGetStringField(TEXT("direction"), Direction);
		}

		const bool bReferencers = !Direction.Equals(TEXT("dependencies"), ESearchCase::IgnoreCase);

		// Normalize to package name: strip object suffix if present.
		FString PackageName = AssetPath;
		int32 DotIndex = INDEX_NONE;
		if (PackageName.FindChar(TEXT('.'), DotIndex))
		{
			PackageName = PackageName.Left(DotIndex);
		}

		IAssetRegistry& AR = GetRegistry();

		// 5.5 overload: GetReferencers(FName PackageName, TArray<FName>& OutReferencers,
		//   EDependencyCategory Category = EDependencyCategory::Package, FDependencyQuery Flags = {})
		TArray<FName> OutPackages;
		bool bOk = false;
		if (bReferencers)
		{
			bOk = AR.GetReferencers(FName(*PackageName), OutPackages,
				UE::AssetRegistry::EDependencyCategory::Package);
		}
		else
		{
			bOk = AR.GetDependencies(FName(*PackageName), OutPackages,
				UE::AssetRegistry::EDependencyCategory::Package);
		}

		if (!bOk)
		{
			// Not found in registry — check if path is even known.
			return FAgentMcpToolResult::Error(
				FString::Printf(TEXT("Package '%s' not found in asset registry. Use search_assets to verify the path."), *PackageName));
		}

		TArray<TSharedPtr<FJsonValue>> PackageArray;
		PackageArray.Reserve(OutPackages.Num());
		for (const FName& Pkg : OutPackages)
		{
			PackageArray.Add(MakeShared<FJsonValueString>(Pkg.ToString()));
		}

		TSharedRef<FJsonObject> Result = MakeShared<FJsonObject>();
		Result->SetStringField(TEXT("direction"), Direction.ToLower());
		Result->SetNumberField(TEXT("count"), OutPackages.Num());
		Result->SetArrayField(TEXT("packages"), PackageArray);
		return FAgentMcpToolResult::Success(AgentMcp::ToolUtils::SerializeObject(Result));
	}

	FAgentMcpToolResult HandleSaveAsset(const TSharedPtr<FJsonObject>& Args)
	{
		FString AssetPath;
		if (!Args.IsValid() || !Args->TryGetStringField(TEXT("asset_path"), AssetPath))
		{
			return FAgentMcpToolResult::Error(TEXT("Missing required string argument 'asset_path'."));
		}

		// Normalize to package name.
		FString PackageName = AssetPath;
		int32 DotIndex = INDEX_NONE;
		if (PackageName.FindChar(TEXT('.'), DotIndex))
		{
			PackageName = PackageName.Left(DotIndex);
		}

		// Find the in-memory package (must already be loaded; we don't force-load here).
		UPackage* Package = FindPackage(nullptr, *PackageName);
		if (!Package)
		{
			// Try loading it.
			Package = LoadPackage(nullptr, *PackageName, LOAD_NoWarn);
		}
		if (!Package)
		{
			return FAgentMcpToolResult::Error(
				FString::Printf(TEXT("Package '%s' is not loaded. Load or create the asset first, e.g. via create_blueprint."), *PackageName));
		}

		// Mark dirty so SavePackages with bOnlyDirty=false still writes it reliably.
		Package->MarkPackageDirty();

		TArray<UPackage*> PackagesToSave;
		PackagesToSave.Add(Package);
		const bool bSaved = UEditorLoadingAndSavingUtils::SavePackages(PackagesToSave, /*bOnlyDirty*/ false);
		if (!bSaved)
		{
			return FAgentMcpToolResult::Error(
				FString::Printf(TEXT("SavePackages returned false for '%s'. The package may be read-only or locked by source control."), *PackageName));
		}

		TSharedRef<FJsonObject> Result = MakeShared<FJsonObject>();
		Result->SetBoolField(TEXT("saved"), true);
		Result->SetStringField(TEXT("package"), Package->GetName());
		return FAgentMcpToolResult::Success(AgentMcp::ToolUtils::SerializeObject(Result));
	}
} // namespace

void AgentMcp::Tools::RegisterAssetQueryTools()
{
	// ── list_assets ──────────────────────────────────────────────────────────
	{
		FAgentMcpToolDef Def;
		Def.Name = TEXT("list_assets");
		Def.Description = TEXT("Lists assets under a package path via the Asset Registry. Args: path (string, default /Game), recursive (bool, default true), limit (int 1-1000, default 200). Returns {total, returned, assets:[{name, class, package_path}]}.");
		Def.InputSchema = MakeShared<FJsonObject>();
		Def.InputSchema->SetStringField(TEXT("type"), TEXT("object"));
		{
			TSharedRef<FJsonObject> Properties = MakeShared<FJsonObject>();
			TSharedRef<FJsonObject> PathProp = MakeShared<FJsonObject>();
			PathProp->SetStringField(TEXT("type"), TEXT("string"));
			PathProp->SetStringField(TEXT("description"), TEXT("Absolute package path, e.g. /Game/Blueprints"));
			Properties->SetObjectField(TEXT("path"), PathProp);
			TSharedRef<FJsonObject> RecursiveProp = MakeShared<FJsonObject>();
			RecursiveProp->SetStringField(TEXT("type"), TEXT("boolean"));
			Properties->SetObjectField(TEXT("recursive"), RecursiveProp);
			TSharedRef<FJsonObject> LimitProp = MakeShared<FJsonObject>();
			LimitProp->SetStringField(TEXT("type"), TEXT("integer"));
			Properties->SetObjectField(TEXT("limit"), LimitProp);
			Def.InputSchema->SetObjectField(TEXT("properties"), Properties);
		}
		Def.Tier = EAgentMcpTier::ReadOnly;
		Def.Handler = FAgentMcpToolHandler::CreateStatic(&HandleListAssets);
		FAgentMcpToolRegistry::Get().Register(MoveTemp(Def));
	}

	// ── search_assets ─────────────────────────────────────────────────────────
	{
		FAgentMcpToolDef Def;
		Def.Name = TEXT("search_assets");
		Def.Description = TEXT("Searches assets in the Asset Registry with optional filters. Returns {total, returned, assets:[{name, class, package_path}]}.");
		Def.InputSchema = MakeShared<FJsonObject>();
		Def.InputSchema->SetStringField(TEXT("type"), TEXT("object"));
		{
			TSharedRef<FJsonObject> Props = MakeShared<FJsonObject>();

			TSharedRef<FJsonObject> NameContainsProp = MakeShared<FJsonObject>();
			NameContainsProp->SetStringField(TEXT("type"), TEXT("string"));
			NameContainsProp->SetStringField(TEXT("description"), TEXT("Case-insensitive substring filter on the asset name."));
			Props->SetObjectField(TEXT("name_contains"), NameContainsProp);

			TSharedRef<FJsonObject> ClassNameProp = MakeShared<FJsonObject>();
			ClassNameProp->SetStringField(TEXT("type"), TEXT("string"));
			ClassNameProp->SetStringField(TEXT("description"), TEXT("UClass short name filter, e.g. Blueprint, StaticMesh, Texture2D."));
			Props->SetObjectField(TEXT("class_name"), ClassNameProp);

			TSharedRef<FJsonObject> PathProp = MakeShared<FJsonObject>();
			PathProp->SetStringField(TEXT("type"), TEXT("string"));
			PathProp->SetStringField(TEXT("description"), TEXT("Root package path to search under (default /Game). Recursive."));
			Props->SetObjectField(TEXT("path"), PathProp);

			TSharedRef<FJsonObject> LimitProp = MakeShared<FJsonObject>();
			LimitProp->SetStringField(TEXT("type"), TEXT("integer"));
			LimitProp->SetStringField(TEXT("description"), TEXT("Max results to return (1-1000, default 100)."));
			Props->SetObjectField(TEXT("limit"), LimitProp);

			Def.InputSchema->SetObjectField(TEXT("properties"), Props);
		}
		Def.Tier = EAgentMcpTier::ReadOnly;
		Def.Handler = FAgentMcpToolHandler::CreateStatic(&HandleSearchAssets);
		FAgentMcpToolRegistry::Get().Register(MoveTemp(Def));
	}

	// ── get_asset_info ────────────────────────────────────────────────────────
	{
		FAgentMcpToolDef Def;
		Def.Name = TEXT("get_asset_info");
		Def.Description = TEXT("Returns metadata for a single asset: name, class, package_path, disk_size (bytes, omitted if not on disk), and tags (up to 8 tag/value pairs from AssetData). Returns {name, class, package_path, disk_size?, tags:{...}}.");
		Def.InputSchema = MakeShared<FJsonObject>();
		Def.InputSchema->SetStringField(TEXT("type"), TEXT("object"));
		{
			TSharedRef<FJsonObject> Props = MakeShared<FJsonObject>();
			TSharedRef<FJsonObject> AssetPathProp = MakeShared<FJsonObject>();
			AssetPathProp->SetStringField(TEXT("type"), TEXT("string"));
			AssetPathProp->SetStringField(TEXT("description"), TEXT("Package path (e.g. /Game/Blueprints/BP_Foo) or object path (e.g. /Game/Blueprints/BP_Foo.BP_Foo)."));
			Props->SetObjectField(TEXT("asset_path"), AssetPathProp);
			Def.InputSchema->SetObjectField(TEXT("properties"), Props);

			TArray<TSharedPtr<FJsonValue>> Required;
			Required.Add(MakeShared<FJsonValueString>(TEXT("asset_path")));
			Def.InputSchema->SetArrayField(TEXT("required"), Required);
		}
		Def.Tier = EAgentMcpTier::ReadOnly;
		Def.Handler = FAgentMcpToolHandler::CreateStatic(&HandleGetAssetInfo);
		FAgentMcpToolRegistry::Get().Register(MoveTemp(Def));
	}

	// ── get_references ────────────────────────────────────────────────────────
	{
		FAgentMcpToolDef Def;
		Def.Name = TEXT("get_references");
		Def.Description = TEXT("Returns hard-dependency references for an asset via the Asset Registry. direction='referencers' (default): assets that depend ON this asset; direction='dependencies': assets this asset depends ON. Returns {direction, count, packages:[...]}.");
		Def.InputSchema = MakeShared<FJsonObject>();
		Def.InputSchema->SetStringField(TEXT("type"), TEXT("object"));
		{
			TSharedRef<FJsonObject> Props = MakeShared<FJsonObject>();

			TSharedRef<FJsonObject> AssetPathProp = MakeShared<FJsonObject>();
			AssetPathProp->SetStringField(TEXT("type"), TEXT("string"));
			AssetPathProp->SetStringField(TEXT("description"), TEXT("Package or object path of the asset to inspect."));
			Props->SetObjectField(TEXT("asset_path"), AssetPathProp);

			TSharedRef<FJsonObject> DirectionProp = MakeShared<FJsonObject>();
			DirectionProp->SetStringField(TEXT("type"), TEXT("string"));
			DirectionProp->SetStringField(TEXT("description"), TEXT("'referencers' (who uses this asset) or 'dependencies' (what this asset uses). Default: referencers."));
			Props->SetObjectField(TEXT("direction"), DirectionProp);

			Def.InputSchema->SetObjectField(TEXT("properties"), Props);

			TArray<TSharedPtr<FJsonValue>> Required;
			Required.Add(MakeShared<FJsonValueString>(TEXT("asset_path")));
			Def.InputSchema->SetArrayField(TEXT("required"), Required);
		}
		Def.Tier = EAgentMcpTier::ReadOnly;
		Def.Handler = FAgentMcpToolHandler::CreateStatic(&HandleGetReferences);
		FAgentMcpToolRegistry::Get().Register(MoveTemp(Def));
	}

	// ── save_asset ────────────────────────────────────────────────────────────
	{
		FAgentMcpToolDef Def;
		Def.Name = TEXT("save_asset");
		Def.Description = TEXT("WRITES the .uasset to disk under Content/ — the asset becomes part of the project. The package must already be loaded (e.g. after create_blueprint). Returns {saved:true, package}.");
		Def.InputSchema = MakeShared<FJsonObject>();
		Def.InputSchema->SetStringField(TEXT("type"), TEXT("object"));
		{
			TSharedRef<FJsonObject> Props = MakeShared<FJsonObject>();
			TSharedRef<FJsonObject> AssetPathProp = MakeShared<FJsonObject>();
			AssetPathProp->SetStringField(TEXT("type"), TEXT("string"));
			AssetPathProp->SetStringField(TEXT("description"), TEXT("Package path of the asset to save, e.g. /Game/Blueprints/BP_Foo."));
			Props->SetObjectField(TEXT("asset_path"), AssetPathProp);
			Def.InputSchema->SetObjectField(TEXT("properties"), Props);

			TArray<TSharedPtr<FJsonValue>> Required;
			Required.Add(MakeShared<FJsonValueString>(TEXT("asset_path")));
			Def.InputSchema->SetArrayField(TEXT("required"), Required);
		}
		Def.Tier = EAgentMcpTier::SafeWrite;
		Def.Handler = FAgentMcpToolHandler::CreateStatic(&HandleSaveAsset);
		FAgentMcpToolRegistry::Get().Register(MoveTemp(Def));
	}
}
