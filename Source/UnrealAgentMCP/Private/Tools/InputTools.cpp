#include "Tools/InputTools.h"

// FAssetRegistryModule::AssetCreated needs this header.
#include "AssetRegistry/AssetRegistryModule.h"
#include "Core/AgentMcpToolRegistry.h"
#include "Core/McpTypes.h"
#include "Dom/JsonObject.h"
#include "InputAction.h"
#include "InputCoreTypes.h"
#include "InputMappingContext.h"
#include "ScopedTransaction.h"
#include "Tools/McpToolUtils.h"
#include "UObject/Package.h"

namespace
{
	using namespace AgentMcp;

	/** Splits "/Game/A/IA_X" (or "/Game/A/IA_X.IA_X") into package path + asset name. */
	bool SplitAssetPath(const FString& AssetPath, FString& OutPackagePath, FString& OutAssetName, FString& OutError)
	{
		if (!AssetPath.StartsWith(TEXT("/")))
		{
			OutError = FString::Printf(TEXT("'asset_path' must be an absolute package path. Got: '%s'"), *AssetPath);
			return false;
		}
		OutPackagePath = AssetPath;
		FString Discard;
		OutPackagePath.Split(TEXT("."), &OutPackagePath, &Discard); // tolerate object-path form
		if (!OutPackagePath.Split(TEXT("/"), nullptr, &OutAssetName, ESearchCase::CaseSensitive, ESearchDir::FromEnd) || OutAssetName.IsEmpty())
		{
			OutError = FString::Printf(TEXT("Cannot derive an asset name from '%s'."), *AssetPath);
			return false;
		}
		return true;
	}

	/** Canonical no-factory data-asset creation: CreatePackage + NewObject + AssetCreated. In-memory until save_asset.
	 *  No FScopedTransaction: asset creation does not go through the editor undo system (no Modify() records are
	 *  written). Undo for created assets = delete_asset, which is Destructive-tier territory. Same rationale as
	 *  create_blueprint. */
	template <typename TAssetClass>
	TAssetClass* CreateDataAsset(const FString& AssetPath, FString& OutError)
	{
		FString PackagePath, AssetName;
		if (!SplitAssetPath(AssetPath, PackagePath, AssetName, OutError))
		{
			return nullptr;
		}
		const FString ObjectPath = PackagePath + TEXT(".") + AssetName;
		if (FindObject<UObject>(nullptr, *ObjectPath))
		{
			OutError = FString::Printf(TEXT("Asset already exists at '%s'."), *ObjectPath);
			return nullptr;
		}
		UPackage* Package = CreatePackage(*PackagePath);
		TAssetClass* Asset = NewObject<TAssetClass>(Package, FName(*AssetName), RF_Public | RF_Standalone | RF_Transactional);
		if (!Asset)
		{
			OutError = FString::Printf(TEXT("NewObject failed for '%s'."), *ObjectPath);
			return nullptr;
		}
		FAssetRegistryModule::AssetCreated(Asset);
		Package->MarkPackageDirty();
		return Asset;
	}

	bool ParseValueType(const FString& Token, EInputActionValueType& OutType, FString& OutError)
	{
		const FString Lower = Token.ToLower();
		if (Lower == TEXT("boolean") || Lower.IsEmpty()) { OutType = EInputActionValueType::Boolean; return true; }
		if (Lower == TEXT("axis1d")) { OutType = EInputActionValueType::Axis1D; return true; }
		if (Lower == TEXT("axis2d")) { OutType = EInputActionValueType::Axis2D; return true; }
		if (Lower == TEXT("axis3d")) { OutType = EInputActionValueType::Axis3D; return true; }
		OutError = FString::Printf(TEXT("Unknown value_type '%s'. Supported: boolean, axis1d, axis2d, axis3d."), *Token);
		return false;
	}

