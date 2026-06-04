#include "Tools/AssetQueryTools.h"

#include "AssetRegistry/AssetRegistryModule.h"
#include "AssetRegistry/IAssetRegistry.h"
#include "Core/AgentMcpToolRegistry.h"
#include "Core/McpTypes.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "Modules/ModuleManager.h"
#include "Serialization/JsonSerializer.h"
#include "Serialization/JsonWriter.h"

namespace
{
	constexpr int32 DefaultLimit = 200;
	constexpr int32 MaxLimit = 1000;

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

		FAssetRegistryModule& AssetRegistryModule = FModuleManager::LoadModuleChecked<FAssetRegistryModule>(TEXT("AssetRegistry"));
		IAssetRegistry& AssetRegistry = AssetRegistryModule.Get();
		AssetRegistry.WaitForCompletion();

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

		FString Out;
		const TSharedRef<TJsonWriter<>> Writer = TJsonWriterFactory<>::Create(&Out);
		FJsonSerializer::Serialize(Result, Writer);
		return FAgentMcpToolResult::Success(Out);
	}
}

void AgentMcp::Tools::RegisterAssetQueryTools()
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
