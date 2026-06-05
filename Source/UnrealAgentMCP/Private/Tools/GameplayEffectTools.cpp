#include "Tools/GameplayEffectTools.h"

#include "Core/AgentMcpToolRegistry.h"
#include "Core/McpTypes.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "Engine/Blueprint.h"
#include "GameplayEffect.h"
#include "GameplayEffectComponents/TargetTagsGameplayEffectComponent.h"
#include "GameplayTagsManager.h"
#include "Kismet2/BlueprintEditorUtils.h"
#include "ScopedTransaction.h"
#include "Tools/McpToolUtils.h"
#include "Tools/NodeGraphUtils.h"

namespace
{
	using namespace AgentMcp;

	FAgentMcpToolResult HandleSetGeTargetTags(const TSharedPtr<FJsonObject>& Args)
	{
		FString BlueprintPath;
		if (!Args.IsValid() || !Args->TryGetStringField(TEXT("blueprint_path"), BlueprintPath))
		{
			return FAgentMcpToolResult::Error(TEXT("Missing required string argument 'blueprint_path'."));
		}

		const TArray<TSharedPtr<FJsonValue>>* GrantedTagsArr = nullptr;
		if (!Args->TryGetArrayField(TEXT("granted_tags"), GrantedTagsArr) || !GrantedTagsArr || GrantedTagsArr->Num() == 0)
		{
			return FAgentMcpToolResult::Error(TEXT("Missing or empty required array argument 'granted_tags'."));
		}

		// Validate all tags before touching the asset.
		UGameplayTagsManager& TagsManager = UGameplayTagsManager::Get();
		TArray<FGameplayTag> ValidatedTags;
		ValidatedTags.Reserve(GrantedTagsArr->Num());
		for (const TSharedPtr<FJsonValue>& TagVal : *GrantedTagsArr)
		{
			const FString TagStr = TagVal->AsString();
			if (TagStr.IsEmpty())
			{
				return FAgentMcpToolResult::Error(TEXT("'granted_tags' contains an empty string. All tag entries must be non-empty."));
			}
			FGameplayTag Tag = TagsManager.RequestGameplayTag(FName(*TagStr), /*ErrorIfNotFound=*/false);
			if (!Tag.IsValid())
			{
				return FAgentMcpToolResult::Error(FString::Printf(
					TEXT("Tag '%s' is not registered. Only tags present in DefaultGameplayTags.ini or registered via native UE_DEFINE_GAMEPLAY_TAG are accepted."),
					*TagStr));
			}
			ValidatedTags.Add(Tag);
		}

		// Resolve blueprint.
		FString BpError;
		UBlueprint* BP = NodeGraphUtils::ResolveBlueprint(BlueprintPath, BpError);
		if (!BP)
		{
			return FAgentMcpToolResult::Error(BpError);
		}

		// Ensure the generated class CDO is a UGameplayEffect.
		UClass* GenClass = BP->GeneratedClass;
		if (!GenClass || !GenClass->IsChildOf(UGameplayEffect::StaticClass()))
		{
			return FAgentMcpToolResult::Error(FString::Printf(
				TEXT("'%s' does not derive from UGameplayEffect (generated class: %s). Only GameplayEffect blueprints are supported."),
				*BlueprintPath, GenClass ? *GenClass->GetName() : TEXT("<null>")));
		}
		UGameplayEffect* GE = Cast<UGameplayEffect>(GenClass->GetDefaultObject());
		if (!GE)
		{
			return FAgentMcpToolResult::Error(FString::Printf(
				TEXT("Could not get CDO for GameplayEffect class '%s'."), *GenClass->GetName()));
		}

		FScopedTransaction Transaction(NSLOCTEXT("AgentMcp", "SetGeTargetTags", "MCP: Set GE Target Tags"));
		GE->Modify();

		// FindOrAddComponent — adds UTargetTagsGameplayEffectComponent if not present.
		UTargetTagsGameplayEffectComponent& TagsComp = GE->FindOrAddComponent<UTargetTagsGameplayEffectComponent>();

		// Build the FInheritedTagContainer with all requested tags in .Added.
		FInheritedTagContainer TagMods;
		for (const FGameplayTag& Tag : ValidatedTags)
		{
			TagMods.Added.AddTag(Tag);
		}

		TagsComp.SetAndApplyTargetTagChanges(TagMods);

		// Mark blueprint and package dirty.
		FBlueprintEditorUtils::MarkBlueprintAsModified(BP);
		BP->GetOutermost()->MarkPackageDirty();

		// Build response: list the combined tags that are now on the component.
		const FInheritedTagContainer& Configured = TagsComp.GetConfiguredTargetTagChanges();
		TArray<TSharedPtr<FJsonValue>> ResultTagArr;
		for (const FGameplayTag& Tag : Configured.CombinedTags)
		{
			ResultTagArr.Add(MakeShared<FJsonValueString>(Tag.ToString()));
		}
		// Fallback: report Added if CombinedTags is empty (parent-GE scenario where CombinedTags
		// only populates after UpdateInheritedTagProperties with a parent container).
		if (ResultTagArr.IsEmpty())
		{
			for (const FGameplayTag& Tag : Configured.Added)
			{
				ResultTagArr.Add(MakeShared<FJsonValueString>(Tag.ToString()));
			}
		}

		TSharedRef<FJsonObject> Result = MakeShared<FJsonObject>();
		Result->SetBoolField(TEXT("set"), true);
		Result->SetArrayField(TEXT("granted_tags"), ResultTagArr);
		Result->SetStringField(TEXT("component"), UTargetTagsGameplayEffectComponent::StaticClass()->GetName());
		return FAgentMcpToolResult::Success(ToolUtils::SerializeObject(Result));
	}

} // namespace