	FAgentMcpToolResult HandleCreateInputAction(const TSharedPtr<FJsonObject>& Args)
	{
		FString AssetPath;
		if (!Args.IsValid() || !Args->TryGetStringField(TEXT("asset_path"), AssetPath))
		{
			return FAgentMcpToolResult::Error(TEXT("Missing required string argument 'asset_path' (e.g. /Game/Input/IA_Jump)."));
		}
		FString ValueTypeToken;
		Args->TryGetStringField(TEXT("value_type"), ValueTypeToken);
		EInputActionValueType ValueType = EInputActionValueType::Boolean;
		FString Error;
		if (!ParseValueType(ValueTypeToken, ValueType, Error))
		{
			return FAgentMcpToolResult::Error(Error);
		}
		UInputAction* Action = CreateDataAsset<UInputAction>(AssetPath, Error);
		if (!Action)
		{
			return FAgentMcpToolResult::Error(Error);
		}
		Action->ValueType = ValueType;

		TSharedRef<FJsonObject> Result = MakeShared<FJsonObject>();
		Result->SetBoolField(TEXT("created"), true);
		Result->SetStringField(TEXT("asset_path"), Action->GetPathName());
		Result->SetStringField(TEXT("value_type"), ValueTypeToken.IsEmpty() ? TEXT("boolean") : ValueTypeToken.ToLower());
		return FAgentMcpToolResult::Success(ToolUtils::SerializeObject(Result));
	}

	FAgentMcpToolResult HandleCreateMappingContext(const TSharedPtr<FJsonObject>& Args)
	{
		FString AssetPath;
		if (!Args.IsValid() || !Args->TryGetStringField(TEXT("asset_path"), AssetPath))
		{
			return FAgentMcpToolResult::Error(TEXT("Missing required string argument 'asset_path' (e.g. /Game/Input/IMC_Default)."));
		}
		FString Error;
		UInputMappingContext* Context = CreateDataAsset<UInputMappingContext>(AssetPath, Error);
		if (!Context)
		{
			return FAgentMcpToolResult::Error(Error);
		}
		TSharedRef<FJsonObject> Result = MakeShared<FJsonObject>();
		Result->SetBoolField(TEXT("created"), true);
		Result->SetStringField(TEXT("asset_path"), Context->GetPathName());
		return FAgentMcpToolResult::Success(ToolUtils::SerializeObject(Result));
	}

	/** Resolves a loaded-or-on-disk asset of T by package/object path. */
	template <typename TAssetClass>
	TAssetClass* ResolveInputAsset(const FString& Path, const TCHAR* What, FString& OutError)
	{
		FString PackagePath, AssetName;
		if (!SplitAssetPath(Path, PackagePath, AssetName, OutError))
		{
			return nullptr;
		}
		const FString ObjectPath = PackagePath + TEXT(".") + AssetName;
		TAssetClass* Asset = FindObject<TAssetClass>(nullptr, *ObjectPath);
		if (!Asset)
		{
			Asset = LoadObject<TAssetClass>(nullptr, *ObjectPath);
		}
		if (!Asset)
		{
			OutError = FString::Printf(TEXT("%s not found: '%s'."), What, *Path);
		}
		return Asset;
	}

