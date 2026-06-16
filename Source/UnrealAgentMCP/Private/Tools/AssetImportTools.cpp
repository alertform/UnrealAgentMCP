#include "Tools/AssetImportTools.h"

#include "AssetImportTask.h"
#include "AssetToolsModule.h"
#include "Core/AgentMcpToolRegistry.h"
#include "Core/McpTypes.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "Editor.h"
#include "IAssetTools.h"
#include "Misc/PackageName.h"
#include "Misc/Paths.h"
#include "Modules/ModuleManager.h"
#include "ObjectTools.h"
#include "Tools/McpToolUtils.h"
#include "UObject/Package.h"
#include "UObject/StrongObjectPtr.h"

namespace
{
	/**
	 * Extensions the stock editor importers handle deterministically (legacy factories or
	 * Interchange for glTF). Anything else is rejected up front so the agent gets a clear
	 * error instead of a silent zero-asset import.
	 */
	const TCHAR* SupportedExtensions[] = {
		TEXT("fbx"), TEXT("obj"), TEXT("gltf"), TEXT("glb"),
		TEXT("png"), TEXT("jpg"), TEXT("jpeg"), TEXT("tga"), TEXT("bmp"), TEXT("exr"), TEXT("hdr"),
		TEXT("wav"),
	};

	bool IsSupportedExtension(const FString& Extension)
	{
		for (const TCHAR* Supported : SupportedExtensions)
		{
			if (Extension == Supported)
			{
				return true;
			}
		}
		return false;
	}

	FString SupportedExtensionList()
	{
		FString List;
		for (const TCHAR* Supported : SupportedExtensions)
		{
			if (!List.IsEmpty())
			{
				List += TEXT(", ");
			}
			List += Supported;
		}
		return List;
	}

	FAgentMcpToolResult HandleImportAsset(const TSharedPtr<FJsonObject>& Args)
	{
		FString SourceFile;
		if (!Args.IsValid() || !Args->TryGetStringField(TEXT("source_file"), SourceFile) ||
			SourceFile.TrimStartAndEnd().IsEmpty())
		{
			return FAgentMcpToolResult::Error(
				TEXT("import_asset requires a non-empty 'source_file' string (absolute path to the file to import)."));
		}
		FString DestinationPath;
		if (!Args->TryGetStringField(TEXT("destination_path"), DestinationPath) ||
			DestinationPath.TrimStartAndEnd().IsEmpty())
		{
			return FAgentMcpToolResult::Error(
				TEXT("import_asset requires a non-empty 'destination_path' string, e.g. /Game/Imported."));
		}
		FString AssetName;
		bool bReplaceExisting = false;
		bool bSave = false;
		Args->TryGetStringField(TEXT("asset_name"), AssetName);
		Args->TryGetBoolField(TEXT("replace_existing"), bReplaceExisting);
		Args->TryGetBoolField(TEXT("save"), bSave);

		SourceFile = FPaths::ConvertRelativePathToFull(SourceFile);
		if (!FPaths::FileExists(SourceFile))
		{
			return FAgentMcpToolResult::Error(
				FString::Printf(TEXT("Source file not found: '%s'."), *SourceFile));
		}

		const FString Extension = FPaths::GetExtension(SourceFile).ToLower();
		if (!IsSupportedExtension(Extension))
		{
			return FAgentMcpToolResult::Error(FString::Printf(
				TEXT("Unsupported file extension '.%s'. Supported: %s."), *Extension, *SupportedExtensionList()));
		}

		// Imports may only land under /Game — never let an agent write into /Engine or plugin content.
		DestinationPath.RemoveFromEnd(TEXT("/"));
		if (DestinationPath != TEXT("/Game") && !DestinationPath.StartsWith(TEXT("/Game/")))
		{
			return FAgentMcpToolResult::Error(FString::Printf(
				TEXT("'destination_path' must be a /Game package path, e.g. /Game/Imported. Got: '%s'."), *DestinationPath));
		}
		FText InvalidReason;
		if (FPackageName::DoesPackageNameContainInvalidCharacters(DestinationPath, &InvalidReason))
		{
			return FAgentMcpToolResult::Error(FString::Printf(
				TEXT("'destination_path' contains invalid characters: %s"), *InvalidReason.ToString()));
		}

		if (!GEditor)
		{
			return FAgentMcpToolResult::Error(TEXT("GEditor unavailable (not running inside the editor)."));
		}

		// TStrongObjectPtr pins the transient task across any GC the importers trigger mid-call.
		TStrongObjectPtr<UAssetImportTask> Task(NewObject<UAssetImportTask>());
		Task->Filename = SourceFile;
		Task->DestinationPath = DestinationPath;
		if (!AssetName.TrimStartAndEnd().IsEmpty())
		{
			Task->DestinationName = ObjectTools::SanitizeObjectName(AssetName);
		}
		Task->bAutomated = true; // no import-options dialogs — headless-safe
		Task->bReplaceExisting = bReplaceExisting;
		Task->bSave = bSave;
		Task->bAsync = false; // synchronous: results are final when ImportAssetTasks returns

		FAssetToolsModule& AssetToolsModule = FModuleManager::LoadModuleChecked<FAssetToolsModule>(TEXT("AssetTools"));
		AssetToolsModule.Get().ImportAssetTasks({ Task.Get() });

		const TArray<UObject*> Imported = Task->GetObjects();
		if (Imported.Num() == 0)
		{
			return FAgentMcpToolResult::Error(FString::Printf(
				TEXT("Import produced no assets from '%s' — the importer rejected the file, or an existing asset blocked it (replace_existing=false). See Output Log."),
				*SourceFile));
		}

		TArray<TSharedPtr<FJsonValue>> AssetArray;
		AssetArray.Reserve(Imported.Num());
		for (const UObject* Object : Imported)
		{
			if (!Object)
			{
				continue;
			}
			TSharedRef<FJsonObject> Entry = MakeShared<FJsonObject>();
			Entry->SetStringField(TEXT("name"), Object->GetName());
			Entry->SetStringField(TEXT("class"), Object->GetClass()->GetName());
			Entry->SetStringField(TEXT("package_path"), Object->GetOutermost()->GetName());
			AssetArray.Add(MakeShared<FJsonValueObject>(Entry));
		}

		TSharedRef<FJsonObject> Result = MakeShared<FJsonObject>();
		Result->SetBoolField(TEXT("imported"), true);
		Result->SetNumberField(TEXT("count"), AssetArray.Num());
		Result->SetBoolField(TEXT("saved"), bSave);
		Result->SetArrayField(TEXT("assets"), AssetArray);
		return FAgentMcpToolResult::Success(AgentMcp::ToolUtils::SerializeObject(Result));
	}
} // namespace