void AgentMcp::Tools::RegisterGameplayEffectTools()
{
	FAgentMcpToolDef Def;
	Def.Name = TEXT("set_ge_target_tags");
	Def.Description = TEXT(
		"SafeWrite. Sets the 'granted tags' (target tags) on a GameplayEffect Blueprint via "
		"UTargetTagsGameplayEffectComponent. All tags must be already registered (in "
		"DefaultGameplayTags.ini or via native UE_DEFINE_GAMEPLAY_TAG). "
		"Returns {set, granted_tags, component}. "
		"Args: blueprint_path (required, GameplayEffect BP), granted_tags (required, string array of tag names).");
	Def.InputSchema = MakeShared<FJsonObject>();
	Def.InputSchema->SetStringField(TEXT("type"), TEXT("object"));
	{
		TSharedRef<FJsonObject> Props = MakeShared<FJsonObject>();

		TSharedRef<FJsonObject> BpProp = MakeShared<FJsonObject>();
		BpProp->SetStringField(TEXT("type"), TEXT("string"));
		BpProp->SetStringField(TEXT("description"), TEXT("Package path of the GameplayEffect Blueprint, e.g. /Game/AbilitySystem/GE_Damage."));
		Props->SetObjectField(TEXT("blueprint_path"), BpProp);

		TSharedRef<FJsonObject> TagsProp = MakeShared<FJsonObject>();
		TagsProp->SetStringField(TEXT("type"), TEXT("array"));
		{
			TSharedRef<FJsonObject> Items = MakeShared<FJsonObject>();
			Items->SetStringField(TEXT("type"), TEXT("string"));
			TagsProp->SetObjectField(TEXT("items"), Items);
		}
		TagsProp->SetStringField(TEXT("description"), TEXT("Array of registered GameplayTag name strings to grant to the target, e.g. [\"Ability.Cooldown.Fireball\"]."));
		Props->SetObjectField(TEXT("granted_tags"), TagsProp);

		Def.InputSchema->SetObjectField(TEXT("properties"), Props);

		TArray<TSharedPtr<FJsonValue>> Required;
		Required.Add(MakeShared<FJsonValueString>(TEXT("blueprint_path")));
		Required.Add(MakeShared<FJsonValueString>(TEXT("granted_tags")));
		Def.InputSchema->SetArrayField(TEXT("required"), Required);
	}
	Def.Tier = EAgentMcpTier::SafeWrite;
	Def.Handler = FAgentMcpToolHandler::CreateStatic(&HandleSetGeTargetTags);
	FAgentMcpToolRegistry::Get().Register(MoveTemp(Def));
}