	FAgentMcpToolResult HandleAddMappingEntry(const TSharedPtr<FJsonObject>& Args)
	{
		FString ContextPath, ActionPath, KeyName;
		if (!Args.IsValid() || !Args->TryGetStringField(TEXT("context_path"), ContextPath) ||
			!Args->TryGetStringField(TEXT("action_path"), ActionPath) || !Args->TryGetStringField(TEXT("key"), KeyName))
		{
			return FAgentMcpToolResult::Error(TEXT("add_mapping_entry requires context_path, action_path and key."));
		}
		const FName KeyFName(*KeyName);
		const ::FKey Key{KeyFName};
		if (!EKeys::GetKeyDetails(Key).IsValid())
		{
			return FAgentMcpToolResult::Error(FString::Printf(
				TEXT("'%s' is not a valid UE key name. Examples: SpaceBar, W, LeftMouseButton, Gamepad_FaceButton_Bottom, LeftShift."), *KeyName));
		}
		FString Error;
		UInputMappingContext* Context = ResolveInputAsset<UInputMappingContext>(ContextPath, TEXT("Mapping context"), Error);
		if (!Context) { return FAgentMcpToolResult::Error(Error); }
		UInputAction* Action = ResolveInputAsset<UInputAction>(ActionPath, TEXT("Input action"), Error);
		if (!Action) { return FAgentMcpToolResult::Error(Error); }

		const FScopedTransaction Transaction(NSLOCTEXT("AgentMcp", "AddMappingEntry", "MCP: Add Mapping Entry"));
		Context->Modify();
		Context->MapKey(Action, Key);
		Context->MarkPackageDirty();

		TSharedRef<FJsonObject> Result = MakeShared<FJsonObject>();
		Result->SetBoolField(TEXT("mapped"), true);
		Result->SetStringField(TEXT("key"), KeyName);
		Result->SetNumberField(TEXT("total_mappings"), Context->GetMappings().Num());
		return FAgentMcpToolResult::Success(ToolUtils::SerializeObject(Result));
	}
}