void AgentMcp::Tools::RegisterAssetImportTools()
{
	// ── import_asset ──────────────────────────────────────────────────────────
	{
		FAgentMcpToolDef Def;
		Def.Name = TEXT("import_asset");
		Def.Description = TEXT(
			"Imports an external file on disk (fbx/obj/gltf/glb, png/jpg/tga/bmp/exr/hdr, wav) as project assets under /Game. "
			"The agent downloads files itself (e.g. from an asset-library API), then calls this with the local path. "
			"save=false (default) keeps the import in memory — call save_asset (or save=true) to write Content/. "
			"Returns {imported, count, saved, assets:[{name, class, package_path}]}.");
		Def.InputSchema = MakeShared<FJsonObject>();
		Def.InputSchema->SetStringField(TEXT("type"), TEXT("object"));
		{
			TSharedRef<FJsonObject> Props = MakeShared<FJsonObject>();

			TSharedRef<FJsonObject> SourceProp = MakeShared<FJsonObject>();
			SourceProp->SetStringField(TEXT("type"), TEXT("string"));
			SourceProp->SetStringField(TEXT("description"), TEXT("Absolute filesystem path of the file to import."));
			Props->SetObjectField(TEXT("source_file"), SourceProp);

			TSharedRef<FJsonObject> DestProp = MakeShared<FJsonObject>();
			DestProp->SetStringField(TEXT("type"), TEXT("string"));
			DestProp->SetStringField(TEXT("description"), TEXT("Destination package folder, must be under /Game, e.g. /Game/Imported."));
			Props->SetObjectField(TEXT("destination_path"), DestProp);

			TSharedRef<FJsonObject> NameProp = MakeShared<FJsonObject>();
			NameProp->SetStringField(TEXT("type"), TEXT("string"));
			NameProp->SetStringField(TEXT("description"), TEXT("Optional asset name override (sanitized). Default: source filename."));
			Props->SetObjectField(TEXT("asset_name"), NameProp);

			TSharedRef<FJsonObject> ReplaceProp = MakeShared<FJsonObject>();
			ReplaceProp->SetStringField(TEXT("type"), TEXT("boolean"));
			ReplaceProp->SetStringField(TEXT("description"), TEXT("Overwrite an existing asset of the same name. Default: false."));
			Props->SetObjectField(TEXT("replace_existing"), ReplaceProp);

			TSharedRef<FJsonObject> SaveProp = MakeShared<FJsonObject>();
			SaveProp->SetStringField(TEXT("type"), TEXT("boolean"));
			SaveProp->SetStringField(TEXT("description"), TEXT("Write the imported package(s) to disk immediately. Default: false."));
			Props->SetObjectField(TEXT("save"), SaveProp);

			Def.InputSchema->SetObjectField(TEXT("properties"), Props);

			TArray<TSharedPtr<FJsonValue>> Required;
			Required.Add(MakeShared<FJsonValueString>(TEXT("source_file")));
			Required.Add(MakeShared<FJsonValueString>(TEXT("destination_path")));
			Def.InputSchema->SetArrayField(TEXT("required"), Required);
		}
		Def.Tier = EAgentMcpTier::SafeWrite;
		Def.Handler = FAgentMcpToolHandler::CreateStatic(&HandleImportAsset);
		FAgentMcpToolRegistry::Get().Register(MoveTemp(Def));
	}
}