void AgentMcp::Tools::RegisterInputTools()
{
	// create_input_action
	{
		FAgentMcpToolDef Def;
		Def.Name = TEXT("create_input_action");
		Def.Description = TEXT("Creates a new UInputAction data asset in memory (NOT saved to disk yet — call save_asset to persist). "
			"Args: asset_path (required, e.g. /Game/Input/IA_Jump), value_type (optional: boolean|axis1d|axis2d|axis3d, default boolean). "
			"Returns {created, asset_path, value_type}.");
		Def.InputSchema = MakeShared<FJsonObject>();
		Def.InputSchema->SetStringField(TEXT("type"), TEXT("object"));
		{
			TSharedRef<FJsonObject> Properties = MakeShared<FJsonObject>();

			TSharedRef<FJsonObject> PathProp = MakeShared<FJsonObject>();
			PathProp->SetStringField(TEXT("type"), TEXT("string"));
			PathProp->SetStringField(TEXT("description"), TEXT("Absolute asset path for the new InputAction, e.g. /Game/Input/IA_Jump"));
			Properties->SetObjectField(TEXT("asset_path"), PathProp);

			TSharedRef<FJsonObject> ValueTypeProp = MakeShared<FJsonObject>();
			ValueTypeProp->SetStringField(TEXT("type"), TEXT("string"));
			ValueTypeProp->SetStringField(TEXT("description"), TEXT("Value type token: boolean (default), axis1d, axis2d, or axis3d."));
			Properties->SetObjectField(TEXT("value_type"), ValueTypeProp);

			Def.InputSchema->SetObjectField(TEXT("properties"), Properties);

			TArray<TSharedPtr<FJsonValue>> Required;
			Required.Add(MakeShared<FJsonValueString>(TEXT("asset_path")));
			Def.InputSchema->SetArrayField(TEXT("required"), Required);
		}
		Def.Tier = EAgentMcpTier::SafeWrite;
		Def.Handler = FAgentMcpToolHandler::CreateStatic(&HandleCreateInputAction);
		FAgentMcpToolRegistry::Get().Register(MoveTemp(Def));
	}

	// create_mapping_context
	{
		FAgentMcpToolDef Def;
		Def.Name = TEXT("create_mapping_context");
		Def.Description = TEXT("Creates a new UInputMappingContext data asset in memory (NOT saved to disk yet — call save_asset to persist). "
			"Args: asset_path (required, e.g. /Game/Input/IMC_Default). "
			"Returns {created, asset_path}.");
		Def.InputSchema = MakeShared<FJsonObject>();
		Def.InputSchema->SetStringField(TEXT("type"), TEXT("object"));
		{
			TSharedRef<FJsonObject> Properties = MakeShared<FJsonObject>();

			TSharedRef<FJsonObject> PathProp = MakeShared<FJsonObject>();
			PathProp->SetStringField(TEXT("type"), TEXT("string"));
			PathProp->SetStringField(TEXT("description"), TEXT("Absolute asset path for the new InputMappingContext, e.g. /Game/Input/IMC_Default"));
			Properties->SetObjectField(TEXT("asset_path"), PathProp);

			Def.InputSchema->SetObjectField(TEXT("properties"), Properties);

			TArray<TSharedPtr<FJsonValue>> Required;
			Required.Add(MakeShared<FJsonValueString>(TEXT("asset_path")));
			Def.InputSchema->SetArrayField(TEXT("required"), Required);
		}
		Def.Tier = EAgentMcpTier::SafeWrite;
		Def.Handler = FAgentMcpToolHandler::CreateStatic(&HandleCreateMappingContext);
		FAgentMcpToolRegistry::Get().Register(MoveTemp(Def));
	}

	// add_mapping_entry
	{
		FAgentMcpToolDef Def;
		Def.Name = TEXT("add_mapping_entry");
		Def.Description = TEXT("Maps a key to an input action within an existing UInputMappingContext. "
			"Args: context_path (required), action_path (required), key (required — UE key name e.g. SpaceBar, W, LeftMouseButton, Gamepad_FaceButton_Bottom, LeftShift). "
			"Returns {mapped, key, total_mappings}. Use save_asset to persist changes to disk.");
		Def.InputSchema = MakeShared<FJsonObject>();
		Def.InputSchema->SetStringField(TEXT("type"), TEXT("object"));
		{
			TSharedRef<FJsonObject> Properties = MakeShared<FJsonObject>();

			TSharedRef<FJsonObject> ContextProp = MakeShared<FJsonObject>();
			ContextProp->SetStringField(TEXT("type"), TEXT("string"));
			ContextProp->SetStringField(TEXT("description"), TEXT("Absolute asset path of the UInputMappingContext to modify, e.g. /Game/Input/IMC_Default"));
			Properties->SetObjectField(TEXT("context_path"), ContextProp);

			TSharedRef<FJsonObject> ActionProp = MakeShared<FJsonObject>();
			ActionProp->SetStringField(TEXT("type"), TEXT("string"));
			ActionProp->SetStringField(TEXT("description"), TEXT("Absolute asset path of the UInputAction to map, e.g. /Game/Input/IA_Jump"));
			Properties->SetObjectField(TEXT("action_path"), ActionProp);

			TSharedRef<FJsonObject> KeyProp = MakeShared<FJsonObject>();
			KeyProp->SetStringField(TEXT("type"), TEXT("string"));
			KeyProp->SetStringField(TEXT("description"), TEXT("UE key name to bind (e.g. SpaceBar, W, LeftMouseButton, Gamepad_FaceButton_Bottom, LeftShift). Must be a valid EKeys entry."));
			Properties->SetObjectField(TEXT("key"), KeyProp);

			Def.InputSchema->SetObjectField(TEXT("properties"), Properties);

			TArray<TSharedPtr<FJsonValue>> Required;
			Required.Add(MakeShared<FJsonValueString>(TEXT("context_path")));
			Required.Add(MakeShared<FJsonValueString>(TEXT("action_path")));
			Required.Add(MakeShared<FJsonValueString>(TEXT("key")));
			Def.InputSchema->SetArrayField(TEXT("required"), Required);
		}
		Def.Tier = EAgentMcpTier::SafeWrite;
		Def.Handler = FAgentMcpToolHandler::CreateStatic(&HandleAddMappingEntry);
		FAgentMcpToolRegistry::Get().Register(MoveTemp(Def));
	}
}
